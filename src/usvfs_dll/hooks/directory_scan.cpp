#include "directory_scan.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <boost/filesystem.hpp>

#include <loghelpers.h>
#include <stringcast.h>
#include <stringutils.h>
#include <unicodestring.h>

#include "../hookcontext.h"

#include "directory_records.h"
#include "handle_registry.h"
#include "handle_tracker.h"

namespace ulog = usvfs::log;
namespace ush  = usvfs::shared;
namespace bfs  = boost::filesystem;

using usvfs::UnicodeString;

// File names compared without regard to case, as shadowing and deduplication
// compare them. NTFS and ReFS fold a name one UTF-16 code unit at a time, through the
// upcase table of the volume, which leaves a surrogate pair as it is; LCMapStringW
// folds some characters beyond the BMP, and so would merge names that the volume
// keeps apart. The names are folded while hashed and compared, so that a lookup does
// not allocate.
class CaseInsensitiveNameSet
{
public:
  // Returns whether the name was not in the set yet.
  bool insert(std::wstring_view name) { return m_names.emplace(name).second; }

  bool contains(std::wstring_view name) const { return m_names.contains(name); }

private:
  // Folds ASCII, which most names are made of, without the call to
  // RtlUpcaseUnicodeChar, which folds it the same.
  static WCHAR fold(WCHAR c)
  {
    if (c >= 0x80) {
      return RtlUpcaseUnicodeChar(c);
    }
    return c >= L'a' && c <= L'z' ? static_cast<WCHAR>(c - (L'a' - L'A')) : c;
  }

  // FNV-1a over the folded code units
  struct FoldedHash
  {
    using is_transparent = void;

    size_t operator()(std::wstring_view name) const
    {
      std::uint64_t hash = 0xcbf29ce484222325;
      for (const WCHAR c : name) {
        hash = (hash ^ fold(c)) * 0x100000001b3;
      }
      return static_cast<size_t>(hash);
    }
  };

  struct FoldedEqual
  {
    using is_transparent = void;

    bool operator()(std::wstring_view lhs, std::wstring_view rhs) const
    {
      return std::ranges::equal(lhs, rhs, {}, fold, fold);
    }
  };

  std::unordered_set<std::wstring, FoldedHash, FoldedEqual> m_names;
};

// Marks the calling thread as doing I/O of the scan itself while it lives: the
// NtOpenFile hook then passes the calls straight to ntdll, so that the directories the
// scan opens are neither rerouted nor tracked, and opening them takes no lock of the
// hook context. A count rather than a flag, so that a scope nested in another one does
// not end it.
//
// The count is kept in a Win32 TLS slot rather than in a thread_local, as the
// trampolines and the hook stack keep their state: the hooks run on every thread of
// the process, and a slot lives in the TEB, which every thread has, whereas the
// implicit TLS block of the DLL exists only on threads the loader initialized.
class InternalIoScope
{
public:
  InternalIoScope()
  {
    // logged here rather than where the slot is allocated, as the DLL loads before
    // the loggers are set up
    if (s_slot == TLS_OUT_OF_INDEXES) {
      std::call_once(s_isSlotFailureLogged, [] {
        spdlog::get("hooks")->error("no TLS slot is left to mark the I/O of directory "
                                    "scans, which therefore goes through the hooks");
      });
    }
    setDepth(depth() + 1);
  }

  ~InternalIoScope() { setDepth(depth() - 1); }

  InternalIoScope(const InternalIoScope&)            = delete;
  InternalIoScope& operator=(const InternalIoScope&) = delete;

  static bool isActive() { return depth() > 0; }

private:
  static std::uintptr_t depth()
  {
    // TlsGetValue resets the last error, which the hooks leave as their caller set it
    PreserveGetLastError tlsGetValueChangesGetLastError;
    return reinterpret_cast<std::uintptr_t>(::TlsGetValue(s_slot));
  }

  static void setDepth(std::uintptr_t depth)
  {
    ::TlsSetValue(s_slot, reinterpret_cast<LPVOID>(depth));
  }

  static const DWORD s_slot;
  static std::once_flag s_isSlotFailureLogged;
};

// allocated as the DLL loads rather than on first use, as a function-local static
// would guard its initialization through implicit TLS
const DWORD InternalIoScope::s_slot = ::TlsAlloc();
std::once_flag InternalIoScope::s_isSlotFailureLogged;

bool isInternalDirectoryIo()
{
  return InternalIoScope::isActive();
}

// A handle the scan opened itself. Move-only; closes the handle when destroyed, so
// that a scan replaced by a restart or dropped by NtClose does not leak it.
class OwnedHandle
{
public:
  OwnedHandle() = default;
  explicit OwnedHandle(HANDLE handle) : m_handle(handle) {}

  OwnedHandle(OwnedHandle&& other) noexcept
      : m_handle(std::exchange(other.m_handle, INVALID_HANDLE_VALUE))
  {}

  OwnedHandle& operator=(OwnedHandle&& other) noexcept
  {
    if (this != &other) {
      reset();
      m_handle = std::exchange(other.m_handle, INVALID_HANDLE_VALUE);
    }
    return *this;
  }

  ~OwnedHandle() { reset(); }

  HANDLE get() const { return m_handle; }
  bool isValid() const { return isValidHandle(m_handle); }

  // Closes the handle, if there is one.
  void reset()
  {
    if (isValid()) {
      if (!::CloseHandle(m_handle)) {
        spdlog::get("hooks")->warn("failed to close handle {0:x}: {1}",
                                   reinterpret_cast<std::uintptr_t>(m_handle),
                                   ::GetLastError());
      }
    }
    m_handle = INVALID_HANDLE_VALUE;
  }

private:
  HANDLE m_handle = INVALID_HANDLE_VALUE;
};

// A directory the scan opened, or why it could not.
struct OpenedDirectory
{
  // invalid if the open failed
  OwnedHandle handle;
  NTSTATUS status;
};

