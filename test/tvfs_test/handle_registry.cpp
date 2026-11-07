#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include <windows_sane.h>

#include <hooks/handle_registry.h>
#include <hooks/handle_tracker.h>

#include <gtest/gtest.h>

namespace
{

// Writes `message` and ends the process. Used once a test deadlocked: the stalled
// threads may hold the lock of the process heap and never release it, so neither
// GoogleTest nor the CRT could report the failure, as both allocate.
[[noreturn]] void abortDeadlocked(const char* message)
{
  DWORD bytesWritten = 0;
  ::WriteFile(::GetStdHandle(STD_ERROR_HANDLE), message,
              static_cast<DWORD>(std::strlen(message)), &bytesWritten, nullptr);
  ::TerminateProcess(::GetCurrentProcess(), 3);
  for (;;) {
    ::Sleep(INFINITE);
  }
}

class Event
{
public:
  Event() : m_handle(::CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
  ~Event() { ::CloseHandle(m_handle); }
  Event(const Event&)            = delete;
  Event& operator=(const Event&) = delete;

  void set() { ::SetEvent(m_handle); }

  bool wait(DWORD milliseconds)
  {
    return ::WaitForSingleObject(m_handle, milliseconds) == WAIT_OBJECT_0;
  }

private:
  HANDLE m_handle;
};

// Counts its destructions, to tell when the registry destroys a value.
struct Counted
{
  explicit Counted(int& destroyedCount) : destroyedCount(destroyedCount) {}
  ~Counted() { ++destroyedCount; }

