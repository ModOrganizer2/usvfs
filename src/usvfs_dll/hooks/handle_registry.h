#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <utility>

#include <windows_sane.h>

// Maps handles to shared values, for the state the hooks keep per handle.
//
// Erasing is wait-free: it takes no lock, allocates nothing and frees nothing, so that
// NtClose can erase wherever it is called. That is where waiting for another thread
// may never end: under the lock of the process heap, inside ntdll, or while a library
// installing hooks holds every other thread suspended, one of them maybe inside this
// registry.
//
// So an erased entry is not destroyed where it is erased: it joins a lock-free list of
// erased entries, which the next insert or destroyErased destroys. Lookups read under a
// shared lock and the list is taken under the exclusive lock, so that no lookup still
// reads an entry once it is destroyed. Nothing is allocated or freed under the lock,
// so that its holder never waits for the heap.
//
// Entries are indexed by the value of their handle, in a table whose blocks are
// allocated on first use and never freed before the registry. The kernel ignores the
// lowest two bits of a handle value and gives a process at most 2^24 handles, so the
// table has a slot for each of them.
template <class T>
class HandleRegistry
{
public:
  using Value = std::shared_ptr<T>;

  HandleRegistry() { ::InitializeSListHead(&m_erased); }

  ~HandleRegistry()
  {
    const ErasedEntries erased(::InterlockedFlushSList(&m_erased));
    for (std::atomic<Block*>& block : m_blocks) {
      const std::unique_ptr<Block> owned(block.load());
      if (owned) {
        for (std::atomic<Entry*>& slot : owned->slots) {
          delete slot.load();
        }
      }
    }
  }

  HandleRegistry(const HandleRegistry&)            = delete;
  HandleRegistry& operator=(const HandleRegistry&) = delete;

  // Returns the value of a handle, or an empty pointer if it has none.
  Value find(HANDLE handle) const
  {
    std::atomic<Entry*>* slot = existingSlotOf(handle);
    if (!slot) {
      return nullptr;
    }
    std::shared_lock lock(m_mutex);
    const Entry* entry = slot->load();
    return entry ? entry->value : nullptr;
  }

  // Gives a handle a value, replacing the one it has.
  void insert(HANDLE handle, Value value)
  {
    insertUnless(handle, std::move(value), [](const Value&) {
      return false;
    });
  }

  // Gives a handle a value, unless shouldKeep, called under the exclusive lock with the
  // value the handle has, says that the handle keeps it. Returns the value the handle
  // has then, which is the given one for a value no handle has.
  template <class ShouldKeep>
  Value insertUnless(HANDLE handle, Value value, ShouldKeep shouldKeep)
  {
    std::atomic<Entry*>* slot = slotOf(handle);
    if (!slot) {
      return value;
    }
    // Made before the lock, and the entries it replaces destroyed after it. Declared
    // before the lock for the latter.
    std::unique_ptr<Entry> entry(new Entry{{}, std::move(value)});
    ErasedEntries destroyed(nullptr);
    std::unique_lock lock(m_mutex);
    destroyed.add(::InterlockedFlushSList(&m_erased));
    const Entry* current = slot->load();
    if (current && shouldKeep(std::as_const(current->value))) {
      return current->value;
    }
    Value result = entry->value;
    destroyed.add(slot->exchange(entry.release()));
    return result;
  }

  // Erases the value of a handle and returns whether it had one. Wait-free; the value
  // is destroyed later (see HandleRegistry).
  bool erase(HANDLE handle) noexcept
  {
    std::atomic<Entry*>* slot = existingSlotOf(handle);
    if (!slot) {
      return false;
    }
    Entry* entry = slot->exchange(nullptr);
    if (!entry) {
      return false;
    }
    ::InterlockedPushEntrySList(&m_erased, &entry->link);
    return true;
  }

  // Destroys the erased values, unless another thread holds the lock: as it does not
  // wait, the values then wait for the next insert or call.
  void destroyErased()
  {
    // a hint only, as the depth wraps around past 65535 entries on x64
    if (::QueryDepthSList(&m_erased) == 0) {
      return;
    }
    ErasedEntries destroyed(nullptr);
    std::unique_lock lock(m_mutex, std::try_to_lock);
    if (lock.owns_lock()) {
      destroyed.add(::InterlockedFlushSList(&m_erased));
    }
  }

private:
  struct Entry
  {
    // first, so that it has the alignment of the block allocated for the entry, as
    // the list of erased entries requires
    alignas(MEMORY_ALLOCATION_ALIGNMENT) SLIST_ENTRY link;
    Value value;
  };

  // Owns a chain of entries linked by their link, from the list of erased entries or
  // replaced ones, and destroys them.
  class ErasedEntries
  {
  public:
    explicit ErasedEntries(PSLIST_ENTRY chain) : m_chain(chain) {}

    ~ErasedEntries()
    {
      while (m_chain) {
        Entry* entry = CONTAINING_RECORD(m_chain, Entry, link);
        m_chain      = m_chain->Next;
        delete entry;
      }
    }

    ErasedEntries(const ErasedEntries&)            = delete;
    ErasedEntries& operator=(const ErasedEntries&) = delete;

    void add(PSLIST_ENTRY chain)
    {
      if (!chain) {
        return;
      }
      PSLIST_ENTRY last = chain;
      while (last->Next) {
        last = last->Next;
      }
      last->Next = m_chain;
      m_chain    = chain;
    }

    void add(Entry* entry)
    {
      if (entry) {
        entry->link.Next = nullptr;
        add(&entry->link);
      }
    }

  private:
    PSLIST_ENTRY m_chain;
  };

  static constexpr std::size_t SlotCount  = std::size_t{1} << 24;
  static constexpr std::size_t BlockSlots = 4096;

  struct Block
  {
    std::array<std::atomic<Entry*>, BlockSlots> slots{};
  };

  // Returns the slot of a handle, or nothing for a value that no handle has.
  static std::optional<std::size_t> slotIndexOf(HANDLE handle)
  {
    const std::uintptr_t index = reinterpret_cast<std::uintptr_t>(handle) >> 2;
    if (index == 0 || index >= SlotCount) {
      return std::nullopt;
    }
    return index;
  }

  // Returns the slot of a handle if its block exists, nullptr otherwise.
  std::atomic<Entry*>* existingSlotOf(HANDLE handle) const
  {
    const auto index = slotIndexOf(handle);
    if (!index) {
      return nullptr;
    }
    Block* block = m_blocks[*index / BlockSlots].load();
    return block ? &block->slots[*index % BlockSlots] : nullptr;
  }

  // Returns the slot of a handle, allocating its block if it has none yet; nullptr for
  // a value that no handle has. Allocates without the lock: a block that loses the
  // race to another thread's is freed again.
  std::atomic<Entry*>* slotOf(HANDLE handle)
  {
    const auto index = slotIndexOf(handle);
    if (!index) {
      return nullptr;
    }
    std::atomic<Block*>& block = m_blocks[*index / BlockSlots];
    Block* current             = block.load();
    if (!current) {
      auto created = std::make_unique<Block>();
      if (block.compare_exchange_strong(current, created.get())) {
        current = created.release();
      }
    }
    return &current->slots[*index % BlockSlots];
  }

  mutable std::shared_mutex m_mutex;
  std::array<std::atomic<Block*>, SlotCount / BlockSlots> m_blocks{};
  SLIST_HEADER m_erased;
};