// Opens a directory, named by a DOS path or one in the \??\ form, for synchronous
// enumeration by the scan itself, with the usvfs hooks suppressed.
//
// The legacy implementation opened mapping source directories with CreateFileW
// without suppressing the hooks, so every internal directory open re-entered the
// NtCreateFile and attribute hooks of usvfs and incurred substantial redundant path
// rerouting and lock overhead.
//
// Suppressing the hooks means that a mapping source directory that only exists
// virtually (a chained virtual-to-virtual mapping) is no longer redirected a second
// time during directory enumeration. However, the file-open hooks only perform a
// single-hop reroute anyway, so such second-hop virtual files could never actually be
// opened (ERROR_PATH_NOT_FOUND); preserving that accidental enumeration behavior has
// little practical value while significantly hurting performance for normal physical
// source directories.
OpenedDirectory openScanDirectory(const std::wstring& path)
{
  // keeps a path in the \??\ form as it is
  UNICODE_STRING ntPath{};
  const NTSTATUS conversion = RtlDosPathNameToRelativeNtPathName_U_WithStatus(
      path.c_str(), &ntPath, nullptr, nullptr);
  if (conversion < 0) {
    return {OwnedHandle(), conversion};
  }
  // case-insensitive, as CreateFileW opens, since the tree may spell a directory in
  // another case than the disk does
  OBJECT_ATTRIBUTES attributes{sizeof(OBJECT_ATTRIBUTES), nullptr, &ntPath,
                               OBJ_CASE_INSENSITIVE,      nullptr, nullptr};
  IO_STATUS_BLOCK status;
  HANDLE handle = INVALID_HANDLE_VALUE;
  NTSTATUS result;
  {
    const InternalIoScope internal;
    // shares delete access, as FindFirstFileExW does: the scan holds the directory open
    // across queries, which must neither keep others from renaming or deleting it nor
    // fail where another handle holds it with DELETE access
    result = NtOpenFile(&handle, FILE_LIST_DIRECTORY | SYNCHRONIZE, &attributes,
                        &status, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT |
                            FILE_OPEN_FOR_BACKUP_INTENT);
  }
  HeapFree(GetProcessHeap(), 0, ntPath.Buffer);
  return {OwnedHandle(result >= 0 ? handle : INVALID_HANDLE_VALUE), result};
}

// Where the regular entries of a directory in the redirection tree are read from.
struct RegularSource
{
  // the physical directory, or invalid for the caller's handle
  OwnedHandle handle;
  // Whether only "." and ".." of the caller's handle are read: the directory holds
  // nothing on disk, so the open hook rerouted that handle to a mapping source, whose
  // other entries are not the directory's. Its dots may stand for the directory, whose
  // information the hooks take from the mapping source anyway.
  bool isDotsOnly;
};

// in the order NTFS and ReFS return them: both or neither, and before any entry of the
// directory index, as they make them up rather than store them
constexpr std::array<std::wstring_view, 2> DOT_ENTRIES{L".", L".."};

// Reads the regular entries of the scanned directory from ntdll and writes those that
// no virtual entry shadows.
class RegularEntryReader
{
public:
  // Reads the source with the pattern of the scan, or every entry without one. The
  // shadowed names are those of the virtual entries, which take precedence;
  // gatherShadowedNames returns them, and is called once an entry other than "." and
  // ".." comes, as a directory that holds nothing else on disk needs none. A source of
  // the dots only ends the entries with the dots. A reader for a restarted scan
  // restarts the native scan with its first read.
  RegularEntryReader(RegularSource source, std::optional<std::wstring_view> pattern,
                     std::function<CaseInsensitiveNameSet()> gatherShadowedNames,
                     bool isRestart)
      : m_source(std::move(source.handle)),
        m_gatherShadowedNames(std::move(gatherShadowedNames)),
        m_isRestartPending(isRestart)
  {
    if (pattern) {
      m_pattern.emplace(pattern->data(), pattern->size());
    }
    if (source.isDotsOnly) {
      m_pendingDots = DOT_ENTRIES;
    }
  }

  // Writes the entries of the next reads, reading only while the output is empty.
  // Returns STATUS_SUCCESS once it wrote some, STATUS_BUFFER_OVERFLOW once it wrote
  // the partial record of a read, STATUS_NO_MORE_FILES once the entries are done, and
  // the error of a read that fails otherwise.
  NTSTATUS fill(const DirectoryQuery& query, DirectoryOutput& output);

private:
  void appendListedRecords(const DirectoryRecordLayout& layout,
                           std::span<const std::byte> data, DirectoryOutput& output);
  bool isShadowed(std::wstring_view name);
  bool isNextDot(const DirectoryRecord& record) const;
  bool areDotsDone() const { return m_pendingDots && m_pendingDots->empty(); }

  OwnedHandle m_source;
  std::optional<UnicodeString> m_pattern;
  std::function<CaseInsensitiveNameSet()> m_gatherShadowedNames;
  // Only these are left out: ntdll lists every entry once, and names that differ in
  // case only are different entries in a case-sensitive directory.
  std::optional<CaseInsensitiveNameSet> m_shadowedNames;
  // the dots a source of the dots only has yet to give, empty once they are done or
  // another entry came; nothing for a source of every entry
  std::optional<std::span<const std::wstring_view>> m_pendingDots;
  bool m_isRestartPending;
  // where a read goes before its entries are filtered; vector storage is aligned for
  // every record, as operator new aligns to 8 bytes or more
  std::vector<std::byte> m_staging;
};

NTSTATUS RegularEntryReader::fill(const DirectoryQuery& query, DirectoryOutput& output)
{
  // what follows the dots belongs to the mapping source, not to the directory
  if (areDotsDone()) {
    return STATUS_NO_MORE_FILES;
  }
  // queryDirectory forwards the classes that have no layout
  const DirectoryRecordLayout layout =
      findDirectoryRecordLayout(query.informationClass).value();
  const HANDLE source = m_source.isValid() ? m_source.get() : query.handle;
  // As large as the buffer of the query, so that a read returns what a read into that
  // buffer would, and every record it returns fits there too. Reading ahead in larger
  // reads would save system calls on small queries (FindNextFile asks for 4 KB), but
  // noticeably only for large directories on disk; and a query that changes the
  // information class would then find records of the old class read past a native
  // cursor that cannot go back, while a read ahead through the caller's handle would
  // move its cursor past what the caller got.
  m_staging.resize(query.length);

  while (output.isEmpty()) {
    // The handle completes synchronously and the caller's notifications belong to
    // the caller's query, so this read asks for none. A restart is passed on, as the
    // native scan of the caller's handle would go on from where it was otherwise.
    // The pattern of the scan is passed on every read: the first read of a handle
    // latches it, which a reopened source needs, and ntdll ignores it otherwise.
    IO_STATUS_BLOCK status;
    const NTSTATUS result = NtQueryDirectoryFile(
        source, nullptr, nullptr, nullptr, &status, m_staging.data(), query.length,
        query.informationClass, query.isSingleEntry(),
        m_pattern ? static_cast<PUNICODE_STRING>(*m_pattern) : nullptr,
        std::exchange(m_isRestartPending, false));
    // ntdll cuts a first record that does not fit short, and returns it again on
    // every query until a buffer holds it, so the regular entries do not end here.
    // Its name may be cut short too, which leaves no telling whether a virtual entry
    // shadows it: it is passed on either way, and filtered once it comes whole.
    if (result == STATUS_BUFFER_OVERFLOW) {
      const std::span<const std::byte> partial =
          std::span(m_staging).first(status.Information);
      if (m_pendingDots && !isNextDot(DirectoryRecord(layout, partial))) {
        m_pendingDots.emplace();
        return STATUS_NO_MORE_FILES;
      }
      output.mirror(partial);
      return result;
    }
    // a pattern that matches nothing ends the entries on the first read already
    if (result == STATUS_NO_MORE_FILES || result == STATUS_NO_SUCH_FILE) {
      return STATUS_NO_MORE_FILES;
    }
    if (result != STATUS_SUCCESS) {
      return result;
    }
    // NTFS, ReFS and FAT cut a first record short on every query, but the contract of
    // NtQueryDirectoryFile lets a later query that holds no whole record succeed with
    // no bytes, for the caller to retry with a larger buffer; another read into this
    // one would return the same, so the caller gets it as it came.
    if (status.Information == 0) {
      return STATUS_SUCCESS;
    }

    appendListedRecords(layout, std::span(m_staging).first(status.Information), output);
    if (areDotsDone()) {
      return output.isEmpty() ? STATUS_NO_MORE_FILES : STATUS_SUCCESS;
    }
  }
  return STATUS_SUCCESS;
}