  int& destroyedCount;
};

using Registry = HandleRegistry<Counted>;

HANDLE handleOf(std::uintptr_t value)
{
  return reinterpret_cast<HANDLE>(value);
}

}  // namespace

TEST(HandleRegistryTest, FindsTheValueOfAnInsertedHandle)
{
  int destroyedCount = 0;
  Registry registry;
  const auto value = std::make_shared<Counted>(destroyedCount);
  registry.insert(handleOf(0x1230), value);

  EXPECT_EQ(registry.find(handleOf(0x1230)), value);
  EXPECT_EQ(registry.find(handleOf(0x4560)), nullptr);
}

TEST(HandleRegistryTest, ReplacesTheValueOfAHandleInsertedAgain)
{
  int destroyedCount = 0;
  Registry registry;
  registry.insert(handleOf(0x1230), std::make_shared<Counted>(destroyedCount));
  const auto value = std::make_shared<Counted>(destroyedCount);
  registry.insert(handleOf(0x1230), value);

  EXPECT_EQ(registry.find(handleOf(0x1230)), value);
  EXPECT_EQ(destroyedCount, 1);
}

TEST(HandleRegistryTest, KeepsTheValueThatInsertUnlessIsToldToKeep)
{
  int destroyedCount = 0;
  Registry registry;
  const auto kept = std::make_shared<Counted>(destroyedCount);
  registry.insert(handleOf(0x1230), kept);

  const auto result =
      registry.insertUnless(handleOf(0x1230), std::make_shared<Counted>(destroyedCount),
                            [&](const Registry::Value& current) {
                              return current == kept;
                            });

  EXPECT_EQ(result, kept);
  EXPECT_EQ(registry.find(handleOf(0x1230)), kept);
  EXPECT_EQ(destroyedCount, 1);
}

// The kernel ignores the lowest two bits of a handle value, so both values name the
// same handle.
TEST(HandleRegistryTest, IgnoresTheTagBitsOfAHandle)
{
  int destroyedCount = 0;
  Registry registry;
  const auto value = std::make_shared<Counted>(destroyedCount);
  registry.insert(handleOf(0x1230), value);

  EXPECT_EQ(registry.find(handleOf(0x1233)), value);
  EXPECT_TRUE(registry.erase(handleOf(0x1231)));
  EXPECT_EQ(registry.find(handleOf(0x1230)), nullptr);
}

TEST(HandleRegistryTest, KeepsNothingForValuesThatNoHandleHas)
{
  int destroyedCount = 0;
  Registry registry;
  for (const HANDLE handle :
       {HANDLE{nullptr}, INVALID_HANDLE_VALUE, ::GetCurrentThread(),
        handleOf(std::uintptr_t{1} << 26)}) {
    const auto value = std::make_shared<Counted>(destroyedCount);

    EXPECT_EQ(registry.insertUnless(handle, value,
                                    [](const Registry::Value&) {
                                      return true;
                                    }),
              value);
    EXPECT_EQ(registry.find(handle), nullptr);
    EXPECT_FALSE(registry.erase(handle));
  }
}

TEST(HandleRegistryTest, DestroysAnErasedValueOnlyInDestroyErased)
{
  int destroyedCount = 0;
  Registry registry;
  registry.insert(handleOf(0x1230), std::make_shared<Counted>(destroyedCount));

  EXPECT_TRUE(registry.erase(handleOf(0x1230)));
  EXPECT_FALSE(registry.erase(handleOf(0x1230)));
  EXPECT_EQ(registry.find(handleOf(0x1230)), nullptr);
  EXPECT_EQ(destroyedCount, 0);

  registry.destroyErased();
  EXPECT_EQ(destroyedCount, 1);
}

TEST(HandleRegistryTest, DestroysErasedValuesOnTheNextInsert)
{
  int destroyedCount = 0;
  Registry registry;
  registry.insert(handleOf(0x1230), std::make_shared<Counted>(destroyedCount));
  registry.erase(handleOf(0x1230));

  registry.insert(handleOf(0x4560), std::make_shared<Counted>(destroyedCount));
  EXPECT_EQ(destroyedCount, 1);
}

// The deadlock of a game under pr99-alpha.6: a library installing hooks suspended every
// other thread, one of them while it held the lock of a registry, then closed a thread
// handle of its own, and so waited for the lock.
TEST(HandleRegistryTest, ErasesWhileAnotherThreadHoldsTheLock)
{
  int destroyedCount = 0;
  Registry registry;
  const HANDLE erasedHandle  = handleOf(0x1230);
  const HANDLE writtenHandle = handleOf(0x4560);
  registry.insert(erasedHandle, std::make_shared<Counted>(destroyedCount));
  // given a value, so that insertUnless asks whether to keep it
  registry.insert(writtenHandle, std::make_shared<Counted>(destroyedCount));

  std::atomic<bool> isLockHeld     = false;
  std::atomic<bool> isLockReleased = false;
  // holds the exclusive lock until told to release it, as insertUnless asks whether
  // to keep the value under it
  std::thread writer([&] {
    registry.insertUnless(writtenHandle, std::make_shared<Counted>(destroyedCount),
                          [&](const Registry::Value&) {
                            isLockHeld = true;
                            while (!isLockReleased) {
                              ::SwitchToThread();
                            }
                            return false;
                          });
  });

  Event isEraseDone;
  bool isErased          = false;
  bool isUntrackedErased = true;
  std::thread eraser([&] {
    while (!isLockHeld) {
      ::SwitchToThread();
    }
    isErased          = registry.erase(erasedHandle);
    isUntrackedErased = registry.erase(handleOf(0x7890));
    isEraseDone.set();
  });

  if (!isEraseDone.wait(10000)) {
    abortDeadlocked("HandleRegistry::erase waited for the holder of the lock\n");
  }
  isLockReleased = true;
  eraser.join();
  writer.join();

  EXPECT_TRUE(isErased);
  EXPECT_FALSE(isUntrackedErased);
  EXPECT_EQ(registry.find(erasedHandle), nullptr);
}

// The other way NtClose is called where the caller cannot wait: another thread holds
// the lock of the process heap, which erasing must therefore neither allocate from nor
// free to.
TEST(HandleRegistryTest, ErasesWhileAnotherThreadHoldsTheProcessHeap)
{
  int destroyedCount = 0;
  Registry registry;
  const HANDLE erasedHandle = handleOf(0x1230);
  registry.insert(erasedHandle, std::make_shared<Counted>(destroyedCount));

  Event isEraserRunning;
  Event isHeapLocked;
  Event isEraseDone;
  bool isHeapLockTaken = false;
  bool isErased        = false;

  // running before the heap is locked, as starting a thread allocates, and so may the
  // DLLs that the thread attaches to as it starts
  std::thread eraser([&] {
    isEraserRunning.set();
    isHeapLocked.wait(INFINITE);
    isErased = registry.erase(erasedHandle);
    isEraseDone.set();
  });
  std::thread locker([&] {
    isEraserRunning.wait(INFINITE);
    const HANDLE heap = ::GetProcessHeap();
    isHeapLockTaken   = ::HeapLock(heap) != FALSE;
    isHeapLocked.set();
    isEraseDone.wait(INFINITE);
    if (isHeapLockTaken) {
      ::HeapUnlock(heap);
    }
  });

  if (!isEraseDone.wait(10000)) {
    abortDeadlocked("HandleRegistry::erase waited for the process heap\n");
  }
  eraser.join();
  locker.join();

  ASSERT_TRUE(isHeapLockTaken);
  EXPECT_TRUE(isErased);
  EXPECT_EQ(destroyedCount, 0);
}

// The deadlock of a game under pr99-alpha.5: a thread erasing a handle held the tracker
// lock while freeing into the process heap, whose lock another thread held while it
// closed a handle, and so waited for the tracker lock.
TEST(HandleTrackerTest, ErasesWhileAnInsertWaitsForTheLockedProcessHeap)
{
  HandleTracker tracker;
  const HANDLE insertedHandle = handleOf(0x1230);
  const HANDLE erasedHandle   = handleOf(0x4560);
  tracker.insert(erasedHandle, usvfs::UnicodeString(L"erased"));

  // Longer than the low-fragmentation heap serves, so that copying it takes the lock
  // of the process heap rather than a lock-free block.
  const usvfs::UnicodeString path(std::wstring(20000, L'x'));

  Event isInserterRunning;
  Event isHeapLocked;
  Event isInsertStarting;
  Event isEraseDone;
  const HANDLE heap    = ::GetProcessHeap();
  bool isHeapLockTaken = false;

  // running before the heap is locked, as starting a thread allocates, and so may the
  // DLLs that the thread attaches to as it starts
  std::thread inserter([&] {
    isInserterRunning.set();
    isHeapLocked.wait(INFINITE);
    isInsertStarting.set();
    tracker.insert(insertedHandle, path);
  });
  std::thread eraser([&] {
    isInserterRunning.wait(INFINITE);
    isHeapLockTaken = ::HeapLock(heap) != FALSE;
    isHeapLocked.set();
    isInsertStarting.wait(INFINITE);
    // gives the insert the time to wait for the heap, under the tracker lock if it
    // copies the path there
    ::Sleep(200);
    tracker.erase(erasedHandle);
    isEraseDone.set();
    if (isHeapLockTaken) {
      ::HeapUnlock(heap);
    }
  });

  if (!isEraseDone.wait(10000)) {
    abortDeadlocked("HandleTracker::erase deadlocked with an insert waiting for the "
                    "process heap\n");
  }
  eraser.join();
  inserter.join();

  ASSERT_TRUE(isHeapLockTaken);
  EXPECT_EQ(tracker.lookup(insertedHandle).size(), path.size());
  EXPECT_EQ(tracker.lookup(erasedHandle).size(), 0);
}