// Writes the records of a read that the directory lists: for a source of the dots only,
// the dots up to the first other entry; for a source of every entry, those that no
// virtual entry shadows. Every record written fits, since the records before it in the
// output are those before it in the read, or fewer.
void RegularEntryReader::appendListedRecords(const DirectoryRecordLayout& layout,
                                             std::span<const std::byte> data,
                                             DirectoryOutput& output)
{
  while (!data.empty()) {
    const DirectoryRecord record(layout, data);
    if (m_pendingDots) {
      if (!isNextDot(record)) {
        m_pendingDots.emplace();
        return;
      }
      m_pendingDots = m_pendingDots->subspan(1);
      output.append(record);
      if (m_pendingDots->empty()) {
        return;
      }
    } else if (!isShadowed(record.fileNameView())) {
      output.append(record);
    }
    if (record.nextEntryOffset() == 0) {
      return;
    }
    data = data.subspan(record.nextEntryOffset());
  }
}

// Returns whether a record, whole or cut short, is that of the next dot. The length the
// record states tells a ".." that ntdll cut short, as the smallest buffer of
// FileBothDirectoryInformation holds one character of a name; another two-character
// name cut short to "." cannot come there, as ".." directly follows ".".
bool RegularEntryReader::isNextDot(const DirectoryRecord& record) const
{
  const std::wstring_view dot = m_pendingDots->front();
  return record.fileNameLength() == dot.size() * sizeof(WCHAR) &&
         dot.starts_with(record.fileNameView());
}

bool RegularEntryReader::isShadowed(std::wstring_view name)
{
  // no virtual entry is named so, as no node of the redirection tree is
  if (name == L"." || name == L"..") {
    return false;
  }
  if (!m_shadowedNames) {
    m_shadowedNames = m_gatherShadowedNames();
  }
  return m_shadowedNames->contains(name);
}

// Reads the records of a directory through a handle of its own, for a query that
// usvfs makes itself rather than one of the caller. Closes the handle when destroyed.
class NativeDirectoryReader
{
public:
  // Scans directory for the entries that match pattern, or for all of them without
  // one, in the information class of layout. Reads into buffer, which it sizes to
  // hold a record with the longest name there is; a record returned stays valid in
  // the buffer until the reader reads again.
  NativeDirectoryReader(OwnedHandle directory, const DirectoryRecordLayout& layout,
                        std::optional<std::wstring_view> pattern,
                        std::vector<std::byte>& buffer);

  // Returns the next record of the scan, or nothing once the scan ends.
  std::optional<DirectoryRecord> next();
  // what ended the scan, STATUS_SUCCESS while it goes on
  NTSTATUS endStatus() const { return m_endStatus; }

private:
  OwnedHandle m_directory;
  DirectoryRecordLayout m_layout;
  std::optional<UnicodeString> m_pattern;
  std::vector<std::byte>& m_buffer;
  // the records of the last read that next has not returned yet
  std::span<const std::byte> m_records;
  NTSTATUS m_endStatus = STATUS_SUCCESS;
};

NativeDirectoryReader::NativeDirectoryReader(OwnedHandle directory,
                                             const DirectoryRecordLayout& layout,
                                             std::optional<std::wstring_view> pattern,
                                             std::vector<std::byte>& buffer)
    : m_directory(std::move(directory)), m_layout(layout), m_buffer(buffer)
{
  if (pattern) {
    m_pattern.emplace(pattern->data(), pattern->size());
  }
  m_buffer.resize(m_layout.fileNameOffset + UNICODE_STRING_MAX_BYTES);
}

std::optional<DirectoryRecord> NativeDirectoryReader::next()
{
  if (m_records.empty()) {
    if (m_endStatus != STATUS_SUCCESS) {
      return std::nullopt;
    }
    IO_STATUS_BLOCK status;
    const NTSTATUS result = NtQueryDirectoryFile(
        m_directory.get(), nullptr, nullptr, nullptr, &status, m_buffer.data(),
        static_cast<ULONG>(m_buffer.size()), m_layout.informationClass, FALSE,
        m_pattern ? static_cast<PUNICODE_STRING>(*m_pattern) : nullptr, FALSE);
    if (result != STATUS_SUCCESS) {
      m_endStatus = result;
      return std::nullopt;
    }
    m_records = std::span(m_buffer).first(status.Information);
  }
  const DirectoryRecord record(m_layout, m_records);
  m_records = record.nextEntryOffset() == 0
                  ? std::span<const std::byte>()
                  : m_records.subspan(record.nextEntryOffset());
  return record;
}

// A directory that virtual entries map files or directories of. Read the first time an
// entry needs it, keeping the records of what the entries map, so that it is read
// once rather than once per entry, and its handle closes before the query that reads
// it returns. Read whole, or queried by name when it has one file mapped: a mapping
// of one file out of a large directory, such as C:\Windows, would read it all.
class SourceDirectory
{
public:
  explicit SourceDirectory(std::wstring path) : m_path(std::move(path)) {}

  // Adds the name of a file or directory that an entry maps.
  void addFileName(std::wstring fileName) { m_fileNames.insert(std::move(fileName)); }

  // Returns the record of a file or directory in the information class of layout, or
  // nothing when it cannot be read. Reads through buffer, as NativeDirectoryReader
  // does; the record stays valid until the next call.
  std::optional<DirectoryRecord> find(const std::wstring& fileName,
                                      const DirectoryRecordLayout& layout,
                                      std::vector<std::byte>& buffer);

private:
  // Reads the directory and keeps the records of the file names.
  void read(const DirectoryRecordLayout& layout, std::vector<std::byte>& buffer);
  // Queries the directory for a file name.
  std::optional<DirectoryRecord> query(const std::wstring& fileName,
                                       const DirectoryRecordLayout& layout,
                                       std::vector<std::byte>& buffer);
  void keep(std::wstring fileName, const DirectoryRecord& record);

  // where a kept record lies in m_records
  struct RecordPlace
  {
    size_t offset;
    size_t size;
  };

  std::wstring m_path;
  std::unordered_set<std::wstring> m_fileNames;
  // the information class of the records kept, nothing before the first read
  std::optional<FILE_INFORMATION_CLASS> m_recordClass;
  // the records kept, one after another, without the padding between them
  std::vector<std::byte> m_records;
  std::unordered_map<std::wstring, RecordPlace> m_recordPlaces;
};

std::optional<DirectoryRecord>
SourceDirectory::find(const std::wstring& fileName, const DirectoryRecordLayout& layout,
                      std::vector<std::byte>& buffer)
{
  if (m_recordClass != layout.informationClass) {
    read(layout, buffer);
  }
  if (const auto place = m_recordPlaces.find(fileName); place != m_recordPlaces.end()) {
    return DirectoryRecord(
        layout, std::span(m_records).subspan(place->second.offset, place->second.size));
  }
  if (m_fileNames.size() == 1) {
    // queried by name already
    return std::nullopt;
  }
  // The file is missing, or the mapping names it in another case or by its 8.3 name,
  // which a query by name leaves to the filesystem to match.
  return query(fileName, layout, buffer);
}

void SourceDirectory::read(const DirectoryRecordLayout& layout,
                           std::vector<std::byte>& buffer)
{
  // set before the read, so that a directory that fails to read is not read again
  // for every entry
  m_recordClass = layout.informationClass;
  m_records.clear();
  m_recordPlaces.clear();
  if (m_fileNames.size() == 1) {
    const std::wstring& fileName = *m_fileNames.begin();
    if (const std::optional<DirectoryRecord> record = query(fileName, layout, buffer)) {
      keep(fileName, *record);
    }
    return;
  }
  OpenedDirectory directory = openScanDirectory(m_path);
  if (!directory.handle.isValid()) {
    spdlog::get("hooks")->warn(
        "failed to open {0}, which virtual entries map files of: {1:#x}",
        ush::string_cast<std::string>(m_path, ush::CodePage::UTF8),
        static_cast<uint32_t>(directory.status));
    return;
  }
  NativeDirectoryReader reader(std::move(directory.handle), layout, std::nullopt,
                               buffer);
  while (const std::optional<DirectoryRecord> record = reader.next()) {
    std::wstring fileName = record->fileName();
    if (m_fileNames.contains(fileName)) {
      keep(std::move(fileName), *record);
      // Every file name has its record: the rest of the directory, which may be large,
      // holds nothing the entries map. Returns rather than breaks, as the scan has not
      // reached the end the status below reports.
      if (m_recordPlaces.size() == m_fileNames.size()) {
        return;
      }
    }
  }
  if (reader.endStatus() != STATUS_NO_MORE_FILES) {
    spdlog::get("hooks")->warn(
        "failed to list {0}, which virtual entries map files of: {1:#x}",
        ush::string_cast<std::string>(m_path, ush::CodePage::UTF8),
        static_cast<uint32_t>(reader.endStatus()));
  }
}

std::optional<DirectoryRecord>
SourceDirectory::query(const std::wstring& fileName,
                       const DirectoryRecordLayout& layout,
                       std::vector<std::byte>& buffer)
{
  const std::string filePath =
      ush::string_cast<std::string>(m_path + L"\\" + fileName, ush::CodePage::UTF8);
  OpenedDirectory directory = openScanDirectory(m_path);
  if (!directory.handle.isValid()) {
    spdlog::get("hooks")->warn(
        "failed to open the directory of {0}, which a virtual entry maps: {1:#x}",
        filePath, static_cast<uint32_t>(directory.status));
    return std::nullopt;
  }
  NativeDirectoryReader reader(
      std::move(directory.handle), layout,
      fileName != L"." ? std::optional<std::wstring_view>(fileName) : std::nullopt,
      buffer);
  // the pattern is the name of the file, so the scan starts with its record
  const std::optional<DirectoryRecord> record = reader.next();
  if (!record) {
    spdlog::get("hooks")->warn("failed to read {0}, which a virtual entry maps: {1:#x}",
                               filePath, static_cast<uint32_t>(reader.endStatus()));
  }
  return record;
}

void SourceDirectory::keep(std::wstring fileName, const DirectoryRecord& record)
{
  const std::span<const std::byte> bytes = record.bytes();
  m_recordPlaces.try_emplace(std::move(fileName),
                             RecordPlace{m_records.size(), bytes.size()});
  m_records.insert(m_records.end(), bytes.begin(), bytes.end());
}

// A virtual entry of the scanned directory.
struct VirtualMatch
{
  // full path to where the file/directory actually is
  std::wstring realPath;
  // virtual filename (only filename since it has to be within the searched
  // directory)
  // this is left empty when a folder with all its content is mapped to the
  // search directory
  std::wstring virtualName;
};

// Reads each virtual entry from the directory its file or directory lies in, and
// writes it under its virtual name.
class VirtualEntryReader
{
public:
  VirtualEntryReader() = default;
  explicit VirtualEntryReader(const std::vector<VirtualMatch>& matches);

  // Writes the entries left to list, as many as the output takes; the first that does
  // not fit stays for the next call. Skips an entry whose record cannot be read or
  // whose name is written already. Returns STATUS_SUCCESS once it wrote some,
  // STATUS_BUFFER_OVERFLOW once it wrote the partial record of one, and
  // STATUS_NO_MORE_FILES when none is left.
  NTSTATUS fill(const DirectoryQuery& query, DirectoryOutput& output);

  // the virtual names of all the entries, listed or not
  CaseInsensitiveNameSet names() const
  {
    CaseInsensitiveNameSet names;
    for (const Entry& entry : m_entries) {
      names.insert(entry.virtualName);
    }
    return names;
  }

private:
  // A file or directory of a source directory, listed under its virtual name.
  struct Entry
  {
    // index into m_sources
    size_t source;
    std::wstring fileName;
    // empty for an entry listed under the name of its file
    std::wstring virtualName;
  };

  // a deque, as a vector that grows copies them: the move constructors of the MSVC
  // unordered containers allocate, so they are not noexcept
  std::deque<SourceDirectory> m_sources;
  // in the order of the matches, which is the order they are listed in
  std::vector<Entry> m_entries;
  // index of the next entry to try; the ones before it are done
  size_t m_nextEntry = 0;
  // the names of the entries written, so that none is written twice; none while the
  // redirection tree keeps them apart already (see the constructor)
  std::optional<CaseInsensitiveNameSet> m_names;
  // where the source directories read, aligned as the regular reader's staging area.
  // A read holds a record whole, whatever the length of its name: the entry goes out
  // under its virtual name, whose length alone decides whether it fits the query.
  std::vector<std::byte> m_staging;
};

VirtualEntryReader::VirtualEntryReader(const std::vector<VirtualMatch>& matches)
{
  std::unordered_map<std::wstring, size_t> sourceIndexes;
  for (const VirtualMatch& match : matches) {
    // the path of a directory mapped with all its content ends with a separator,
    // which leaves "." as its file name
    bfs::path fullPath(match.realPath);
    if (fullPath.filename().wstring() == L".") {
      fullPath = fullPath.parent_path();
    }
    std::wstring sourcePath = fullPath.parent_path().wstring();
    if (sourcePath.length() >= MAX_PATH &&
        !ush::startswith(sourcePath.c_str(), LR"(\\?\)")) {
      sourcePath = LR"(\\?\)" + sourcePath;
    }
    const auto [sourceIndex, isNew] =
        sourceIndexes.try_emplace(sourcePath, m_sources.size());
    if (isNew) {
      m_sources.emplace_back(sourcePath);
    }
    std::wstring fileName = fullPath.filename().wstring();
    m_sources[sourceIndex->second].addFileName(fileName);
    m_entries.push_back({sourceIndex->second, std::move(fileName), match.virtualName});
  }

  // The tree compares the names of its nodes folding the ASCII letters, as
  // RtlUpcaseUnicodeChar folds them, so no two virtual names that are all ASCII name
  // one file. It folds no other letter, so names that differ only in the case of one
  // (U+00C4 and U+00E4, say) are nodes of their own; and an entry without a virtual
  // name goes out under the name of its file, which the tree does not hold.
  const bool areAllAscii = std::ranges::all_of(m_entries, [](const Entry& entry) {
    return !entry.virtualName.empty() &&
           std::ranges::all_of(entry.virtualName, [](WCHAR c) {
             return c < 0x80;
           });
  });
  if (!areAllAscii) {
    m_names.emplace();
  }
}

NTSTATUS VirtualEntryReader::fill(const DirectoryQuery& query, DirectoryOutput& output)
{
  // queryDirectory forwards the classes that have no layout
  const DirectoryRecordLayout layout =
      findDirectoryRecordLayout(query.informationClass).value();
  // the skipped entries are dropped as the stock code dropped them; a single-entry
  // query stops before reading an entry it would not take
  for (; m_nextEntry < m_entries.size() && !output.isSatisfied(); ++m_nextEntry) {
    const Entry& entry = m_entries[m_nextEntry];
    const std::optional<DirectoryRecord> record =
        m_sources[entry.source].find(entry.fileName, layout, m_staging);
    if (!record) {
      continue;
    }
    // the records of a source directory lie in a buffer of its own, aligned for WCHAR
    const std::wstring_view name =
        entry.virtualName.empty() ? record->fileNameView() : entry.virtualName;
    if (m_names && m_names->contains(name)) {
      continue;
    }
    const bool isWritten = output.appendRenamed(*record, name);
    // like ntdll, an entry that does not fit stays for the next call to return, be it
    // after its partial record or after the entries the output holds
    if (!isWritten) {
      return output.isOverflowed() ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
    }
    if (m_names) {
      m_names->insert(name);
    }
  }
  return output.isEmpty() ? STATUS_NO_MORE_FILES : STATUS_SUCCESS;
}

// The state of one scan: the queries on a handle from its first query or restart on.
class DirectoryScan
{
public:
  // Starts the scan: picks the directory the regular entries are read from and
  // gathers the virtual entries matching the pattern of the scan. The first query of
  // a handle starts its first scan, with replaced null; a restart starts the others,
  // replacing the scan before it.
  DirectoryScan(const DirectoryQuery& query, const DirectoryScan* replaced);

  // Answers a query with the next entries of the scan. Like ntdll, reports the
  // result both as the return value and in the status block. Queries on the same
  // scan run one at a time, as ntdll runs them on a synchronous handle.
  NTSTATUS query(const DirectoryQuery& query);

  // Whether the current hook context built the scan. One that replaces another may map
  // other files, while the scan lists those of the one before; the stock code kept its
  // scans in the context, which dropped them with it.
  bool isFromCurrentContext() const
  {
    return m_contextGeneration == usvfs::HookContext::generation();
  }

private:
  NTSTATUS fill(const DirectoryQuery& query, DirectoryOutput& output);

  // taken before the tree is read, so that a context replaced meanwhile leaves the scan
  // outdated rather than current
  const std::uint64_t m_contextGeneration;
  // whether the queries go straight to ntdll; set only by the constructor, before the
  // scan is shared, so read without the lock
  bool m_isPassthrough = true;
  // the pattern the scan lists with, std::nullopt for every entry (see latchPattern);
  // fixed at construction, so the restart replacing the scan reads it unlocked
  const std::optional<std::wstring> m_pattern;

  // Guards the state below. Taken after the registry lock is released; nested
  // directory queries made while holding it go straight to ntdll, so it is never
  // taken twice by one thread.
  std::mutex m_mutex;
  // has a value while the scan lists the regular entries, before the virtual ones
  std::optional<RegularEntryReader> m_regular;
  VirtualEntryReader m_virtual;
  // An empty result is STATUS_NO_SUCH_FILE on the first query of a handle only, as
  // NTFS and ReFS tell it by whether the handle has a pattern yet, which a restart
  // does not drop.
  bool m_isFirstCall;
};

// The scans of all directory handles. Erasing a scan leaves it to the next query, or
// to destroyErased, to destroy (see HandleRegistry), as NtClose erases.
class DirectoryScans
{
public:
  // Returns the scan serving a query: a new one on the first query of a handle, on a
  // restart, or once the hook context that built the current one is replaced; the
  // current one otherwise. A scan replacing an outdated one takes over its pattern, as
  // a restart does, but leaves the native cursor of the caller's handle where it is,
  // so its regular entries start over only where it reads them through its own handle.
  std::shared_ptr<DirectoryScan> scanFor(const DirectoryQuery& query)
  {
    std::shared_ptr<DirectoryScan> current = m_scans.find(query.handle);
    if (current && !query.isRestart() && current->isFromCurrentContext()) {
      return current;
    }
    if (current && !query.isRestart()) {
      // logged as info, as contexts are rarely replaced, and the regular entries the
      // handle listed already may come again
      spdlog::get("hooks")->info(
          "rebuilding the directory scan of handle {0:#x} for a new hook context",
          reinterpret_cast<std::uintptr_t>(query.handle));
    }
    // tradeoff time: we store this search status even if no virtual results
    // were found. This causes a little extra cost here and in NtClose every
    // time a non-virtual dir is being searched. However if we don't,
    // whenever NtQueryDirectoryFile is called another time on the same handle,
    // this (expensive) construction would be run again.
    //
    // Constructed without the lock of the registry, since construction takes the hook
    // context lock and reenters hooks.
    auto scan = std::make_shared<DirectoryScan>(query, current.get());
    // concurrent first queries on one handle, or concurrent replacements of an outdated
    // scan: the first scan inserted wins
    return m_scans.insertUnless(query.handle, std::move(scan),
                                [&](const std::shared_ptr<DirectoryScan>& inserted) {
                                  return !query.isRestart() &&
                                         inserted->isFromCurrentContext();
                                });
  }

  // Wait-free, as NtClose calls it (see HandleRegistry).
  bool erase(HANDLE handle) noexcept { return m_scans.erase(handle); }

  void destroyErased() { m_scans.destroyErased(); }

private:
  HandleRegistry<DirectoryScan> m_scans;
};

// Shared by both query hooks and NtClose. Never destroyed, so that hooks still
// running while the DLL unloads do not touch a destroyed registry.
DirectoryScans& directoryScans = *new DirectoryScans;

// The children of a directory in the redirection tree that map something, copied out
// of the tree as it holds them, in UTF-8.
struct TreeDirectorySnapshot
{
  struct Child
  {
    std::string name;
    std::string linkTarget;
  };

  std::vector<Child> children;
};

// Returns the path of a directory in the redirection tree, which leaves out the prefix
// of the NT path the hooks track.
bfs::path treePathOf(const UnicodeString& dirName)
{
  LPCWSTR dirNameW = static_cast<LPCWSTR>(dirName);
  // fix directory name. I'd love to know why microsoft sometimes uses "\??\" vs
  // "\\?\"
  if ((wcsncmp(dirNameW, LR"(\\?\)", 4) == 0) ||
      (wcsncmp(dirNameW, LR"(\??\)", 4) == 0)) {
    dirNameW += 4;
  }
  return bfs::path(dirNameW);
}

// Returns whether the redirection tree holds a directory, which takes the hook context.
bool isInRedirectionTree(const UnicodeString& dirName)
{
  using usvfs::HookContext;
  return READ_CONTEXT()->redirectionTable()->findNode(treePathOf(dirName)).get() !=
         nullptr;
}

// Copies the children of a directory out of the redirection tree, or returns nothing if
// the tree does not hold it. Only copies while holding the hook context, which
// excludes every other hook of the process; converting and matching the names is left
// to gatherVirtualEntries.
std::optional<TreeDirectorySnapshot> snapshotTreeDirectory(const UnicodeString& dirName)
{
  const bfs::path treePath = treePathOf(dirName);

  // Read access, though for now it takes the same exclusive lock as write access
  // (see HookContext::readAccess), so that only the shared side is taken once the two
  // are split. The split has to keep TreeContainer::get() const in mind, which
  // reassigns the container when another process moved the tree to a new block:
  // readers sharing the lock must not do that concurrently.
  using usvfs::HookContext;
  const HookContext::ConstPtr context = READ_CONTEXT();
  const auto node                     = context->redirectionTable()->findNode(treePath);
  if (node.get() == nullptr) {
    return std::nullopt;
  }
  TreeDirectorySnapshot snapshot;
  snapshot.children.reserve(node->numNodes());
  for (auto child = node->filesBegin(); child != node->filesEnd(); ++child) {
    const auto& subNode    = child->second;
    const auto& linkTarget = subNode->data().linkTarget;
    // A child without a link target maps nothing, so whatever of it is on disk is
    // listed with the regular entries. The mapping functions give every directory
    // they add a link target, and the parents they add are dummies, so such a child
    // is only made through the tree itself.
    if (linkTarget.length() == 0 || subNode->hasFlag(usvfs::shared::FLAG_DUMMY)) {
      continue;
    }
    snapshot.children.push_back(
        {subNode->name(), std::string(linkTarget.c_str(), linkTarget.length())});
  }
  return snapshot;
}

// Returns the virtual entries of a directory that match a pattern, all of them
// without one.
std::vector<VirtualMatch>
gatherVirtualEntries(const TreeDirectorySnapshot& snapshot,
                     const std::optional<std::wstring>& pattern)
{
  std::vector<VirtualMatch> matches;

  // RtlIsNameInExpression wants the pattern upcased, as it ignores case; it reads the
  // DOS wildcards as the filesystem does.
  std::optional<std::wstring> upcasedPattern;
  UNICODE_STRING expression{};
  if (pattern) {
    const auto length = static_cast<USHORT>(pattern->size() * sizeof(WCHAR));
    // only read, though UNICODE_STRING has no const buffer
    const UNICODE_STRING source{length, length, const_cast<PWSTR>(pattern->data())};
    upcasedPattern.emplace(pattern->size(), L'\0');
    expression = {length, length, upcasedPattern->data()};
    RtlUpcaseUnicodeString(&expression, &source, FALSE);
  }

  for (const TreeDirectorySnapshot::Child& child : snapshot.children) {
    std::wstring vName =
        ush::string_cast<std::wstring>(child.name, ush::CodePage::UTF8);
    if (upcasedPattern) {
      UNICODE_STRING name{static_cast<USHORT>(vName.size() * sizeof(WCHAR)),
                          static_cast<USHORT>(vName.size() * sizeof(WCHAR)),
                          vName.data()};
      if (!RtlIsNameInExpression(&expression, &name, TRUE, nullptr)) {
        continue;
      }
    }

    matches.push_back(
        {ush::string_cast<std::wstring>(child.linkTarget, ush::CodePage::UTF8),
         std::move(vName)});
  }
  return matches;
}

// Returns whether a handle was granted the access that listing a directory takes,
// which the I/O manager checks before the filesystem sees a query.
bool canListDirectory(HANDLE handle)
{
  PUBLIC_OBJECT_BASIC_INFORMATION information{};
  return NtQueryObject(handle, ObjectBasicInformation, &information,
                       sizeof(information), nullptr) >= 0 &&
         (information.GrantedAccess & FILE_LIST_DIRECTORY) != 0;
}

// Returns whether the status an open of a directory failed with shows that it holds
// nothing on disk but "." and "..": there is no directory at its path, or one pending
// deletion, which is empty, as NTFS and ReFS mark only an empty directory for deletion.
bool holdsNothingOnDisk(NTSTATUS openStatus)
{
  return openStatus == STATUS_OBJECT_NAME_NOT_FOUND ||
         openStatus == STATUS_OBJECT_PATH_NOT_FOUND ||
         openStatus == STATUS_NOT_A_DIRECTORY || openStatus == STATUS_DELETE_PENDING;
}

// Opens the directory the regular entries of a directory in the redirection tree are
// read from: the physical directory at its virtual path, since the open hook may have
// rerouted the caller's handle to a mapping source. The caller's handle is read
// instead: only its dots where the directory holds nothing on disk; whole where it may
// not list the directory, which only a read of it reports like ntdll does, or where
// the directory cannot be opened otherwise. It is the physical directory then, unless
// the NtOpenFile hook opened it, which reroutes every directory the tree maps.
RegularSource openRegularSource(HANDLE callerHandle, const UnicodeString& virtualPath)
{
  if (!canListDirectory(callerHandle)) {
    return {OwnedHandle(), false};
  }
  OpenedDirectory directory = openScanDirectory(static_cast<LPCWSTR>(virtualPath));
  const bool isDotsOnly =
      !directory.handle.isValid() && holdsNothingOnDisk(directory.status);
  // a directory on disk that cannot be opened, as an ACL or a sharing mode may keep it
  // from the scan: the caller's handle may be a mapping source, whose entries the
  // listing then shows in place of those on disk
  if (!directory.handle.isValid() && !isDotsOnly) {
    spdlog::get("hooks")->warn(
        "failed to open {0} to list its entries on disk, listing handle {1:#x} "
        "instead: {2:#x}",
        virtualPath, reinterpret_cast<std::uintptr_t>(callerHandle),
        static_cast<uint32_t>(directory.status));
  }
  return {std::move(directory.handle), isDotsOnly};
}

// Returns whether every I/O on the handle completes before returning. Only then may
// the virtual listing read regular entries through it and complete the query
// itself. A read on an asynchronous handle can pend, and is not waited for: ntdll
// writes its status block after the read returned, into a stack frame that is
// gone. And the handle may be bound to a completion port, whose packets the
// virtual listing does not post.
bool isSynchronousHandle(HANDLE handle)
{
  FILE_MODE_INFORMATION mode{};
  IO_STATUS_BLOCK status;
  const NTSTATUS result = ::NtQueryInformationFile(handle, &status, &mode, sizeof(mode),
                                                   FileModeInformation);
  return result >= 0 &&
         (mode.Mode & (FILE_SYNCHRONOUS_IO_ALERT | FILE_SYNCHRONOUS_IO_NONALERT)) != 0;
}

// Returns the pattern a scan lists with, std::nullopt for every entry, as NTFS and
// ReFS latch it: that of the query starting the scan, unless it is null or empty.
// Then they keep the pattern of the scan it replaces, which is every entry for the
// first scan of a handle.
std::optional<std::wstring>
latchPattern(const DirectoryQuery& query,
             const std::optional<std::wstring>& replacedPattern)
{
  if (query.fileName == nullptr || query.fileName->Length == 0) {
    return replacedPattern;
  }
  return std::wstring(query.fileName->Buffer, query.fileName->Length / sizeof(WCHAR));
}

// Returns the name of the hook a query came through, which the log lines of a query
// carry so that both entry points stay distinguishable.
const char* hookNameOf(const DirectoryQuery& query)
{
  return query.isEx ? "hook_NtQueryDirectoryFileEx" : "hook_NtQueryDirectoryFile";
}

// A scan that the virtual listing cannot or need not serve only forwards its queries,
// so that this is decided once rather than on every query: that of a directory the
// redirection tree does not hold, and that of an asynchronous handle.
DirectoryScan::DirectoryScan(const DirectoryQuery& query, const DirectoryScan* replaced)
    : m_contextGeneration(usvfs::HookContext::generation()),
      m_pattern(latchPattern(query, replaced ? replaced->m_pattern : std::nullopt)),
      m_isFirstCall(replaced == nullptr)
{
  // the path the caller opened, before the open hook rerouted it
  const UnicodeString virtualPath = ntdllHandleTracker.lookup(query.handle);
  const std::optional<TreeDirectorySnapshot> snapshot =
      snapshotTreeDirectory(virtualPath);
  // The open hook reroutes only a directory in the tree, so the caller's handle of any
  // other is the physical directory, which has no virtual entries to add. Checked
  // before the handle, so that the scan of such a directory, as most are, makes no
  // system call.
  if (!snapshot) {
    return;
  }
  if (!isSynchronousHandle(query.handle)) {
    // logged as info, which the default log level shows, since the mapped files go
    // missing from what the caller lists
    spdlog::get("hooks")->info(
        "listing {0} without its virtual entries, as handle {1:#x} is asynchronous",
        virtualPath, reinterpret_cast<std::uintptr_t>(query.handle));
    return;
  }
  m_isPassthrough = false;

  RegularSource regularSource = openRegularSource(query.handle, virtualPath);
  const std::vector<VirtualMatch> matches = gatherVirtualEntries(*snapshot, m_pattern);

  // logged once per scan, with the handle that the later lines of the scan name
  ulog::CallLogger(hookNameOf(query))
      .addParam("path", virtualPath)
      .addParam("handle", query.handle)
      .addParam("informationClass", query.informationClass)
      .addParam("flags", query.flags, static_cast<uint8_t>(ulog::DisplayStyle::Hex))
      .addParam("pattern", m_pattern.value_or(L"<every entry>"))
      .addParam("regularSource",
                std::wstring(regularSource.handle.isValid() ? L"<physical directory>"
                             : regularSource.isDotsOnly ? L"<dots of queried handle>"
                                                        : L"<queried handle>"))
      .addParam("numVirtualFiles", matches.size());

  // called during a query, by when m_virtual is set, and under m_mutex like every
  // other use of it
  m_regular.emplace(
      std::move(regularSource), m_pattern,
      [this] {
        return m_virtual.names();
      },
      query.isRestart());
  m_virtual = VirtualEntryReader(matches);
}

NTSTATUS queryNative(const DirectoryQuery& query)
{
  if (query.isEx) {
    return ::NtQueryDirectoryFileEx(query.handle, query.event, query.apcRoutine,
                                    query.apcContext, query.statusBlock, query.buffer,
                                    query.length, query.informationClass, query.flags,
                                    query.fileName);
  }
  return ::NtQueryDirectoryFile(
      query.handle, query.event, query.apcRoutine, query.apcContext, query.statusBlock,
      query.buffer, query.length, query.informationClass, query.isSingleEntry(),
      query.fileName, query.isRestart());
}

// Returns the alignment ntdll requires of the buffer of a query.
ULONG requiredBufferAlignment(const DirectoryQuery& query,
                              const DirectoryRecordLayout& layout)
{
  if constexpr (sizeof(void*) == 8) {
    return layout.alignment;
  } else {
    // usvfs_x86 runs under WOW64, where the kernel requires only 4-byte alignment,
    // and wow64.dll copies a legacy query whose buffer is not 8-byte aligned through
    // an aligned buffer of its own; it passes the buffer of an Ex query on as it is
    return query.isEx ? 4 : 1;
  }
}

// Returns why the virtual listing cannot serve a query, for the log, or nothing when it
// can.
std::optional<std::string_view> findForwardingReason(const DirectoryQuery& query)
{
  // A query asking for an event, an APC or a completion packet is left to ntdll:
  // the virtual listing completes the query itself and delivers none of them.
  // Passing them on to the internal reads does not work either. The notifications
  // then report the internal read instead of the query: the APC gets the read's
  // status block on our stack, the packet the read's byte count. And an internal
  // read can pend, as NTFS does at the end of a directory on a handle bound to a
  // completion port; ntdll then writes its status block after the read returned,
  // into a stack frame that is gone.
  //
  // TODO: virtualize these queries and those on asynchronous handles too, by
  // delivering the notifications ourselves after completing the query: NtSetEvent,
  // NtQueueApcThread(ApcRoutine, ApcContext, IoStatusBlock), and NtSetIoCompletion
  // with the port and key a NtSetInformationFile hook recorded for the handle (they
  // cannot be queried). Until then, these callers see only the physical directory.
  if (query.event || query.apcRoutine || query.apcContext) {
    return "the query asks for an event, an APC or a completion packet";
  }
  // The I/O manager drops the flags outside the mask, SL_INDEX_SPECIFIED among them,
  // before the filesystem sees them, so the virtual listing ignores them too.
  // SL_RETURN_ON_DISK_ENTRIES_ONLY asks a filter that virtualizes the directory to
  // leave the query to the filesystem. SL_NO_CURSOR_UPDATE, which only NTFS honors,
  // is not emulated.
  constexpr ULONG VIRTUALIZED_FLAGS = SL_RESTART_SCAN | SL_RETURN_SINGLE_ENTRY;
  if ((query.flags & SL_QUERY_DIRECTORY_MASK & ~VIRTUALIZED_FLAGS) != 0) {
    return "the virtual listing does not serve the query flags";
  }
  const std::optional<DirectoryRecordLayout> layout =
      findDirectoryRecordLayout(query.informationClass);
  if (!layout) {
    return "the information class carries no file names";
  }
  // ntdll rejects a buffer that cannot hold a record or is misaligned, so it is left
  // to: the internal reads into that buffer would fail too, and the virtual listing
  // would then report an empty directory instead of the error
  if (query.length < layout->minimumLength) {
    return "the buffer cannot hold a record";
  }
  if (reinterpret_cast<uintptr_t>(query.buffer) %
          requiredBufferAlignment(query, *layout) !=
      0) {
    return "the buffer is misaligned";
  }
  return std::nullopt;
}

// Logs a query on a directory of the redirection tree that goes straight to ntdll, as
// the caller then lists none of its virtual entries. Logged as debug, once per query,
// since FindNextFile repeats the query flags of FindFirstFileEx on every query; only
// looks the directory up when that level is logged, as the lookup takes the hook
// context.
void logForwardedQuery(const DirectoryQuery& query, std::string_view reason)
{
  if (!spdlog::get("hooks")->should_log(spdlog::level::debug)) {
    return;
  }
  const UnicodeString virtualPath = ntdllHandleTracker.lookup(query.handle);
  if (!isInRedirectionTree(virtualPath)) {
    return;
  }
  ulog::CallLogger(hookNameOf(query))
      .addParam("path", virtualPath)
      .addParam("handle", query.handle)
      .addParam("informationClass", query.informationClass)
      .addParam("flags", query.flags, static_cast<uint8_t>(ulog::DisplayStyle::Hex))
      .addParam("length", query.length)
      .addParam("forwarded", reason);
}

// Writes the next entries of the scan: the regular ones first, so that "." and ".."
// come with the first query of a wildcard search as callers expect, then the virtual
// ones. Returns STATUS_SUCCESS if it wrote any, STATUS_BUFFER_OVERFLOW if it passed
// on a partial record from ntdll, STATUS_NO_MORE_FILES once both are exhausted, and
// the error of a regular read that fails, after which the next query reads again.
NTSTATUS DirectoryScan::fill(const DirectoryQuery& query, DirectoryOutput& output)
{
  if (m_regular) {
    const NTSTATUS status = m_regular->fill(query, output);
    // logged as debug, as the caller gets the error as ntdll reports it
    if (status != STATUS_SUCCESS && status != STATUS_BUFFER_OVERFLOW &&
        status != STATUS_NO_MORE_FILES) {
      spdlog::get("hooks")->debug(
          "failed to read the entries on disk for handle {0:#x}: {1:#x}",
          reinterpret_cast<std::uintptr_t>(query.handle),
          static_cast<uint32_t>(status));
    }
    if (status != STATUS_NO_MORE_FILES) {
      return status;
    }
    // the regular entries are done, and the reader's handle and staging area with them
    m_regular.reset();
  }
  return m_virtual.fill(query, output);
}

NTSTATUS DirectoryScan::query(const DirectoryQuery& query)
{
  if (m_isPassthrough) {
    return queryNative(query);
  }
  std::lock_guard lock(m_mutex);
  // queryDirectory forwards the classes that have no layout
  DirectoryOutput output(std::span(static_cast<std::byte*>(query.buffer), query.length),
                         findDirectoryRecordLayout(query.informationClass).value(),
                         {.isSingleEntry = query.isSingleEntry(),
                          .isFirstCall   = std::exchange(m_isFirstCall, false)});
  return output.complete(fill(query, output), *query.statusBlock);
}

NTSTATUS queryDirectory(const DirectoryQuery& query)
{
  if (const std::optional<std::string_view> reason = findForwardingReason(query)) {
    logForwardedQuery(query, *reason);
    return queryNative(query);
  }
  return directoryScans.scanFor(query)->query(query);
}

bool eraseDirectoryScan(HANDLE handle) noexcept
{
  // a query still running on the scan keeps it, and its handle, open until it returns
  return directoryScans.erase(handle);
}

void destroyErasedDirectoryScans()
{
  directoryScans.destroyErased();
}
