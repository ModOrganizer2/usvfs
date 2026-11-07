// this file depends on so many stuff that the easiest way is to include
// pch.h from shared
#include "pch.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <latch>
#include <mutex>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <aclapi.h>

#include <hookcontext.h>
#include <hooks/directory_records.h>
#include <hooks/ntdll.h>
#include <unicodestring.h>

#include <gtest/gtest.h>
#include <test_helpers.h>

#include "tvfs_test.h"

TEST_F(USVFSTest, NtQueryDirectoryFileHonorsFileNameLength)
{
  auto params = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));
  usvfs::RedirectionTreeContainer& tree = ctx->redirectionTable();

  tree.addFile(L"C:\\np.exe", usvfs::RedirectionDataLocal(REAL_FILEA));

  HANDLE hdl =
      hooked_NtOpenFile(L"C:\\", FILE_GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
  ASSERT_NE(INVALID_HANDLE_VALUE, hdl);

  // the pattern is "np.exe": the characters past Length must be ignored
  WCHAR patternBuffer[] = L"np.exe.bak";
  UNICODE_STRING pattern;
  pattern.Buffer        = patternBuffer;
  pattern.Length        = static_cast<USHORT>(wcslen(L"np.exe") * sizeof(WCHAR));
  pattern.MaximumLength = sizeof(patternBuffer);

  IO_STATUS_BLOCK status;
  char buffer[1024];

  usvfs::hook_NtQueryDirectoryFile(hdl, nullptr, nullptr, nullptr, &status, buffer,
                                   1024, FileDirectoryInformation, TRUE, &pattern,
                                   TRUE);

  const auto* info = reinterpret_cast<FILE_DIRECTORY_INFORMATION*>(buffer);
  ASSERT_EQ(STATUS_SUCCESS, status.Status);
  ASSERT_EQ(L"np.exe",
            std::wstring(info->FileName, info->FileNameLength / sizeof(WCHAR)));

  usvfs::hook_NtClose(hdl);
}

TEST_F(USVFSTest, NtQueryDirectoryFileForwardsUnsupportedInformationClass)
{
  auto params = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));

  HANDLE hdl =
      hooked_NtOpenFile(L"C:\\", FILE_GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
  ASSERT_NE(INVALID_HANDLE_VALUE, hdl);

  IO_STATUS_BLOCK status;
  char buffer[1024];

  // not a directory information class; ntdll rejects it with a status that differs
  // between architectures, so compare against a direct call
  const auto nativeQuery = reinterpret_cast<NtQueryDirectoryFile_type>(
      ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtQueryDirectoryFile"));
  const auto hookedQuery = &usvfs::hook_NtQueryDirectoryFile;

  const NTSTATUS expected =
      nativeQuery(hdl, nullptr, nullptr, nullptr, &status, buffer, 1024,
                  FileStandardInformation, TRUE, nullptr, TRUE);
  const NTSTATUS actual =
      hookedQuery(hdl, nullptr, nullptr, nullptr, &status, buffer, 1024,
                  FileStandardInformation, TRUE, nullptr, TRUE);

  ASSERT_LT(expected, 0);
  ASSERT_EQ(expected, actual);

  usvfs::hook_NtClose(hdl);
}

TEST_F(USVFSTest, NtQueryDirectoryFileRestartClosesReopenedHandle)
{
  auto params = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));

  // a mapped entry makes the scan open the directory into a handle of its own
  const std::filesystem::path directory = test::path_of_test_temp(L"restart_reopened");
  std::filesystem::create_directories(directory);
  ctx->redirectionTable().addFile((directory / L"usvfs_virtual.txt").c_str(),
                                  usvfs::RedirectionDataLocal(REAL_FILEA));
  HANDLE hdl = hooked_NtOpenFile(directory.c_str(), FILE_GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
  ASSERT_NE(INVALID_HANDLE_VALUE, hdl);

  IO_STATUS_BLOCK status;
  char buffer[1024];

  // a single entry leaves the regular phase, and so the reopened handle, open
  usvfs::hook_NtQueryDirectoryFile(hdl, nullptr, nullptr, nullptr, &status, buffer,
                                   sizeof(buffer), FileDirectoryInformation, TRUE,
                                   nullptr, FALSE);
  ASSERT_EQ(STATUS_SUCCESS, status.Status);

  DWORD handleCountBefore = 0;
  ASSERT_TRUE(::GetProcessHandleCount(::GetCurrentProcess(), &handleCountBefore));

  for (int restart = 0; restart < 10; ++restart) {
    usvfs::hook_NtQueryDirectoryFile(hdl, nullptr, nullptr, nullptr, &status, buffer,
                                     sizeof(buffer), FileDirectoryInformation, TRUE,
                                     nullptr, TRUE);
    ASSERT_EQ(STATUS_SUCCESS, status.Status);
  }

  DWORD handleCountAfter = 0;
  ASSERT_TRUE(::GetProcessHandleCount(::GetCurrentProcess(), &handleCountAfter));
  EXPECT_EQ(handleCountBefore, handleCountAfter);

  usvfs::hook_NtClose(hdl);
}

namespace
{

// Lists a directory one entry per call, as long as the query succeeds.
std::vector<std::wstring> listSingleEntries(HANDLE directory)
{
  std::vector<std::wstring> names;
  IO_STATUS_BLOCK status;
  char buffer[1024];
  while (usvfs::hook_NtQueryDirectoryFile(
             directory, nullptr, nullptr, nullptr, &status, buffer, sizeof(buffer),
             FileDirectoryInformation, TRUE, nullptr, FALSE) == STATUS_SUCCESS) {
    const auto* info = reinterpret_cast<FILE_DIRECTORY_INFORMATION*>(buffer);
    names.emplace_back(info->FileName, info->FileNameLength / sizeof(WCHAR));
  }
  return names;
}

}  // namespace

TEST_F(USVFSTest, NtQueryDirectoryFileSerializesConcurrentQueries)
{
  auto params = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));
  usvfs::RedirectionTreeContainer& tree = ctx->redirectionTable();

  // virtual entries make each call long enough for the threads to overlap
  for (int file = 0; file < 64; ++file) {
    tree.addFile((L"C:\\usvfs_concurrent_" + std::to_wstring(file) + L".txt").c_str(),
                 usvfs::RedirectionDataLocal(REAL_FILEA));
  }

  const auto openRoot = [] {
    return hooked_NtOpenFile(L"C:\\", FILE_GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
  };

  HANDLE serialHandle = openRoot();
  ASSERT_NE(INVALID_HANDLE_VALUE, serialHandle);
  std::vector<std::wstring> expected = listSingleEntries(serialHandle);
  usvfs::hook_NtClose(serialHandle);
  std::sort(expected.begin(), expected.end());
  ASSERT_TRUE(std::binary_search(expected.begin(), expected.end(),
                                 std::wstring(L"usvfs_concurrent_63.txt")))
      << "the virtual entries must be listed, " << expected.size() << " names found";

  for (int attempt = 0; attempt < 10; ++attempt) {
    HANDLE sharedHandle = openRoot();
    ASSERT_NE(INVALID_HANDLE_VALUE, sharedHandle);

    constexpr int THREAD_COUNT = 8;
    std::latch start(THREAD_COUNT);
    std::mutex namesMutex;
    std::vector<std::wstring> names;
    std::vector<std::thread> threads;
    for (int thread = 0; thread < THREAD_COUNT; ++thread) {
      threads.emplace_back([&] {
        start.arrive_and_wait();
        std::vector<std::wstring> threadNames = listSingleEntries(sharedHandle);
        std::lock_guard lock(namesMutex);
        names.insert(names.end(), threadNames.begin(), threadNames.end());
      });
    }
    for (std::thread& thread : threads) {
      thread.join();
    }
    usvfs::hook_NtClose(sharedHandle);

    // every entry exactly once across all threads, like a single caller sees them
    std::sort(names.begin(), names.end());
    ASSERT_EQ(expected, names) << "attempt " << attempt;
  }
}

namespace
{

enum class CompletionNotification
{
  Event,
  Apc,
  CompletionPort,
};

const auto QUERY_CONTEXT                  = reinterpret_cast<PVOID>(0x5A5A);
constexpr ULONG_PTR COMPLETION_KEY        = 0x1234;
constexpr DWORD COMPLETION_TIMEOUT        = 5000;
const auto UNWRITTEN_STATUS               = static_cast<NTSTATUS>(0xDEADBEEF);
constexpr ULONG_PTR UNWRITTEN_INFORMATION = 0xDEADBEEF;

struct ApcRecord
{
  int count;
  PVOID context;
  PIO_STATUS_BLOCK statusBlock;
};

ApcRecord apcRecord;

VOID NTAPI recordApc(PVOID context, PIO_STATUS_BLOCK statusBlock, ULONG)
{
  ++apcRecord.count;
  apcRecord.context     = context;
  apcRecord.statusBlock = statusBlock;
}

// How one query completed, in terms that do not depend on the entries it returned,
// so that listings of different directories can be compared call by call.
struct CompletionRecord
{
  // from the status block, or the returned status if the query left it alone
  NTSTATUS status;
  bool isStatusBlockWritten;
  // returning STATUS_PENDING instead is consistent too: a query may pend or not
  bool isReturnedStatusFinal;
  bool isEventSignaled;
  int apcCount;
  bool isApcForQuery;
  int packetCount;
  bool isPacketForQuery;
  bool isPacketByteCountInformation;

  bool operator==(const CompletionRecord&) const = default;
};

std::ostream& operator<<(std::ostream& out, const CompletionRecord& record)
{
  return out << "{status " << std::hex << static_cast<ULONG>(record.status) << std::dec
             << ", status block written " << record.isStatusBlockWritten
             << ", returned status final " << record.isReturnedStatusFinal
             << ", event signaled " << record.isEventSignaled << ", APCs "
             << record.apcCount << ", APC for query " << record.isApcForQuery
             << ", packets " << record.packetCount << ", packet for query "
             << record.isPacketForQuery << ", packet byte count is Information "
             << record.isPacketByteCountInformation << "}";
}

// Lists a directory one entry per call, asking to be notified of each completion,
// and records how each call completed, up to the first one that returns nothing.
std::vector<CompletionRecord> recordNotifiedListing(NtQueryDirectoryFile_type query,
                                                    HANDLE directory,
                                                    CompletionNotification notification,
                                                    HANDLE port)
{
  const HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::vector<CompletionRecord> records;
  for (int call = 0; call < 10000; ++call) {
    IO_STATUS_BLOCK status;
    status.Status      = UNWRITTEN_STATUS;
    status.Information = UNWRITTEN_INFORMATION;
    alignas(8) char buffer[1024];
    ::ResetEvent(event);
    apcRecord = {};

    const NTSTATUS result = query(
        directory, notification == CompletionNotification::Event ? event : nullptr,
        notification == CompletionNotification::Apc ? recordApc : nullptr,
        notification == CompletionNotification::Event ? nullptr : QUERY_CONTEXT,
        &status, buffer, sizeof(buffer), FileDirectoryInformation, TRUE, nullptr,
        FALSE);
    // a query that did not pend has delivered its notifications before returning
    const DWORD timeout = result == STATUS_PENDING ? COMPLETION_TIMEOUT : 0;

    CompletionRecord record{};
    switch (notification) {
    case CompletionNotification::Event:
      record.isEventSignaled = ::WaitForSingleObject(event, timeout) == WAIT_OBJECT_0;
      break;
    case CompletionNotification::Apc: {
      const ULONGLONG deadline = ::GetTickCount64() + timeout;
      while (apcRecord.count == 0 && ::GetTickCount64() < deadline) {
        ::SleepEx(10, TRUE);
      }
      ::SleepEx(0, TRUE);
      record.apcCount = apcRecord.count;
      record.isApcForQuery =
          apcRecord.context == QUERY_CONTEXT && apcRecord.statusBlock == &status;
      break;
    }
    case CompletionNotification::CompletionPort: {
      // a packet of a warning or error dequeues as a failure, so look at the
      // context to tell a packet from a timeout
      DWORD bytes = 0;
      for (DWORD packetTimeout = timeout;; packetTimeout = 0) {
        ULONG_PTR key        = 0;
        LPOVERLAPPED context = nullptr;
        ::GetQueuedCompletionStatus(port, &bytes, &key, &context, packetTimeout);
        if (context == nullptr) {
          break;
        }
        ++record.packetCount;
        record.isPacketForQuery = key == COMPLETION_KEY && context == QUERY_CONTEXT;
      }
      record.isPacketByteCountInformation =
          record.packetCount != 0 && bytes == status.Information;
      break;
    }
    }

    record.isStatusBlockWritten  = status.Status != UNWRITTEN_STATUS;
    record.status                = record.isStatusBlockWritten ? status.Status : result;
    record.isReturnedStatusFinal = result == STATUS_PENDING || result == record.status;
    records.push_back(record);
    if (record.status != STATUS_SUCCESS) {
      break;
    }
  }
  ::CloseHandle(event);
  return records;
}

// The virtual entries make the listing reach the virtual phase, which completes
// the queries itself.
void addVirtualEntries(usvfs::RedirectionTreeContainer& tree)
{
  tree.addFile(L"C:\\usvfs_notified_a.txt", usvfs::RedirectionDataLocal(REAL_FILEA));
  tree.addFile(L"C:\\usvfs_notified_b.txt", usvfs::RedirectionDataLocal(REAL_FILEA));
}

HANDLE openRootDirectory(bool isSynchronous)
{
  return hooked_NtOpenFile(
      L"C:\\", FILE_GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
      FILE_DIRECTORY_FILE | (isSynchronous ? FILE_SYNCHRONOUS_IO_NONALERT : 0));
}

NtQueryDirectoryFile_type nativeQueryDirectoryFile()
{
  return reinterpret_cast<NtQueryDirectoryFile_type>(
      ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtQueryDirectoryFile"));
}

// Lists the root directory through ntdll and through the hook, each with a handle
// of its own, and expects every call under the hook to complete the way ntdll
// completes a call with the same result.
void expectNativeCompletions(bool isSynchronous, CompletionNotification notification)
{
  // a query moves its handle through the directory and completes to the port of
  // its handle, so each listing gets a handle and a port of its own
  HANDLE nativeHandle = openRootDirectory(isSynchronous);
  HANDLE hookedHandle = openRootDirectory(isSynchronous);
  ASSERT_NE(INVALID_HANDLE_VALUE, nativeHandle);
  ASSERT_NE(INVALID_HANDLE_VALUE, hookedHandle);
  HANDLE nativePort = nullptr;
  HANDLE hookedPort = nullptr;
  if (notification == CompletionNotification::CompletionPort) {
    nativePort = ::CreateIoCompletionPort(nativeHandle, nullptr, COMPLETION_KEY, 0);
    hookedPort = ::CreateIoCompletionPort(hookedHandle, nullptr, COMPLETION_KEY, 0);
    ASSERT_NE(nullptr, nativePort);
    ASSERT_NE(nullptr, hookedPort);
  }

  const std::vector<CompletionRecord> expected = recordNotifiedListing(
      nativeQueryDirectoryFile(), nativeHandle, notification, nativePort);
  const std::vector<CompletionRecord> actual = recordNotifiedListing(
      &usvfs::hook_NtQueryDirectoryFile, hookedHandle, notification, hookedPort);

  ASSERT_LE(2u, expected.size());
  ASSERT_EQ(STATUS_SUCCESS, expected.front().status);
  ASSERT_EQ(STATUS_NO_MORE_FILES, expected.back().status);
  for (size_t call = 0; call + 1 < actual.size(); ++call) {
    ASSERT_EQ(expected.front(), actual[call]) << "call " << call;
  }
  ASSERT_EQ(expected.back(), actual.back()) << "the end of the listing";

  usvfs::hook_NtClose(nativeHandle);
  usvfs::hook_NtClose(hookedHandle);
  if (notification == CompletionNotification::CompletionPort) {
    ::CloseHandle(nativePort);
    ::CloseHandle(hookedPort);
  }
}

}  // namespace

TEST_F(USVFSTest, NtQueryDirectoryFileSignalsEventOnSynchronousHandle)
{
  auto params = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));
  addVirtualEntries(ctx->redirectionTable());
  expectNativeCompletions(true, CompletionNotification::Event);
}

TEST_F(USVFSTest, NtQueryDirectoryFileSignalsEventOnAsynchronousHandle)
{
  auto params = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));
  addVirtualEntries(ctx->redirectionTable());
  expectNativeCompletions(false, CompletionNotification::Event);
}

TEST_F(USVFSTest, NtQueryDirectoryFileQueuesApcOnAsynchronousHandle)
{
  auto params = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));
  addVirtualEntries(ctx->redirectionTable());
  expectNativeCompletions(false, CompletionNotification::Apc);
}

TEST_F(USVFSTest, NtQueryDirectoryFilePostsCompletionPackets)
{
  auto params = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));
  addVirtualEntries(ctx->redirectionTable());
  expectNativeCompletions(false, CompletionNotification::CompletionPort);
}

namespace
{

struct QueryOutcome
{
  NTSTATUS result;
  IO_STATUS_BLOCK statusBlock;
};

// Queries a directory with a status block that shows whether the query wrote it.
QueryOutcome queryWithMarkedStatusBlock(NtQueryDirectoryFile_type query,
                                        HANDLE directory, PUNICODE_STRING fileName)
{
  QueryOutcome outcome;
  outcome.statusBlock.Status      = UNWRITTEN_STATUS;
  outcome.statusBlock.Information = UNWRITTEN_INFORMATION;
  alignas(8) char buffer[1024];
  outcome.result =
      query(directory, nullptr, nullptr, nullptr, &outcome.statusBlock, buffer,
            sizeof(buffer), FileDirectoryInformation, FALSE, fileName, FALSE);
  return outcome;
}

// Lists a directory until it reports its end, then queries it once more, and
// returns the outcomes of these two queries.
std::vector<QueryOutcome> queryPastTheLastEntry(NtQueryDirectoryFile_type query,
                                                HANDLE directory)
{
  std::vector<QueryOutcome> endOutcomes;
  for (int call = 0; endOutcomes.size() < 2 && call < 10000; ++call) {
    const QueryOutcome outcome = queryWithMarkedStatusBlock(query, directory, nullptr);
    if (outcome.result != STATUS_SUCCESS) {
      endOutcomes.push_back(outcome);
    }
  }
  return endOutcomes;
}

void expectSameOutcome(const QueryOutcome& expected, const QueryOutcome& actual)
{
  EXPECT_EQ(expected.result, actual.result);
  EXPECT_EQ(expected.statusBlock.Status, actual.statusBlock.Status);
  EXPECT_EQ(expected.statusBlock.Information, actual.statusBlock.Information);
}

}  // namespace

TEST_F(USVFSTest, NtQueryDirectoryFileLeavesStatusBlockWhenNothingMatches)
{
  auto params = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));
  addVirtualEntries(ctx->redirectionTable());
  usvfs::UnicodeString pattern(L"usvfs_no_such_file_*");

  // a query moves its handle through the directory, so ntdll and the hook each
  // list through a handle of their own
  HANDLE nativeHandle = openRootDirectory(true);
  HANDLE hookedHandle = openRootDirectory(true);
  ASSERT_NE(INVALID_HANDLE_VALUE, nativeHandle);
  ASSERT_NE(INVALID_HANDLE_VALUE, hookedHandle);

  const QueryOutcome expected = queryWithMarkedStatusBlock(
      nativeQueryDirectoryFile(), nativeHandle, static_cast<PUNICODE_STRING>(pattern));
  const QueryOutcome actual =
      queryWithMarkedStatusBlock(&usvfs::hook_NtQueryDirectoryFile, hookedHandle,
                                 static_cast<PUNICODE_STRING>(pattern));

  ASSERT_EQ(STATUS_NO_SUCH_FILE, expected.result);
  expectSameOutcome(expected, actual);

  usvfs::hook_NtClose(nativeHandle);
  usvfs::hook_NtClose(hookedHandle);
}

TEST_F(USVFSTest, NtQueryDirectoryFileReportsNoBytesPastTheLastEntry)
{
  auto params = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));
  addVirtualEntries(ctx->redirectionTable());

  HANDLE nativeHandle = openRootDirectory(true);
  HANDLE hookedHandle = openRootDirectory(true);
  ASSERT_NE(INVALID_HANDLE_VALUE, nativeHandle);
  ASSERT_NE(INVALID_HANDLE_VALUE, hookedHandle);

  const std::vector<QueryOutcome> expected =
      queryPastTheLastEntry(nativeQueryDirectoryFile(), nativeHandle);
  const std::vector<QueryOutcome> actual =
      queryPastTheLastEntry(&usvfs::hook_NtQueryDirectoryFile, hookedHandle);

  ASSERT_EQ(2u, expected.size());
  ASSERT_EQ(STATUS_NO_MORE_FILES, expected.front().result);
  ASSERT_EQ(expected.size(), actual.size()) << "the listing does not end";
  for (size_t query = 0; query < expected.size(); ++query) {
    SCOPED_TRACE(query);
    expectSameOutcome(expected[query], actual[query]);
  }

  usvfs::hook_NtClose(nativeHandle);
  usvfs::hook_NtClose(hookedHandle);
}

namespace
{

const FILE_INFORMATION_CLASS DIRECTORY_INFORMATION_CLASSES[] = {
    FileDirectoryInformation,          FileFullDirectoryInformation,
    FileBothDirectoryInformation,      FileNamesInformation,
    FileIdBothDirectoryInformation,    FileIdFullDirectoryInformation,
    FileIdExtdDirectoryInformation,    FileIdExtdBothDirectoryInformation,
    FileId64ExtdDirectoryInformation,  FileId64ExtdBothDirectoryInformation,
    FileIdAllExtdDirectoryInformation, FileIdAllExtdBothDirectoryInformation,
};

struct BufferQuery
{
  FILE_INFORMATION_CLASS informationClass;
  bool isEx;
  // bytes past an 8-byte boundary
  size_t offset;
  ULONG length;
};

std::string describe(const BufferQuery& query)
{
  return std::format("class {}, {} entry point, offset {}, length {:#x}",
                     static_cast<int>(query.informationClass),
                     query.isEx ? "Ex" : "legacy", query.offset, query.length);
}

// Queries through the entry point of the query, restarting the scan so that every
// query starts from the same state.
QueryOutcome queryBuffer(bool isNative, HANDLE directory, const BufferQuery& query,
                         PUNICODE_STRING fileName)
{
  QueryOutcome outcome;
  outcome.statusBlock.Status      = UNWRITTEN_STATUS;
  outcome.statusBlock.Information = UNWRITTEN_INFORMATION;
  alignas(8) char storage[0x1000 + 8];
  PVOID buffer = storage + query.offset;

  if (query.isEx) {
    const auto exQuery =
        isNative ? reinterpret_cast<NtQueryDirectoryFileEx_type>(::GetProcAddress(
                       ::GetModuleHandleW(L"ntdll.dll"), "NtQueryDirectoryFileEx"))
                 : &usvfs::hook_NtQueryDirectoryFileEx;
    outcome.result =
        exQuery(directory, nullptr, nullptr, nullptr, &outcome.statusBlock, buffer,
                query.length, query.informationClass, SL_RESTART_SCAN, fileName);
  } else {
    const auto legacyQuery =
        isNative ? nativeQueryDirectoryFile() : &usvfs::hook_NtQueryDirectoryFile;
    outcome.result =
        legacyQuery(directory, nullptr, nullptr, nullptr, &outcome.statusBlock, buffer,
                    query.length, query.informationClass, FALSE, fileName, TRUE);
  }
  return outcome;
}

// Expects the hook to reject a buffer that ntdll rejects, with the same result,
// and, if asked to, to list the virtual entries into a buffer that ntdll accepts.
void expectBufferHandledLikeNtdll(const BufferQuery& query, bool shouldListIfAccepted)
{
  SCOPED_TRACE(describe(query));
  HANDLE nativeHandle = openRootDirectory(true);
  HANDLE hookedHandle = openRootDirectory(true);
  ASSERT_NE(INVALID_HANDLE_VALUE, nativeHandle);
  ASSERT_NE(INVALID_HANDLE_VALUE, hookedHandle);

  const QueryOutcome expected = queryBuffer(true, nativeHandle, query, nullptr);
  // ntdll leaves the status block alone when it rejects the buffer, and fills it
  // for a buffer it accepts, the root directory not being empty
  if (expected.statusBlock.Status == UNWRITTEN_STATUS) {
    expectSameOutcome(expected, queryBuffer(false, hookedHandle, query, nullptr));
  } else if (shouldListIfAccepted) {
    usvfs::UnicodeString pattern(L"usvfs_notified_a.txt");
    const QueryOutcome actual =
        queryBuffer(false, hookedHandle, query, static_cast<PUNICODE_STRING>(pattern));
    EXPECT_EQ(STATUS_SUCCESS, actual.result) << "the virtual entry is not listed";
  }

  usvfs::hook_NtClose(nativeHandle);
  usvfs::hook_NtClose(hookedHandle);
}

}  // namespace

TEST_F(USVFSTest, NtQueryDirectoryFileRejectsUndersizedBuffersLikeNtdll)
{
  auto params = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));
  addVirtualEntries(ctx->redirectionTable());

  // every length up to past the largest minimum, 0x80; whether a small buffer that
  // ntdll accepts lists the virtual entries is up to how the hook fills it
  for (const FILE_INFORMATION_CLASS informationClass : DIRECTORY_INFORMATION_CLASSES) {
    for (const bool isEx : {false, true}) {
      for (ULONG length = 0; length <= 0x88; ++length) {
        expectBufferHandledLikeNtdll({informationClass, isEx, 0, length}, false);
      }
    }
  }
}

TEST_F(USVFSTest, NtQueryDirectoryFileRejectsMisalignedBuffersLikeNtdll)
{
  auto params = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));
  addVirtualEntries(ctx->redirectionTable());

  for (const FILE_INFORMATION_CLASS informationClass : DIRECTORY_INFORMATION_CLASSES) {
    for (const bool isEx : {false, true}) {
      for (size_t offset = 0; offset < 8; ++offset) {
        expectBufferHandledLikeNtdll({informationClass, isEx, offset, 0x1000}, true);
      }
    }
  }
}

namespace
{

template <FILE_INFORMATION_CLASS InformationClass, typename Record>
struct RecordType
{
  static constexpr FILE_INFORMATION_CLASS informationClass = InformationClass;
  using Type                                               = Record;
};

using RecordTypes = ::testing::Types<
    RecordType<FileDirectoryInformation, FILE_DIRECTORY_INFORMATION>,
    RecordType<FileFullDirectoryInformation, FILE_FULL_DIR_INFORMATION>,
    RecordType<FileBothDirectoryInformation, FILE_BOTH_DIR_INFORMATION>,
    RecordType<FileNamesInformation, FILE_NAMES_INFORMATION>,
    RecordType<FileIdBothDirectoryInformation, FILE_ID_BOTH_DIR_INFORMATION>,
    RecordType<FileIdFullDirectoryInformation, FILE_ID_FULL_DIR_INFORMATION>,
    RecordType<FileIdExtdDirectoryInformation, FILE_ID_EXTD_DIR_INFORMATION>,
    RecordType<FileIdExtdBothDirectoryInformation, FILE_ID_EXTD_BOTH_DIR_INFORMATION>,
    RecordType<FileId64ExtdDirectoryInformation, FILE_ID_64_EXTD_DIR_INFORMATION>,
    RecordType<FileId64ExtdBothDirectoryInformation,
               FILE_ID_64_EXTD_BOTH_DIR_INFORMATION>,
    RecordType<FileIdAllExtdDirectoryInformation, FILE_ID_ALL_EXTD_DIR_INFORMATION>,
    RecordType<FileIdAllExtdBothDirectoryInformation,
               FILE_ID_ALL_EXTD_BOTH_DIR_INFORMATION>>;

// A record built through the fields of its declared structure, so that it does not
// depend on the offsets in the layout table it is read with.
template <typename Record>
struct SyntheticRecord
{
  alignas(Record) std::array<std::byte, 0x400> storage{};
  size_t size = 0;

  SyntheticRecord(std::wstring_view name, ULONG nextEntryOffset)
  {
    fields().NextEntryOffset = nextEntryOffset;
    fields().FileNameLength  = static_cast<ULONG>(name.size() * sizeof(WCHAR));
    std::memcpy(storage.data() + offsetof(Record, FileName), name.data(),
                fields().FileNameLength);
    size = offsetof(Record, FileName) + fields().FileNameLength;
  }

  Record& fields() { return *reinterpret_cast<Record*>(storage.data()); }
  std::span<const std::byte> bytes() const { return std::span(storage).first(size); }
};

template <typename T>
class DirectoryRecordTest : public ::testing::Test
{
protected:
  using Record = typename T::Type;

  static DirectoryRecordLayout layout()
  {
    return findDirectoryRecordLayout(T::informationClass).value();
  }
};

// A directory on the volume the tests run from, which differs from the system one on
// a ReFS Dev Drive. Its names take every length up to 64 characters, so that records
// end at every offset modulo 8.
std::filesystem::path generatedListingDirectory()
{
  const std::filesystem::path directory = test::path_of_test_temp(L"directory_records");
  std::filesystem::create_directories(directory);
  for (size_t length = 1; length <= 64; ++length) {
    std::ofstream(directory / std::wstring(length, L'n'));
  }
  return directory;
}

// The directories whose native listings the records are checked against, one per
// filesystem when the tests run from a volume other than NTFS.
std::vector<std::filesystem::path> nativeListingDirectories()
{
  return {L"C:\\Windows", generatedListingDirectory()};
}

std::wstring fileSystemOf(const std::filesystem::path& path)
{
  std::wstring volume(MAX_PATH, L'\0');
  std::wstring fileSystem(MAX_PATH, L'\0');
  if (!::GetVolumePathNameW(path.c_str(), volume.data(), MAX_PATH) ||
      !::GetVolumeInformationW(volume.c_str(), nullptr, 0, nullptr, nullptr, nullptr,
                               fileSystem.data(), MAX_PATH)) {
    return L"unknown";
  }
  return fileSystem.c_str();
}

std::string describeDirectory(const std::filesystem::path& path)
{
  return std::format("{} ({})", path.string(),
                     std::filesystem::path(fileSystemOf(path)).string());
}

// Lists a directory through ntdll and reads the names with DirectoryRecord.
std::vector<std::wstring> listWithDirectoryRecords(const std::filesystem::path& path,
                                                   const DirectoryRecordLayout& layout)
{
  const HANDLE directory = ::CreateFileW(
      path.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  EXPECT_NE(INVALID_HANDLE_VALUE, directory);

  std::vector<std::wstring> names;
  std::vector<std::byte> buffer(0x10000);
  IO_STATUS_BLOCK statusBlock;
  while (nativeQueryDirectoryFile()(directory, nullptr, nullptr, nullptr, &statusBlock,
                                    buffer.data(), static_cast<ULONG>(buffer.size()),
                                    layout.informationClass, FALSE, nullptr,
                                    FALSE) == STATUS_SUCCESS) {
    std::span<const std::byte> data(buffer.data(), statusBlock.Information);
    for (;;) {
      const DirectoryRecord record(layout, data);
      EXPECT_FALSE(record.isTruncated());
      names.push_back(record.fileName());
      if (record.nextEntryOffset() == 0) {
        break;
      }
      data = data.subspan(record.nextEntryOffset());
    }
  }
  ::CloseHandle(directory);
  std::ranges::sort(names);
  return names;
}

std::vector<std::wstring> listWithFindFirstFile(const std::filesystem::path& path)
{
  std::vector<std::wstring> names;
  WIN32_FIND_DATAW data;
  const HANDLE search = ::FindFirstFileW((path / L"*").c_str(), &data);
  EXPECT_NE(INVALID_HANDLE_VALUE, search);
  do {
    names.emplace_back(data.cFileName);
  } while (::FindNextFileW(search, &data));
  ::FindClose(search);
  std::ranges::sort(names);
  return names;
}

}  // namespace

TYPED_TEST_SUITE(DirectoryRecordTest, RecordTypes);

TYPED_TEST(DirectoryRecordTest, ReadsTheFieldsOfARecord)
{
  const SyntheticRecord<typename TestFixture::Record> synthetic(L"usvfs.txt", 0x100);
  const DirectoryRecord record(TestFixture::layout(), synthetic.bytes());

  EXPECT_EQ(0x100u, record.nextEntryOffset());
  EXPECT_EQ(synthetic.size, record.size());
  EXPECT_FALSE(record.isTruncated());
  EXPECT_EQ(L"usvfs.txt", record.fileName());
}

TYPED_TEST(DirectoryRecordTest, ReadsACutRecordAsTruncated)
{
  const SyntheticRecord<typename TestFixture::Record> synthetic(L"usvfs.txt", 0);
  const DirectoryRecord record(TestFixture::layout(),
                               synthetic.bytes().first(synthetic.size - sizeof(WCHAR)));

  EXPECT_EQ(synthetic.size, record.size());
  EXPECT_TRUE(record.isTruncated());
  EXPECT_EQ(L"usvfs.tx", record.fileName());
}

TYPED_TEST(DirectoryRecordTest, ReadsBytesShortOfTheFixedFieldsAsTruncated)
{
  const SyntheticRecord<typename TestFixture::Record> synthetic(L"usvfs.txt", 0x100);
  const DirectoryRecord record(TestFixture::layout(), synthetic.bytes().first(2));

  EXPECT_EQ(0u, record.nextEntryOffset());
  EXPECT_TRUE(record.isTruncated());
  EXPECT_EQ(L"", record.fileName());
}

// ntdll is the reference for the declared structures the layout table derives from
TYPED_TEST(DirectoryRecordTest, ReadsTheNamesNtdllWrites)
{
  for (const std::filesystem::path& directory : nativeListingDirectories()) {
    SCOPED_TRACE(describeDirectory(directory));
    const std::vector<std::wstring> names =
        listWithDirectoryRecords(directory, TestFixture::layout());

    ASSERT_LT(2u, names.size());
    EXPECT_EQ(listWithFindFirstFile(directory), names);
  }
}

TEST(DirectoryRecordLayoutTest, KnowsOnlyClassesWhoseRecordsCarryAFileName)
{
  EXPECT_TRUE(findDirectoryRecordLayout(FileDirectoryInformation).has_value());
  EXPECT_FALSE(findDirectoryRecordLayout(FileStandardInformation).has_value());
  EXPECT_FALSE(findDirectoryRecordLayout(FileObjectIdInformation).has_value());
}

namespace
{

// Returns the data each query of a native listing returned into a buffer of the
// given length.
std::vector<std::vector<std::byte>>
listNativeQueries(const std::filesystem::path& path,
                  FILE_INFORMATION_CLASS informationClass, ULONG length)
{
  const HANDLE directory = ::CreateFileW(
      path.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  EXPECT_NE(INVALID_HANDLE_VALUE, directory);

  std::vector<std::vector<std::byte>> queries;
  std::vector<std::byte> buffer(length);
  IO_STATUS_BLOCK statusBlock;
  while (nativeQueryDirectoryFile()(directory, nullptr, nullptr, nullptr, &statusBlock,
                                    buffer.data(), length, informationClass, FALSE,
                                    nullptr, FALSE) == STATUS_SUCCESS) {
    queries.emplace_back(buffer.begin(), buffer.begin() + statusBlock.Information);
  }
  ::CloseHandle(directory);
  return queries;
}

// Returns where the records in the data of a query start.
std::vector<size_t> recordOffsets(const DirectoryRecordLayout& layout,
                                  std::span<const std::byte> data)
{
  std::vector<size_t> offsets{0};
  for (;;) {
    const ULONG next =
        DirectoryRecord(layout, data.subspan(offsets.back())).nextEntryOffset();
    if (next == 0) {
      return offsets;
    }
    offsets.push_back(offsets.back() + next);
  }
}

}  // namespace

// ntdll is the reference for where records go and for the size reported
TYPED_TEST(DirectoryRecordTest, LaysOutRecordsLikeNtdll)
{
  // small enough for the listing to take several queries
  constexpr ULONG LENGTH             = 0x800;
  const DirectoryRecordLayout layout = TestFixture::layout();
  for (const std::filesystem::path& directory : nativeListingDirectories()) {
    SCOPED_TRACE(describeDirectory(directory));
    const std::vector<std::vector<std::byte>> queries =
        listNativeQueries(directory, layout.informationClass, LENGTH);
    ASSERT_LE(2u, queries.size());

    for (size_t query = 0; query < queries.size(); ++query) {
      SCOPED_TRACE(query);
      const std::span<const std::byte> expected(queries[query]);
      const std::vector<size_t> offsets = recordOffsets(layout, expected);
      std::vector<std::byte> buffer(LENGTH);
      DirectoryOutput output(buffer, layout,
                             {.isSingleEntry = false, .isFirstCall = false});
      for (const size_t offset : offsets) {
        ASSERT_TRUE(output.append(DirectoryRecord(layout, expected.subspan(offset))));
      }

      // the records with their links, and the size; the padding between them is
      // whatever the buffer held
      EXPECT_EQ(expected.size(), output.size());
      for (const size_t offset : offsets) {
        const size_t size = DirectoryRecord(layout, expected.subspan(offset)).size();
        EXPECT_TRUE(std::ranges::equal(expected.subspan(offset, size),
                                       std::span(buffer).subspan(offset, size)))
            << "record at " << offset;
      }
    }
  }
}

// The rule NtfsQueryDirectory and RefsQueryDirectory apply to a record after the
// first: it fits when it ends, without the padding after it, within the buffer.
// Native listings are no reference for it: where a query ends depends on the
// filesystem, on the filters above it, and on how NTFS makes up "." and "..".
TYPED_TEST(DirectoryRecordTest, FitsARecordThatEndsWithTheBuffer)
{
  using Record = typename TestFixture::Record;
  const SyntheticRecord<Record> first(L"usvfs.txt", 0);
  const SyntheticRecord<Record> second(L"usvfs_second.txt", 0);
  const size_t end = (first.size + 7) / 8 * 8 + second.size;

  for (const size_t length : {end, end - 1}) {
    SCOPED_TRACE(length);
    std::vector<std::byte> buffer(length);
    DirectoryOutput output(buffer, TestFixture::layout(),
                           {.isSingleEntry = false, .isFirstCall = false});
    ASSERT_TRUE(output.append(DirectoryRecord(TestFixture::layout(), first.bytes())));

    EXPECT_EQ(length == end,
              output.append(DirectoryRecord(TestFixture::layout(), second.bytes())));
    EXPECT_EQ(length == end ? end : first.size, output.size());
  }
}

TYPED_TEST(DirectoryRecordTest, WritesOneRecordForASingleEntryQuery)
{
  const SyntheticRecord<typename TestFixture::Record> synthetic(L"usvfs.txt", 0);
  const DirectoryRecord record(TestFixture::layout(), synthetic.bytes());
  std::vector<std::byte> buffer(0x400);
  DirectoryOutput output(buffer, TestFixture::layout(),
                         {.isSingleEntry = true, .isFirstCall = false});

  EXPECT_FALSE(output.isSatisfied());
  ASSERT_TRUE(output.append(record));
  EXPECT_TRUE(output.isSatisfied());
  EXPECT_FALSE(output.append(record));
  EXPECT_EQ(synthetic.size, output.size());
}

TYPED_TEST(DirectoryRecordTest, WritesARecordUnderAnotherName)
{
  SyntheticRecord<typename TestFixture::Record> synthetic(L"usvfs.txt", 0x100);
  synthetic.fields().FileIndex = 0x12345678;
  std::vector<std::byte> buffer(0x400);
  DirectoryOutput output(buffer, TestFixture::layout(),
                         {.isSingleEntry = false, .isFirstCall = false});

  ASSERT_TRUE(
      output.appendRenamed(DirectoryRecord(TestFixture::layout(), synthetic.bytes()),
                           L"usvfs_a_longer_name.txt"));

  const DirectoryRecord renamed(TestFixture::layout(),
                                std::span(buffer).first(output.size()));
  EXPECT_EQ(L"usvfs_a_longer_name.txt", renamed.fileName());
  EXPECT_EQ(0u, renamed.nextEntryOffset());
  EXPECT_EQ(output.size(), renamed.size());
  EXPECT_EQ(
      0x12345678u,
      reinterpret_cast<const typename TestFixture::Record*>(buffer.data())->FileIndex);
}

TYPED_TEST(DirectoryRecordTest, RefusesATruncatedRecord)
{
  const SyntheticRecord<typename TestFixture::Record> synthetic(L"usvfs.txt", 0);
  const DirectoryRecord record(TestFixture::layout(),
                               synthetic.bytes().first(synthetic.size - sizeof(WCHAR)));
  std::vector<std::byte> buffer(0x400);
  DirectoryOutput output(buffer, TestFixture::layout(),
                         {.isSingleEntry = false, .isFirstCall = false});

  EXPECT_FALSE(output.append(record));
  EXPECT_FALSE(output.appendRenamed(record, L"usvfs.txt"));
  EXPECT_TRUE(output.isEmpty());
}

namespace
{

using ShortNameRecordTypes = ::testing::Types<
    RecordType<FileBothDirectoryInformation, FILE_BOTH_DIR_INFORMATION>,
    RecordType<FileIdBothDirectoryInformation, FILE_ID_BOTH_DIR_INFORMATION>,
    RecordType<FileIdExtdBothDirectoryInformation, FILE_ID_EXTD_BOTH_DIR_INFORMATION>,
    RecordType<FileId64ExtdBothDirectoryInformation,
               FILE_ID_64_EXTD_BOTH_DIR_INFORMATION>,
    RecordType<FileIdAllExtdBothDirectoryInformation,
               FILE_ID_ALL_EXTD_BOTH_DIR_INFORMATION>>;

template <typename T>
class ShortNameRecordTest : public DirectoryRecordTest<T>
{};

}  // namespace

TYPED_TEST_SUITE(ShortNameRecordTest, ShortNameRecordTypes);

// An 8.3 name is an alias of the name a record had, so a record written under another
// name goes without one, as NTFS writes a record that has none: with a zero length
// and a zeroed name, over whatever the buffer held.
TYPED_TEST(ShortNameRecordTest, DropsTheShortNameOfARenamedRecord)
{
  using Record                           = typename TestFixture::Record;
  constexpr std::wstring_view SHORT_NAME = L"USVFS_~1.TXT";
  SyntheticRecord<Record> synthetic(L"usvfs_a_long_name.txt", 0);
  synthetic.fields().ShortNameLength =
      static_cast<CCHAR>(SHORT_NAME.size() * sizeof(WCHAR));
  std::memcpy(synthetic.fields().ShortName, SHORT_NAME.data(),
              SHORT_NAME.size() * sizeof(WCHAR));
  std::vector<std::byte> buffer(0x400, std::byte{0xCC});
  DirectoryOutput output(buffer, TestFixture::layout(),
                         {.isSingleEntry = false, .isFirstCall = false});

  ASSERT_TRUE(output.appendRenamed(
      DirectoryRecord(TestFixture::layout(), synthetic.bytes()), L"usvfs_v.txt"));

  const auto& renamed = *reinterpret_cast<const Record*>(buffer.data());
  EXPECT_EQ(0, renamed.ShortNameLength);
  EXPECT_TRUE(std::ranges::all_of(std::as_bytes(std::span(renamed.ShortName)),
                                  [](std::byte value) {
                                    return value == std::byte{0};
                                  }));
}

namespace
{

struct ListedQuery
{
  QueryOutcome outcome;
  // the bytes the status block reports, for a query that returned records, whole or
  // cut short
  std::vector<std::byte> data;
};

// The queries of a listing, which differ only in where they find the handle.
struct ListingQuery
{
  FILE_INFORMATION_CLASS informationClass;
  ULONG length;
  bool isSingleEntry;
  // nothing for a query without FileName
  std::optional<std::wstring_view> pattern;
  bool isRestart = false;
};

// Queries a directory with a status block that shows whether the query wrote it.
ListedQuery queryOnce(NtQueryDirectoryFile_type query, HANDLE directory,
                      const ListingQuery& listing)
{
  std::optional<usvfs::UnicodeString> fileName;
  if (listing.pattern) {
    fileName.emplace(listing.pattern->data(), listing.pattern->size());
  }
  // the hook writes into the caller's buffer itself, where the kernel bounds what
  // ntdll writes, so canaries past the end show a record or padding written beyond it
  constexpr size_t CANARY_LENGTH = 32;
  constexpr std::byte CANARY{0x5A};
  std::vector<std::byte> buffer(listing.length + CANARY_LENGTH, CANARY);
  ListedQuery listed;
  listed.outcome.statusBlock.Status      = UNWRITTEN_STATUS;
  listed.outcome.statusBlock.Information = UNWRITTEN_INFORMATION;
  listed.outcome.result                  = query(
      directory, nullptr, nullptr, nullptr, &listed.outcome.statusBlock, buffer.data(),
      listing.length, listing.informationClass, listing.isSingleEntry,
      fileName ? static_cast<PUNICODE_STRING>(*fileName) : nullptr, listing.isRestart);

  EXPECT_EQ(std::vector(CANARY_LENGTH, CANARY),
            std::vector(buffer.begin() + listing.length, buffer.end()))
      << "the query wrote past the end of the buffer";

  if (listed.outcome.result == STATUS_SUCCESS ||
      listed.outcome.result == STATUS_BUFFER_OVERFLOW) {
    const ULONG_PTR reported = listed.outcome.statusBlock.Information;
    EXPECT_LE(reported, listing.length)
        << "the query reported more than the buffer holds";
    listed.data.assign(buffer.begin(),
                       buffer.begin() + std::min<ULONG_PTR>(reported, listing.length));
  }
  return listed;
}

// Lists a directory as queryOnce queries it, and returns every query up to the first
// that fails.
std::vector<ListedQuery> listQueries(NtQueryDirectoryFile_type query, HANDLE directory,
                                     const ListingQuery& listing)
{
  std::vector<ListedQuery> queries;
  do {
    queries.push_back(queryOnce(query, directory, listing));
  } while (queries.back().outcome.result == STATUS_SUCCESS);
  return queries;
}

// Lists a directory through ntdll, as listQueries does.
std::vector<ListedQuery> listNativeOutcomes(const std::filesystem::path& path,
                                            std::wstring_view pattern, ULONG length)
{
  const HANDLE directory = ::CreateFileW(
      path.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  EXPECT_NE(INVALID_HANDLE_VALUE, directory);
  std::vector<ListedQuery> queries =
      listQueries(nativeQueryDirectoryFile(), directory,
                  {FileDirectoryInformation, length, false, pattern});
  ::CloseHandle(directory);
  return queries;
}

// Writes the records of a native query to a DirectoryOutput that ends with them, and
// completes it with the status a fill of the output ended with.
QueryOutcome replay(const ListedQuery& native, bool isFirstCall, NTSTATUS fillStatus)
{
  const DirectoryRecordLayout layout =
      findDirectoryRecordLayout(FileDirectoryInformation).value();
  std::vector<std::byte> buffer(native.data.size());
  DirectoryOutput output(buffer, layout,
                         {.isSingleEntry = false, .isFirstCall = isFirstCall});
  if (!native.data.empty()) {
    for (const size_t offset : recordOffsets(layout, native.data)) {
      EXPECT_TRUE(output.append(
          DirectoryRecord(layout, std::span(native.data).subspan(offset))));
    }
  }

  QueryOutcome outcome;
  outcome.statusBlock.Status      = UNWRITTEN_STATUS;
  outcome.statusBlock.Information = UNWRITTEN_INFORMATION;
  outcome.result                  = output.complete(fillStatus, outcome.statusBlock);
  return outcome;
}

// Whether the filesystem pends the first query for a pattern instead of completing it
// at once, which only an asynchronous handle shows.
bool isFirstQueryPended(const std::filesystem::path& path, std::wstring_view pattern)
{
  const HANDLE directory = ::CreateFileW(
      path.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
  EXPECT_NE(INVALID_HANDLE_VALUE, directory);
  const HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  usvfs::UnicodeString fileName(pattern.data(), pattern.size());

  IO_STATUS_BLOCK statusBlock;
  alignas(8) std::array<std::byte, 0x800> buffer;
  const NTSTATUS status = nativeQueryDirectoryFile()(
      directory, event, nullptr, nullptr, &statusBlock, buffer.data(),
      static_cast<ULONG>(buffer.size()), FileDirectoryInformation, FALSE,
      static_cast<PUNICODE_STRING>(fileName), FALSE);
  if (status == STATUS_PENDING) {
    // the query writes the buffer and status block until it completes
    ::WaitForSingleObject(event, INFINITE);
  }
  ::CloseHandle(event);
  ::CloseHandle(directory);
  return status == STATUS_PENDING;
}

}  // namespace

// ntdll is the reference for how the queries of a listing complete: a query reports
// its records whether its fill ran out of room or of records, and the end of the
// listing comes with the query after the last records.
TEST(DirectoryOutputTest, CompletesTheQueriesOfAListingLikeNtdll)
{
  for (const std::filesystem::path& directory : nativeListingDirectories()) {
    SCOPED_TRACE(describeDirectory(directory));
    const std::vector<ListedQuery> queries = listNativeOutcomes(directory, L"*", 0x800);
    ASSERT_LE(3u, queries.size());
    ASSERT_EQ(STATUS_NO_MORE_FILES, queries.back().outcome.result);

    for (size_t query = 0; query + 1 < queries.size(); ++query) {
      SCOPED_TRACE(query);
      for (const NTSTATUS fillStatus : {STATUS_SUCCESS, STATUS_NO_MORE_FILES}) {
        SCOPED_TRACE(fillStatus);
        expectSameOutcome(queries[query].outcome,
                          replay(queries[query], query == 0, fillStatus));
      }
    }
    expectSameOutcome(queries.back().outcome,
                      replay(queries.back(), false, STATUS_NO_MORE_FILES));
  }
}

// ntdll is the reference for a query that fails at once, as the queries of the hook
// do. The I/O manager writes the status block of a failed query only when the
// filesystem pended it (IopCompleteRequest checks Irp->PendingReturned), which ReFS
// does here and NTFS does not, so a pended query is a reference for the result only.
TEST(DirectoryOutputTest, FailsAQueryThatMatchesNothingLikeNtdll)
{
  constexpr std::wstring_view PATTERN = L"usvfs_no_such_file_*";
  for (const std::filesystem::path& directory : nativeListingDirectories()) {
    SCOPED_TRACE(describeDirectory(directory));
    const std::vector<ListedQuery> queries =
        listNativeOutcomes(directory, PATTERN, 0x800);
    ASSERT_EQ(1u, queries.size());
    const QueryOutcome actual = replay(queries.front(), true, STATUS_NO_MORE_FILES);

    if (isFirstQueryPended(directory, PATTERN)) {
      EXPECT_EQ(queries.front().outcome.result, actual.result);
      continue;
    }
    expectSameOutcome(queries.front().outcome, actual);
  }
}

TEST(DirectoryOutputTest, PassesAnErrorOnLikeNtdll)
{
  for (const std::filesystem::path& directory : nativeListingDirectories()) {
    SCOPED_TRACE(describeDirectory(directory));
    // shorter than any record, which ntdll rejects before the filesystem sees it
    const std::vector<ListedQuery> queries = listNativeOutcomes(directory, L"*", 1);
    ASSERT_EQ(1u, queries.size());
    ASSERT_EQ(STATUS_INFO_LENGTH_MISMATCH, queries.front().outcome.result);

    expectSameOutcome(queries.front().outcome,
                      replay(queries.front(), true, queries.front().outcome.result));
  }
}

// ntdll is the reference for a record that does not fit an empty buffer: it returns
// as many bytes of it as the buffer holds, with the full FileNameLength, whether the
// buffer ends within a character of the name or not
TYPED_TEST(DirectoryRecordTest, CutsARecordThatDoesNotFitLikeNtdll)
{
  const DirectoryRecordLayout layout    = TestFixture::layout();
  const std::filesystem::path directory = generatedListingDirectory();
  const std::wstring name(64, L'n');
  const auto queryNative = [&](ULONG length) {
    const HANDLE handle = ::CreateFileW(
        directory.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    EXPECT_NE(INVALID_HANDLE_VALUE, handle);
    const ListedQuery query = queryOnce(nativeQueryDirectoryFile(), handle,
                                        {layout.informationClass, length, false, name});
    ::CloseHandle(handle);
    return query;
  };
  const ListedQuery whole = queryNative(0x1000);
  ASSERT_EQ(STATUS_SUCCESS, whole.outcome.result);

  for (const ULONG length : {layout.minimumLength, layout.minimumLength + 1}) {
    SCOPED_TRACE(length);
    const ListedQuery cut = queryNative(length);
    ASSERT_EQ(STATUS_BUFFER_OVERFLOW, cut.outcome.result);
    std::vector<std::byte> buffer(length);
    DirectoryOutput output(buffer, layout,
                           {.isSingleEntry = false, .isFirstCall = true});

    EXPECT_FALSE(output.append(DirectoryRecord(layout, whole.data)));
    EXPECT_TRUE(output.isOverflowed());
    EXPECT_EQ(cut.outcome.statusBlock.Information, output.size());
    EXPECT_EQ(cut.data, buffer);
  }
}

namespace
{

// Expects the same record at an offset, but for the link to the next one.
void expectSameRecordAt(const DirectoryRecordLayout& layout,
                        std::span<const std::byte> expected,
                        std::span<const std::byte> actual, size_t offset)
{
  // past NextEntryOffset, the first field of every record
  constexpr size_t FIELDS_OFFSET = sizeof(ULONG);
  const size_t size = DirectoryRecord(layout, expected.subspan(offset)).size();
  ASSERT_LE(offset + size, actual.size()) << "record at " << offset;
  EXPECT_TRUE(
      std::ranges::equal(expected.subspan(offset + FIELDS_OFFSET, size - FIELDS_OFFSET),
                         actual.subspan(offset + FIELDS_OFFSET, size - FIELDS_OFFSET)))
      << "record at " << offset;
}

// Expects the same records at the same offsets; the padding between them is whatever
// the buffers held.
void expectSameRecords(const DirectoryRecordLayout& layout,
                       std::span<const std::byte> expected,
                       std::span<const std::byte> actual)
{
  ASSERT_EQ(expected.empty(), actual.empty());
  if (expected.empty()) {
    return;
  }
  const std::vector<size_t> offsets = recordOffsets(layout, expected);
  ASSERT_EQ(offsets, recordOffsets(layout, actual));
  for (const size_t offset : offsets) {
    expectSameRecordAt(layout, expected, actual, offset);
  }
}

HANDLE openHookedDirectory(const std::filesystem::path& path)
{
  return hooked_NtOpenFile(path.c_str(), FILE_GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
}

}  // namespace

// ntdll is the reference for the regular entries of a directory without virtual
// entries: the hook reads them from ntdll and has none to leave out or add
TEST_F(USVFSTest, NtQueryDirectoryFileListsRegularEntriesLikeNtdll)
{
  auto params = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));

  for (const std::filesystem::path& directory : nativeListingDirectories()) {
    for (const FILE_INFORMATION_CLASS informationClass :
         DIRECTORY_INFORMATION_CLASSES) {
      for (const bool isSingleEntry : {false, true}) {
        SCOPED_TRACE(std::format("{}, class {}, {}", describeDirectory(directory),
                                 static_cast<int>(informationClass),
                                 isSingleEntry ? "single entry" : "full buffer"));
        const ListingQuery listing{informationClass, 0x800, isSingleEntry, L"*"};
        // a query moves its handle through the directory, so ntdll and the hook each
        // list through a handle of their own
        HANDLE nativeHandle = openHookedDirectory(directory);
        HANDLE hookedHandle = openHookedDirectory(directory);
        ASSERT_NE(INVALID_HANDLE_VALUE, nativeHandle);
        ASSERT_NE(INVALID_HANDLE_VALUE, hookedHandle);

        const std::vector<ListedQuery> expected =
            listQueries(nativeQueryDirectoryFile(), nativeHandle, listing);
        const std::vector<ListedQuery> actual =
            listQueries(&usvfs::hook_NtQueryDirectoryFile, hookedHandle, listing);

        ASSERT_LE(2u, expected.size());
        ASSERT_EQ(expected.size(), actual.size());
        const DirectoryRecordLayout layout =
            findDirectoryRecordLayout(informationClass).value();
        for (size_t query = 0; query < expected.size(); ++query) {
          SCOPED_TRACE(query);
          expectSameOutcome(expected[query].outcome, actual[query].outcome);
          expectSameRecords(layout, expected[query].data, actual[query].data);
        }

        usvfs::hook_NtClose(nativeHandle);
        usvfs::hook_NtClose(hookedHandle);
      }
    }
  }
}

// ntdll is the reference for where the data of a query ends: with its last record,
// without the padding after it, also when the hook leaves out the shadowed records
// after it. Every entry but "." and ".." is shadowed here, so the first query holds
// the two, as ntdll returns them into a buffer that holds no more.
TEST_F(USVFSTest, NtQueryDirectoryFileEndsWithTheLastRegularEntryLikeNtdll)
{
  auto params = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));
  const std::vector<std::filesystem::path> directories = nativeListingDirectories();
  for (const std::filesystem::path& directory : directories) {
    for (const std::wstring& name : listWithFindFirstFile(directory)) {
      if (name != L"." && name != L"..") {
        ctx->redirectionTable().addFile((directory / name).c_str(),
                                        usvfs::RedirectionDataLocal(REAL_FILEA));
      }
    }
  }

  for (const std::filesystem::path& directory : directories) {
    for (const FILE_INFORMATION_CLASS informationClass :
         DIRECTORY_INFORMATION_CLASSES) {
      SCOPED_TRACE(std::format("{}, class {}", describeDirectory(directory),
                               static_cast<int>(informationClass)));
      const ListingQuery listing{informationClass, 0x800, false, L"*"};
      HANDLE nativeHandle = openHookedDirectory(directory);
      HANDLE hookedHandle = openHookedDirectory(directory);
      ASSERT_NE(INVALID_HANDLE_VALUE, nativeHandle);
      ASSERT_NE(INVALID_HANDLE_VALUE, hookedHandle);

      const ListedQuery native =
          queryOnce(nativeQueryDirectoryFile(), nativeHandle, listing);
      const ListedQuery hooked =
          queryOnce(&usvfs::hook_NtQueryDirectoryFile, hookedHandle, listing);

      const DirectoryRecordLayout layout =
          findDirectoryRecordLayout(informationClass).value();
      const std::vector<size_t> offsets = recordOffsets(layout, native.data);
      ASSERT_LE(3u, offsets.size()) << "no shadowed record follows \"..\"";
      const size_t end =
          offsets[1] +
          DirectoryRecord(layout, std::span(native.data).subspan(offsets[1])).size();
      QueryOutcome expected            = native.outcome;
      expected.statusBlock.Information = end;
      expectSameOutcome(expected, hooked.outcome);
      ASSERT_EQ((std::vector<size_t>{0, offsets[1]}),
                recordOffsets(layout, hooked.data));
      for (const size_t offset : {offsets[0], offsets[1]}) {
        expectSameRecordAt(layout, native.data, hooked.data, offset);
      }

      usvfs::hook_NtClose(nativeHandle);
      usvfs::hook_NtClose(hookedHandle);
    }
  }
}

namespace
{

// Expects the same fields between NextEntryOffset and FileName, but for the name's
// length and the 8.3 name, which a renamed record does not take from its file.
void expectSameFixedFields(const DirectoryRecordLayout& layout,
                           std::span<const std::byte> expected,
                           std::span<const std::byte> actual)
{
  ASSERT_LE(layout.fileNameOffset, expected.size());
  ASSERT_LE(layout.fileNameOffset, actual.size());
  for (size_t offset = sizeof(ULONG); offset < layout.fileNameOffset; ++offset) {
    const bool isFileNameLength = offset >= layout.fileNameLengthOffset &&
                                  offset < layout.fileNameLengthOffset + sizeof(ULONG);
    const bool isShortName = layout.shortName &&
                             offset >= layout.shortName->lengthOffset &&
                             offset < layout.shortName->offset + 12 * sizeof(WCHAR);
    if (!isFileNameLength && !isShortName) {
      EXPECT_EQ(expected[offset], actual[offset]) << "byte " << offset;
    }
  }
}

}  // namespace

// ntdll is the reference for a virtual entry: the record ntdll lists for the file the
// entry maps to, under the virtual name, reported up to the end of that name
TEST_F(USVFSTest, NtQueryDirectoryFileListsAVirtualEntryLikeNtdllListsItsFile)
{
  constexpr std::wstring_view VIRTUAL_NAME = L"usvfs_a_name_longer_than_notepad.exe";
  auto params                              = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));
  ctx->redirectionTable().addFile((L"C:\\" + std::wstring(VIRTUAL_NAME)).c_str(),
                                  usvfs::RedirectionDataLocal(REAL_FILEA));

  for (const FILE_INFORMATION_CLASS informationClass : DIRECTORY_INFORMATION_CLASSES) {
    SCOPED_TRACE(std::format("class {}", static_cast<int>(informationClass)));
    HANDLE nativeHandle = openHookedDirectory(L"C:\\Windows");
    HANDLE hookedHandle = openHookedDirectory(L"C:\\");
    ASSERT_NE(INVALID_HANDLE_VALUE, nativeHandle);
    ASSERT_NE(INVALID_HANDLE_VALUE, hookedHandle);

    const ListedQuery native =
        queryOnce(nativeQueryDirectoryFile(), nativeHandle,
                  {informationClass, 0x1000, false, L"notepad.exe"});
    const ListedQuery hooked =
        queryOnce(&usvfs::hook_NtQueryDirectoryFile, hookedHandle,
                  {informationClass, 0x1000, false, VIRTUAL_NAME});

    ASSERT_EQ(STATUS_SUCCESS, native.outcome.result);
    const DirectoryRecordLayout layout =
        findDirectoryRecordLayout(informationClass).value();
    QueryOutcome expected = native.outcome;
    expected.statusBlock.Information =
        layout.fileNameOffset + VIRTUAL_NAME.size() * sizeof(WCHAR);
    expectSameOutcome(expected, hooked.outcome);
    const DirectoryRecord record(layout, hooked.data);
    EXPECT_EQ(0u, record.nextEntryOffset());
    EXPECT_EQ(VIRTUAL_NAME, record.fileName());
    expectSameFixedFields(layout, native.data, hooked.data);

    usvfs::hook_NtClose(nativeHandle);
    usvfs::hook_NtClose(hookedHandle);
  }
}

// A virtual entry has no 8.3 name, as an entry of a volume that makes none: the 8.3
// name of its file belongs to another directory, where the redirection tree does not
// find it, and GetShortPathNameW builds paths of the 8.3 names that listings return.
TEST_F(USVFSTest, NtQueryDirectoryFileListsVirtualEntriesWithoutShortNames)
{
  // a directory that Windows setup gives an 8.3 name on nearly every system
  const std::filesystem::path sourceParent = L"C:\\";
  constexpr std::wstring_view SOURCE_NAME  = L"Program Files";
  constexpr std::wstring_view VIRTUAL_NAME = L"usvfs_virtual_program_files";
  auto params                              = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));
  const std::filesystem::path directory =
      test::path_of_test_temp(L"virtual_short_names");
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  ctx->redirectionTable().addDirectory(
      (directory / VIRTUAL_NAME).c_str(),
      usvfs::RedirectionDataLocal((sourceParent / SOURCE_NAME).string() + "\\"),
      usvfs::shared::FLAG_DIRECTORY, false);

  for (const FILE_INFORMATION_CLASS informationClass : DIRECTORY_INFORMATION_CLASSES) {
    const DirectoryRecordLayout layout =
        findDirectoryRecordLayout(informationClass).value();
    if (!layout.shortName) {
      continue;
    }
    SCOPED_TRACE(std::format("class {}", static_cast<int>(informationClass)));
    HANDLE nativeHandle = openHookedDirectory(sourceParent);
    HANDLE hookedHandle = openHookedDirectory(directory);
    ASSERT_NE(INVALID_HANDLE_VALUE, nativeHandle);
    ASSERT_NE(INVALID_HANDLE_VALUE, hookedHandle);

    const ListedQuery native =
        queryOnce(nativeQueryDirectoryFile(), nativeHandle,
                  {informationClass, 0x1000, false, SOURCE_NAME});
    const ListedQuery hooked =
        queryOnce(&usvfs::hook_NtQueryDirectoryFile, hookedHandle,
                  {informationClass, 0x1000, false, VIRTUAL_NAME});
    usvfs::hook_NtClose(nativeHandle);
    usvfs::hook_NtClose(hookedHandle);

    ASSERT_EQ(STATUS_SUCCESS, native.outcome.result);
    ASSERT_EQ(STATUS_SUCCESS, hooked.outcome.result);
    const size_t shortNameEnd = layout.shortName->offset + 12 * sizeof(WCHAR);
    if (native.data[layout.shortName->lengthOffset] == std::byte{0}) {
      GTEST_SKIP() << "the source has no 8.3 name on this volume";
    }
    EXPECT_TRUE(
        std::ranges::all_of(std::span(hooked.data)
                                .subspan(layout.shortName->lengthOffset,
                                         shortNameEnd - layout.shortName->lengthOffset),
                            [](std::byte value) {
                              return value == std::byte{0};
                            }));
    expectSameFixedFields(layout, native.data, hooked.data);
  }
}

// ntdll is the reference for a virtual entry whose file has a longer name: the record
// ntdll lists for the file, under the virtual name, in a buffer that holds it under
// that name only
TEST_F(USVFSTest, NtQueryDirectoryFileListsAVirtualEntryThatFitsWhereItsFileWouldNot)
{
  constexpr std::wstring_view VIRTUAL_NAME = L"usvfs_v.txt";
  constexpr std::wstring_view FILE_NAME =
      L"usvfs_a_name_longer_than_the_virtual_one.txt";
  const std::filesystem::path directory = test::path_of_test_temp(L"virtual_file");
  std::filesystem::create_directories(directory);
  std::ofstream(directory / FILE_NAME);
  auto params = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));
  ctx->redirectionTable().addFile(
      (L"C:\\" + std::wstring(VIRTUAL_NAME)).c_str(),
      usvfs::RedirectionDataLocal((directory / FILE_NAME).string()));

  for (const FILE_INFORMATION_CLASS informationClass : DIRECTORY_INFORMATION_CLASSES) {
    SCOPED_TRACE(std::format("class {}", static_cast<int>(informationClass)));
    const DirectoryRecordLayout layout =
        findDirectoryRecordLayout(informationClass).value();
    const ULONG length =
        layout.fileNameOffset + static_cast<ULONG>(VIRTUAL_NAME.size() * sizeof(WCHAR));
    HANDLE nativeHandle = openHookedDirectory(directory);
    HANDLE hookedHandle = openHookedDirectory(L"C:\\");
    ASSERT_NE(INVALID_HANDLE_VALUE, nativeHandle);
    ASSERT_NE(INVALID_HANDLE_VALUE, hookedHandle);

    const ListedQuery native = queryOnce(nativeQueryDirectoryFile(), nativeHandle,
                                         {informationClass, 0x1000, false, FILE_NAME});
    const ListedQuery hooked =
        queryOnce(&usvfs::hook_NtQueryDirectoryFile, hookedHandle,
                  {informationClass, length, false, VIRTUAL_NAME});

    ASSERT_EQ(STATUS_SUCCESS, native.outcome.result);
    QueryOutcome expected            = native.outcome;
    expected.statusBlock.Information = length;
    expectSameOutcome(expected, hooked.outcome);
    const DirectoryRecord record(layout, hooked.data);
    EXPECT_EQ(0u, record.nextEntryOffset());
    EXPECT_EQ(VIRTUAL_NAME, record.fileName());
    expectSameFixedFields(layout, native.data, hooked.data);

    usvfs::hook_NtClose(nativeHandle);
    usvfs::hook_NtClose(hookedHandle);
  }
}

// ntdll is the reference for a first record that does not fit: it returns as much of
// it as fits, with STATUS_BUFFER_OVERFLOW, again on every query until a buffer holds
// it, and the listing goes on from there
TEST_F(USVFSTest, NtQueryDirectoryFileListsPastAPartialRecordLikeNtdll)
{
  // queries enough to list either directory several times over
  constexpr size_t MAXIMUM_QUERIES = 1000;
  auto params                      = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));

  for (const std::filesystem::path& directory : nativeListingDirectories()) {
    for (const FILE_INFORMATION_CLASS informationClass :
         DIRECTORY_INFORMATION_CLASSES) {
      SCOPED_TRACE(std::format("{}, class {}", describeDirectory(directory),
                               static_cast<int>(informationClass)));
      const DirectoryRecordLayout layout =
          findDirectoryRecordLayout(informationClass).value();
      // a query moves its handle through the directory, so ntdll and the hook each
      // list through a handle of their own
      HANDLE nativeHandle = openHookedDirectory(directory);
      HANDLE hookedHandle = openHookedDirectory(directory);
      ASSERT_NE(INVALID_HANDLE_VALUE, nativeHandle);
      ASSERT_NE(INVALID_HANDLE_VALUE, hookedHandle);

      std::vector<ListedQuery> expected;
      std::vector<ListedQuery> actual;
      const auto queryBoth = [&](ULONG length) {
        const ListingQuery listing{informationClass, length, false, L"*"};
        expected.push_back(
            queryOnce(nativeQueryDirectoryFile(), nativeHandle, listing));
        actual.push_back(
            queryOnce(&usvfs::hook_NtQueryDirectoryFile, hookedHandle, listing));
        return expected.back().outcome.result;
      };
      // the smallest buffer ntdll accepts, which a longer name overflows
      while (queryBoth(layout.minimumLength) == STATUS_SUCCESS &&
             expected.size() < MAXIMUM_QUERIES) {
      }
      ASSERT_EQ(STATUS_BUFFER_OVERFLOW, expected.back().outcome.result);
      queryBoth(layout.minimumLength);
      while (queryBoth(0x1000) == STATUS_SUCCESS && expected.size() < MAXIMUM_QUERIES) {
      }
      ASSERT_EQ(STATUS_NO_MORE_FILES, expected.back().outcome.result);

      for (size_t query = 0; query < expected.size(); ++query) {
        SCOPED_TRACE(query);
        expectSameOutcome(expected[query].outcome, actual[query].outcome);
        if (expected[query].outcome.result == STATUS_BUFFER_OVERFLOW) {
          EXPECT_EQ(expected[query].data, actual[query].data) << "the partial record";
        } else {
          expectSameRecords(layout, expected[query].data, actual[query].data);
        }
      }

      usvfs::hook_NtClose(nativeHandle);
      usvfs::hook_NtClose(hookedHandle);
    }
  }
}

// ntdll is the reference for a virtual entry that does not fit: the record it cuts
// short for a file of the virtual name, with the fields of the file the entry maps to
TEST_F(USVFSTest, NtQueryDirectoryFileCutsAVirtualEntryThatDoesNotFitLikeNtdll)
{
  constexpr std::wstring_view VIRTUAL_NAME = L"usvfs_a_name_longer_than_notepad.exe";
  const std::filesystem::path namesakeDirectory =
      test::path_of_test_temp(L"virtual_namesake");
  std::filesystem::create_directories(namesakeDirectory);
  std::ofstream(namesakeDirectory / VIRTUAL_NAME);
  auto params = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));
  ctx->redirectionTable().addFile((L"C:\\" + std::wstring(VIRTUAL_NAME)).c_str(),
                                  usvfs::RedirectionDataLocal(REAL_FILEA));

  for (const FILE_INFORMATION_CLASS informationClass : DIRECTORY_INFORMATION_CLASSES) {
    const DirectoryRecordLayout layout =
        findDirectoryRecordLayout(informationClass).value();
    HANDLE fileHandle = openHookedDirectory(L"C:\\Windows");
    ASSERT_NE(INVALID_HANDLE_VALUE, fileHandle);
    const ListedQuery file =
        queryOnce(nativeQueryDirectoryFile(), fileHandle,
                  {informationClass, 0x1000, false, L"notepad.exe"});
    ASSERT_EQ(STATUS_SUCCESS, file.outcome.result);
    usvfs::hook_NtClose(fileHandle);

    // the first holds neither the record of the file the entry maps to nor the one
    // under the virtual name, the second holds the record of the file only
    for (const ULONG length :
         {layout.minimumLength, static_cast<ULONG>(file.data.size())}) {
      SCOPED_TRACE(std::format("class {}, length {}",
                               static_cast<int>(informationClass), length));
      HANDLE namesakeHandle = openHookedDirectory(namesakeDirectory);
      HANDLE hookedHandle   = openHookedDirectory(L"C:\\");
      ASSERT_NE(INVALID_HANDLE_VALUE, namesakeHandle);
      ASSERT_NE(INVALID_HANDLE_VALUE, hookedHandle);

      const ListedQuery namesake =
          queryOnce(nativeQueryDirectoryFile(), namesakeHandle,
                    {informationClass, length, false, VIRTUAL_NAME});
      const ListedQuery hooked =
          queryOnce(&usvfs::hook_NtQueryDirectoryFile, hookedHandle,
                    {informationClass, length, false, VIRTUAL_NAME});

      ASSERT_EQ(STATUS_BUFFER_OVERFLOW, namesake.outcome.result);
      expectSameOutcome(namesake.outcome, hooked.outcome);
      ASSERT_EQ(namesake.data.size(), hooked.data.size());
      const DirectoryRecord record(layout, hooked.data);
      EXPECT_EQ(0u, record.nextEntryOffset());
      EXPECT_EQ(DirectoryRecord(layout, namesake.data).size(), record.size());
      EXPECT_TRUE(
          std::ranges::equal(std::span(namesake.data).subspan(layout.fileNameOffset),
                             std::span(hooked.data).subspan(layout.fileNameOffset)))
          << "the name, as much of it as fits";
      expectSameFixedFields(layout, file.data, hooked.data);

      usvfs::hook_NtClose(namesakeHandle);
      usvfs::hook_NtClose(hookedHandle);
    }
  }
}

// ntdll is the reference for what follows the partial record of a virtual entry, as
// it lists a file of the virtual name: the same partial record on every query until a
// buffer holds the entry, then the entry, then the end of the listing
TEST_F(USVFSTest, NtQueryDirectoryFileListsPastAVirtualPartialRecordLikeNtdll)
{
  constexpr std::wstring_view VIRTUAL_NAME = L"usvfs_a_name_longer_than_notepad.exe";
  const std::filesystem::path namesakeDirectory =
      test::path_of_test_temp(L"virtual_namesake");
  std::filesystem::create_directories(namesakeDirectory);
  std::ofstream(namesakeDirectory / VIRTUAL_NAME);
  auto params = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));
  ctx->redirectionTable().addFile((L"C:\\" + std::wstring(VIRTUAL_NAME)).c_str(),
                                  usvfs::RedirectionDataLocal(REAL_FILEA));

  for (const FILE_INFORMATION_CLASS informationClass : DIRECTORY_INFORMATION_CLASSES) {
    SCOPED_TRACE(std::format("class {}", static_cast<int>(informationClass)));
    const DirectoryRecordLayout layout =
        findDirectoryRecordLayout(informationClass).value();
    // holds the record of the file the entry maps to, and not the one under the
    // virtual name
    const ULONG cutLength = layout.fileNameOffset +
                            static_cast<ULONG>(sizeof(L"notepad.exe") - sizeof(WCHAR));
    // a query moves its handle through the directory, so ntdll and the hook each list
    // through a handle of their own
    HANDLE namesakeHandle = openHookedDirectory(namesakeDirectory);
    HANDLE hookedHandle   = openHookedDirectory(L"C:\\");
    ASSERT_NE(INVALID_HANDLE_VALUE, namesakeHandle);
    ASSERT_NE(INVALID_HANDLE_VALUE, hookedHandle);

    std::vector<ListedQuery> expected;
    std::vector<ListedQuery> actual;
    for (const ULONG length : {cutLength, cutLength, 0x1000ul, 0x1000ul}) {
      const ListingQuery listing{informationClass, length, false, VIRTUAL_NAME};
      expected.push_back(
          queryOnce(nativeQueryDirectoryFile(), namesakeHandle, listing));
      actual.push_back(
          queryOnce(&usvfs::hook_NtQueryDirectoryFile, hookedHandle, listing));
    }
    ASSERT_EQ(STATUS_BUFFER_OVERFLOW, expected[1].outcome.result);
    ASSERT_EQ(STATUS_NO_MORE_FILES, expected.back().outcome.result);

    for (size_t query = 0; query < expected.size(); ++query) {
      SCOPED_TRACE(query);
      expectSameOutcome(expected[query].outcome, actual[query].outcome);
      ASSERT_EQ(expected[query].data.size(), actual[query].data.size());
      if (expected[query].data.empty()) {
        continue;
      }
      // the fields before the name are those of the file the entry maps to
      EXPECT_EQ(DirectoryRecord(layout, expected[query].data).size(),
                DirectoryRecord(layout, actual[query].data).size());
      EXPECT_TRUE(std::ranges::equal(
          std::span(expected[query].data).subspan(layout.fileNameOffset),
          std::span(actual[query].data).subspan(layout.fileNameOffset)))
          << "the name, as much of it as fits";
    }

    usvfs::hook_NtClose(namesakeHandle);
    usvfs::hook_NtClose(hookedHandle);
  }
}

namespace
{

std::vector<std::wstring> recordNames(const DirectoryRecordLayout& layout,
                                      std::span<const std::byte> data)
{
  std::vector<std::wstring> names;
  for (const size_t offset : recordOffsets(layout, data)) {
    names.push_back(DirectoryRecord(layout, data.subspan(offset)).fileName());
  }
  return names;
}

// Expects the records of a query that returned whole records to be packed as
// FitsARecordThatEndsWithTheBuffer has the rule: each at the 8-byte boundary after the
// one before it, the data ending with the last, and no room left for the record of the
// entry the listing goes on with, if any. A single-entry query returns one record.
void expectPackedRecords(const ListingQuery& listing, std::span<const std::byte> data,
                         std::optional<std::wstring_view> nextName)
{
  const DirectoryRecordLayout layout =
      findDirectoryRecordLayout(listing.informationClass).value();
  const std::vector<size_t> offsets = recordOffsets(layout, data);
  for (size_t index = 0; index + 1 < offsets.size(); ++index) {
    const size_t size = DirectoryRecord(layout, data.subspan(offsets[index])).size();
    EXPECT_EQ((size + 7) / 8 * 8, offsets[index + 1] - offsets[index])
        << "record at " << offsets[index];
  }
  EXPECT_EQ(data.size(),
            offsets.back() +
                DirectoryRecord(layout, data.subspan(offsets.back())).size())
      << "the data ends past the last record";

  if (listing.isSingleEntry) {
    EXPECT_EQ(1u, offsets.size());
    return;
  }
  if (nextName) {
    const size_t nextEnd = (data.size() + 7) / 8 * 8 + layout.fileNameOffset +
                           nextName->size() * sizeof(WCHAR);
    EXPECT_LT(listing.length, nextEnd)
        << "the record of the next entry fits after " << offsets.size();
  }
}

}  // namespace

// ntdll is the reference for virtual entries that map files of several directories,
// some of them with one file mapped, as it lists files of the virtual names: every
// entry whose file exists, also when the mapping names it in another case, and none
// twice or out of order, also when a query asks for another information class than
// the one before it. ntdll is no reference for where a query ends: NTFS can stop short
// of a record that fits, as it does for FileDirectoryInformation and a buffer that ends
// where the fourth record starts on some systems, so the listings are compared as a
// whole, and each query of the hook is held to the rule of
// FitsARecordThatEndsWithTheBuffer instead: as many as the buffer holds, the first that
// does not fit starting the next query. The pattern leaves out "." and "..", so the
// regular entries end before the first query writes any.
TEST_F(USVFSTest, NtQueryDirectoryFileListsEntriesOfSeveralSourcesLikeNtdll)
{
  constexpr int ENTRY_COUNT = 12;
  // the entries whose file exists, with the two that map the one file of a directory
  constexpr size_t LISTED_COUNT = ENTRY_COUNT - 1 + 2;
  // a guard against a listing that does not end
  constexpr size_t MAXIMUM_QUERIES    = 1000;
  constexpr std::wstring_view PATTERN = L"usvfs_entry_*";
  const std::filesystem::path root    = test::path_of_test_temp(L"several_sources");
  std::filesystem::remove_all(root);
  const std::filesystem::path namesakeDirectory = root / L"namesakes";
  const std::filesystem::path virtualDirectory  = root / L"virtual";
  const std::array sourceDirectories{root / L"source_a", root / L"source_b"};
  for (const std::filesystem::path& directory :
       {namesakeDirectory, virtualDirectory, sourceDirectories[0],
        sourceDirectories[1]}) {
    std::filesystem::create_directories(directory);
  }
  auto params = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));
  for (int entry = 0; entry < ENTRY_COUNT; ++entry) {
    const std::wstring name = std::format(L"usvfs_entry_{:02}.txt", entry);
    // the entries alternate between the sources, so that the scans interleave
    const std::filesystem::path& source = sourceDirectories[entry % 2];
    const std::wstring sourceName       = std::format(L"source_{}.txt", entry);
    std::filesystem::path linkTarget    = source / sourceName;
    if (entry == 5) {
      // the file exists under the name in another case
      linkTarget = source / std::format(L"SOURCE_{}.TXT", entry);
    }
    if (entry == 8) {
      // the file does not exist, so the entry is not listed
      linkTarget = source / L"missing.txt";
    } else {
      std::ofstream(source / sourceName);
      std::ofstream(namesakeDirectory / name);
    }
    ctx->redirectionTable().addFile((virtualDirectory / name).c_str(),
                                    usvfs::RedirectionDataLocal(linkTarget.string()));
  }
  // a directory with one file, which two entries map under the name in another case,
  // and a directory whose one mapped file does not exist
  const std::filesystem::path singleSource  = root / L"source_c";
  const std::filesystem::path missingSource = root / L"source_d";
  std::filesystem::create_directories(singleSource);
  std::filesystem::create_directories(missingSource);
  std::ofstream(singleSource / L"single.txt");
  for (const std::wstring_view name : {L"usvfs_entry_12.txt", L"usvfs_entry_13.txt"}) {
    std::ofstream(namesakeDirectory / name);
    ctx->redirectionTable().addFile(
        (virtualDirectory / name).c_str(),
        usvfs::RedirectionDataLocal((singleSource / L"SINGLE.TXT").string()));
  }
  ctx->redirectionTable().addFile(
      (virtualDirectory / L"usvfs_entry_14.txt").c_str(),
      usvfs::RedirectionDataLocal((missingSource / L"missing.txt").string()));

  for (const FILE_INFORMATION_CLASS informationClass : DIRECTORY_INFORMATION_CLASSES) {
    const DirectoryRecordLayout layout =
        findDirectoryRecordLayout(informationClass).value();
    HANDLE wholeHandle = openHookedDirectory(namesakeDirectory);
    ASSERT_NE(INVALID_HANDLE_VALUE, wholeHandle);
    const ListedQuery whole = queryOnce(nativeQueryDirectoryFile(), wholeHandle,
                                        {informationClass, 0x1000, false, PATTERN});
    usvfs::hook_NtClose(wholeHandle);
    const std::vector<size_t> offsets = recordOffsets(layout, whole.data);
    ASSERT_EQ(LISTED_COUNT, offsets.size());

    struct ListingCase
    {
      ULONG length;
      bool isSingleEntry;
      bool isClassChanging;
    };
    // a buffer that holds every entry and one that ends where the fourth record
    // starts, so that it holds three, each also for a single entry, and the second
    // with the class changed on every query
    const auto cutLength = static_cast<ULONG>(offsets[3]);
    for (const auto& [length, isSingleEntry, isClassChanging] :
         {ListingCase{0x1000, false, false}, ListingCase{0x1000, true, false},
          ListingCase{cutLength, false, false}, ListingCase{cutLength, true, false},
          ListingCase{cutLength, false, true}}) {
      SCOPED_TRACE(std::format("class {}, length {}, {}{}",
                               static_cast<int>(informationClass), length,
                               isSingleEntry ? "single entry" : "full buffer",
                               isClassChanging ? ", class changing" : ""));
      HANDLE namesakeHandle = openHookedDirectory(namesakeDirectory);
      HANDLE hookedHandle   = openHookedDirectory(virtualDirectory);
      ASSERT_NE(INVALID_HANDLE_VALUE, namesakeHandle);
      ASSERT_NE(INVALID_HANDLE_VALUE, hookedHandle);

      const size_t firstClassIndex =
          std::ranges::find(DIRECTORY_INFORMATION_CLASSES, informationClass) -
          std::begin(DIRECTORY_INFORMATION_CLASSES);
      const auto listingOf = [&](size_t query) {
        const size_t classIndex =
            isClassChanging ? firstClassIndex + query : firstClassIndex;
        return ListingQuery{
            DIRECTORY_INFORMATION_CLASSES[classIndex %
                                          std::size(DIRECTORY_INFORMATION_CLASSES)],
            length, isSingleEntry, PATTERN};
      };
      // a class whose record does not fit the buffer gets a partial record, and the
      // listing goes on with the class of the next query
      const auto list = [&](NtQueryDirectoryFile_type query, HANDLE directory) {
        std::vector<ListedQuery> queries;
        NTSTATUS result;
        do {
          queries.push_back(queryOnce(query, directory, listingOf(queries.size())));
          result = queries.back().outcome.result;
        } while ((result == STATUS_SUCCESS || result == STATUS_BUFFER_OVERFLOW) &&
                 queries.size() < MAXIMUM_QUERIES);
        return queries;
      };
      const auto listedNames = [&](const std::vector<ListedQuery>& queries) {
        std::vector<std::wstring> names;
        for (size_t query = 0; query < queries.size(); ++query) {
          if (queries[query].outcome.result != STATUS_SUCCESS) {
            continue;
          }
          const DirectoryRecordLayout queryLayout =
              findDirectoryRecordLayout(listingOf(query).informationClass).value();
          std::ranges::move(recordNames(queryLayout, queries[query].data),
                            std::back_inserter(names));
        }
        return names;
      };
      const std::vector<ListedQuery> expected =
          list(nativeQueryDirectoryFile(), namesakeHandle);
      const std::vector<ListedQuery> actual =
          list(&usvfs::hook_NtQueryDirectoryFile, hookedHandle);
      usvfs::hook_NtClose(namesakeHandle);
      usvfs::hook_NtClose(hookedHandle);

      const std::vector<std::wstring> expectedNames = listedNames(expected);
      ASSERT_EQ(LISTED_COUNT, expectedNames.size());
      EXPECT_EQ(expectedNames, listedNames(actual));
      expectSameOutcome(expected.back().outcome, actual.back().outcome);

      size_t listedCount = 0;
      for (size_t query = 0; query + 1 < actual.size(); ++query) {
        SCOPED_TRACE(query);
        const ListingQuery listing = listingOf(query);
        const ListedQuery& listed  = actual[query];
        const DirectoryRecordLayout queryLayout =
            findDirectoryRecordLayout(listing.informationClass).value();
        ASSERT_LT(listedCount, expectedNames.size());
        if (listed.outcome.result == STATUS_BUFFER_OVERFLOW) {
          EXPECT_LT(listing.length,
                    queryLayout.fileNameOffset +
                        expectedNames[listedCount].size() * sizeof(WCHAR))
              << "a partial record of an entry that fits";
          continue;
        }
        EXPECT_EQ(STATUS_SUCCESS, listed.outcome.statusBlock.Status);
        ASSERT_FALSE(listed.data.empty());
        listedCount += recordOffsets(queryLayout, listed.data).size();
        const std::optional<std::wstring_view> nextName =
            listedCount < expectedNames.size()
                ? std::optional<std::wstring_view>(expectedNames[listedCount])
                : std::nullopt;
        expectPackedRecords(listing, listed.data, nextName);
      }
    }
  }
}

namespace
{

// The names a listing holds, sorted: the virtual entries follow the regular ones in an
// order of their own, so a test that mixes them compares only which names are listed.
std::vector<std::wstring> sortedNames(const std::vector<ListedQuery>& queries)
{
  const DirectoryRecordLayout layout =
      findDirectoryRecordLayout(FileDirectoryInformation).value();
  std::vector<std::wstring> names;
  for (const ListedQuery& query : queries) {
    if (!query.data.empty()) {
      std::ranges::move(recordNames(layout, query.data), std::back_inserter(names));
    }
  }
  std::ranges::sort(names);
  return names;
}

}  // namespace

// ntdll is the reference for which virtual entries a pattern matches, as it matches
// files of the virtual names: without regard to case, beyond ASCII too, with the DOS
// wildcards "<", ">" and "\"" as the filesystem reads them, and with nothing of what
// cmd.exe makes of a pattern.
TEST_F(USVFSTest, NtQueryDirectoryFileMatchesVirtualEntriesLikeNtdll)
{
  const std::filesystem::path root = test::path_of_test_temp(L"virtual_patterns");
  std::filesystem::remove_all(root);
  const std::filesystem::path namesakeDirectory = root / L"namesakes";
  const std::filesystem::path virtualDirectory  = root / L"virtual";
  const std::filesystem::path sourceDirectory   = root / L"source";
  for (const std::filesystem::path& directory :
       {namesakeDirectory, virtualDirectory, sourceDirectory}) {
    std::filesystem::create_directories(directory);
  }
  auto params = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));
  const std::wstring_view names[] = {L"readme.txt", L"Report.TXT", L"a.txt",
                                     L"ab.txt",     L"noext",      L"two.dots.txt",
                                     L".hidden",    L"foo",        L"foo.bar",
                                     L"Ärger.dat",  L"über.txt"};
  for (size_t index = 0; index < std::size(names); ++index) {
    const std::filesystem::path sourceFile =
        sourceDirectory / std::format(L"source_{}.txt", index);
    std::ofstream(namesakeDirectory / names[index]);
    std::ofstream{sourceFile};
    // in UTF-8, as usvfsVirtualLinkFile adds it: a wide path would go through the
    // locale of the Boost.Filesystem of this executable, which is not UTF-8
    ctx->redirectionTable().addFile(
        usvfs::shared::string_cast<std::string>(
            (virtualDirectory / names[index]).wstring(), usvfs::shared::CodePage::UTF8),
        usvfs::RedirectionDataLocal(sourceFile.string()));
  }

  const std::wstring_view patterns[] = {
      L"*",       L"*.*",       L"*.txt",  L"*.TXT",     L"?.txt",
      L"??.txt",  L"a*",        L"foo.*",  L"foo*",      L"*.",
      L"noext.",  L".hidden",   L"hidden", L"äRGER.DAT", L"Über.txt",
      L"<.txt",   L"<",         L"foo>",   L">.txt",     L"readme\"txt",
      L"noext\"", L"foo.bar\"", L"two<",   L"missing",   L""};
  for (const std::wstring_view pattern : patterns) {
    SCOPED_TRACE(std::format(
        "pattern \"{}\"", usvfs::shared::string_cast<std::string>(
                              std::wstring(pattern), usvfs::shared::CodePage::UTF8)));
    const ListingQuery listing{FileDirectoryInformation, 0x1000, false, pattern};
    HANDLE namesakeHandle = openHookedDirectory(namesakeDirectory);
    HANDLE hookedHandle   = openHookedDirectory(virtualDirectory);
    ASSERT_NE(INVALID_HANDLE_VALUE, namesakeHandle);
    ASSERT_NE(INVALID_HANDLE_VALUE, hookedHandle);

    const std::vector<ListedQuery> expected =
        listQueries(nativeQueryDirectoryFile(), namesakeHandle, listing);
    const std::vector<ListedQuery> actual =
        listQueries(&usvfs::hook_NtQueryDirectoryFile, hookedHandle, listing);

    EXPECT_EQ(expected.front().outcome.result, actual.front().outcome.result);
    EXPECT_EQ(sortedNames(expected), sortedNames(actual));

    usvfs::hook_NtClose(namesakeHandle);
    usvfs::hook_NtClose(hookedHandle);
  }
}

// ntdll is the reference for a directory read that fails, also where virtual entries
// would follow: the error, on every query. The I/O manager refuses a handle without
// FILE_LIST_DIRECTORY before the filesystem sees the query, so every filesystem fails
// it alike.
TEST_F(USVFSTest, NtQueryDirectoryFilePassesAFailedRegularReadOnLikeNtdll)
{
  // from ntstatus.h, which ntdll_declarations.h copies only what usvfs uses of
  constexpr auto STATUS_ACCESS_DENIED = static_cast<NTSTATUS>(0xC0000022L);

  auto params = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));
  const std::vector<std::filesystem::path> directories = nativeListingDirectories();
  for (const std::filesystem::path& directory : directories) {
    ctx->redirectionTable().addFile((directory / L"usvfs_virtual.txt").c_str(),
                                    usvfs::RedirectionDataLocal(REAL_FILEA));
  }

  for (const std::filesystem::path& directory : directories) {
    SCOPED_TRACE(describeDirectory(directory));
    const auto openUnlistable = [&] {
      return hooked_NtOpenFile(directory.c_str(), FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
    };
    HANDLE nativeHandle = openUnlistable();
    HANDLE hookedHandle = openUnlistable();
    ASSERT_NE(INVALID_HANDLE_VALUE, nativeHandle);
    ASSERT_NE(INVALID_HANDLE_VALUE, hookedHandle);

    const ListingQuery listing{FileDirectoryInformation, 0x1000, false, L"*"};
    for (int query = 0; query < 2; ++query) {
      SCOPED_TRACE(query);
      const ListedQuery expected =
          queryOnce(nativeQueryDirectoryFile(), nativeHandle, listing);
      const ListedQuery actual =
          queryOnce(&usvfs::hook_NtQueryDirectoryFile, hookedHandle, listing);
      ASSERT_EQ(STATUS_ACCESS_DENIED, expected.outcome.result);
      expectSameOutcome(expected.outcome, actual.outcome);
    }

    usvfs::hook_NtClose(nativeHandle);
    usvfs::hook_NtClose(hookedHandle);
  }
}

// ntdll is the reference for a restart in the middle of a listing: what the scan
// lists from there with the pattern the restart brings, or without one
TEST_F(USVFSTest, NtQueryDirectoryFileRestartsTheRegularEntriesLikeNtdll)
{
  // queries enough to list either directory several times over
  constexpr size_t MAXIMUM_QUERIES = 1000;
  struct Restart
  {
    std::wstring_view firstPattern;
    std::optional<std::wstring_view> restartPattern;
  };
  auto params = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));

  for (const std::filesystem::path& directory : nativeListingDirectories()) {
    for (const Restart& restart :
         {Restart{L"*", L"*"}, Restart{L"*", L"n*"}, Restart{L"n*", std::nullopt}}) {
      SCOPED_TRACE(std::format(
          "{}, {} then {}", describeDirectory(directory),
          std::filesystem::path(restart.firstPattern).string(),
          std::filesystem::path(restart.restartPattern.value_or(L"<null>")).string()));
      const DirectoryRecordLayout layout =
          findDirectoryRecordLayout(FileDirectoryInformation).value();
      // a query moves its handle through the directory, so ntdll and the hook each
      // list through a handle of their own
      HANDLE nativeHandle = openHookedDirectory(directory);
      HANDLE hookedHandle = openHookedDirectory(directory);
      ASSERT_NE(INVALID_HANDLE_VALUE, nativeHandle);
      ASSERT_NE(INVALID_HANDLE_VALUE, hookedHandle);

      std::vector<ListedQuery> expected;
      std::vector<ListedQuery> actual;
      const auto queryBoth = [&](const ListingQuery& listing) {
        expected.push_back(
            queryOnce(nativeQueryDirectoryFile(), nativeHandle, listing));
        actual.push_back(
            queryOnce(&usvfs::hook_NtQueryDirectoryFile, hookedHandle, listing));
        return expected.back().outcome.result;
      };
      // small enough for the listing to go on after it
      ASSERT_EQ(STATUS_SUCCESS, queryBoth({FileDirectoryInformation, 0x800, false,
                                           restart.firstPattern}));
      NTSTATUS result = queryBoth(
          {FileDirectoryInformation, 0x800, false, restart.restartPattern, true});
      while (result == STATUS_SUCCESS && expected.size() < MAXIMUM_QUERIES) {
        result = queryBoth({FileDirectoryInformation, 0x800, false, L"*"});
      }
      ASSERT_EQ(STATUS_NO_MORE_FILES, result);

      for (size_t query = 0; query < expected.size(); ++query) {
        SCOPED_TRACE(query);
        expectSameOutcome(expected[query].outcome, actual[query].outcome);
        expectSameRecords(layout, expected[query].data, actual[query].data);
      }

      usvfs::hook_NtClose(nativeHandle);
      usvfs::hook_NtClose(hookedHandle);
    }
  }
}

// ntdll is the reference for a restart that matches nothing: STATUS_NO_MORE_FILES,
// as NTFS and ReFS keep STATUS_NO_SUCH_FILE for the first query of a handle. A
// filesystem that pends the query reports it in the status block too (see
// FailsAQueryThatMatchesNothingLikeNtdll), so a pended one is a reference for the
// result only.
TEST_F(USVFSTest, NtQueryDirectoryFileEndsAnEmptyRestartLikeNtdll)
{
  constexpr std::wstring_view NO_MATCH = L"usvfs_no_such_file_*";
  auto params                          = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));

  for (const std::filesystem::path& directory : nativeListingDirectories()) {
    SCOPED_TRACE(describeDirectory(directory));
    HANDLE nativeHandle = openHookedDirectory(directory);
    HANDLE hookedHandle = openHookedDirectory(directory);
    ASSERT_NE(INVALID_HANDLE_VALUE, nativeHandle);
    ASSERT_NE(INVALID_HANDLE_VALUE, hookedHandle);

    const bool isPended = isFirstQueryPended(directory, NO_MATCH);
    for (const ListingQuery& listing :
         {ListingQuery{FileDirectoryInformation, 0x800, false, L"*"},
          ListingQuery{FileDirectoryInformation, 0x800, false, NO_MATCH, true},
          ListingQuery{FileDirectoryInformation, 0x800, false, L"*"}}) {
      SCOPED_TRACE(listing.isRestart ? "restart" : "query");
      const ListedQuery expected =
          queryOnce(nativeQueryDirectoryFile(), nativeHandle, listing);
      const ListedQuery actual =
          queryOnce(&usvfs::hook_NtQueryDirectoryFile, hookedHandle, listing);
      if (listing.isRestart && isPended) {
        EXPECT_EQ(expected.outcome.result, actual.outcome.result);
      } else {
        expectSameOutcome(expected.outcome, actual.outcome);
      }
      EXPECT_EQ(expected.data.size(), actual.data.size());
    }

    usvfs::hook_NtClose(nativeHandle);
    usvfs::hook_NtClose(hookedHandle);
  }
}

// ntdll is the reference for the pattern a restart lists with, as NTFS and ReFS latch
// it: that of the first query, or every name without one, until a restart brings one
// that is not empty; a query that does not restart ignores its pattern. The virtual
// entries follow it as the regular ones do, which the scan reads through a handle of
// its own.
TEST_F(USVFSTest, NtQueryDirectoryFileRestartsWithTheLatchedPatternLikeNtdll)
{
  // a guard against a listing that does not end
  constexpr size_t MAXIMUM_QUERIES = 100;
  const std::filesystem::path root = test::path_of_test_temp(L"latched_pattern");
  std::filesystem::remove_all(root);
  const std::filesystem::path namesakeDirectory = root / L"namesakes";
  const std::filesystem::path virtualDirectory  = root / L"virtual";
  const std::filesystem::path sourceDirectory   = root / L"source";
  for (const std::filesystem::path& directory :
       {namesakeDirectory, virtualDirectory, sourceDirectory}) {
    std::filesystem::create_directories(directory);
  }
  auto params = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));
  for (const std::wstring_view name : {L"a_regular.txt", L"b_regular.txt"}) {
    std::ofstream{namesakeDirectory / name};
    std::ofstream{virtualDirectory / name};
  }
  for (const std::wstring_view name : {L"a_virtual.txt", L"b_virtual.txt"}) {
    std::ofstream{namesakeDirectory / name};
    std::ofstream{sourceDirectory / name};
    ctx->redirectionTable().addFile(
        (virtualDirectory / name).c_str(),
        usvfs::RedirectionDataLocal((sourceDirectory / name).string()));
  }

  // the patterns of the first query and of the restarts after it, std::nullopt for a
  // query without FileName
  struct Sequence
  {
    std::string_view description;
    std::vector<std::optional<std::wstring_view>> patterns;
  };
  const Sequence sequences[] = {
      {"a*, restart without FileName", {L"a*", std::nullopt}},
      {"a*, restart b*, restart without FileName", {L"a*", L"b*", std::nullopt}},
      {"no FileName, restart b*, restart without FileName",
       {std::nullopt, L"b*", std::nullopt}},
      {"a*, restart with an empty FileName", {L"a*", L""}}};
  // What a query lists together with the queries after it, up to the next restart.
  // Those pass a pattern that matches nothing, which a query that does not restart
  // ignores.
  struct Listing
  {
    NTSTATUS firstResult;
    std::vector<std::wstring> names;
    NTSTATUS endResult;
  };
  const auto list = [](NtQueryDirectoryFile_type query, HANDLE directory,
                       const Sequence& sequence) {
    const DirectoryRecordLayout layout =
        findDirectoryRecordLayout(FileDirectoryInformation).value();
    std::vector<Listing> listings;
    for (size_t index = 0; index < sequence.patterns.size(); ++index) {
      const bool isRestart = index > 0;
      ListedQuery listed   = queryOnce(query, directory,
                                       {FileDirectoryInformation, 0x1000, false,
                                        sequence.patterns[index], isRestart});
      Listing listing{.firstResult = listed.outcome.result};
      for (size_t count = 0;
           listed.outcome.result == STATUS_SUCCESS && count < MAXIMUM_QUERIES;
           ++count) {
        std::ranges::move(recordNames(layout, listed.data),
                          std::back_inserter(listing.names));
        listed =
            queryOnce(query, directory,
                      {FileDirectoryInformation, 0x1000, false, L"usvfs_no_match"});
      }
      listing.endResult = listed.outcome.result;
      // the virtual entries follow the regular ones in an order of their own
      std::ranges::sort(listing.names);
      listings.push_back(std::move(listing));
    }
    return listings;
  };

  // ntdll keeps a* on the restart without FileName, so that the comparisons below tell
  // the latched pattern from every entry
  HANDLE referenceHandle = openHookedDirectory(namesakeDirectory);
  ASSERT_NE(INVALID_HANDLE_VALUE, referenceHandle);
  const std::vector<Listing> reference =
      list(nativeQueryDirectoryFile(), referenceHandle, sequences[0]);
  usvfs::hook_NtClose(referenceHandle);
  ASSERT_EQ((std::vector<std::wstring>{L"a_regular.txt", L"a_virtual.txt"}),
            reference.back().names);

  for (const Sequence& sequence : sequences) {
    SCOPED_TRACE(sequence.description);
    HANDLE namesakeHandle = openHookedDirectory(namesakeDirectory);
    HANDLE hookedHandle   = openHookedDirectory(virtualDirectory);
    ASSERT_NE(INVALID_HANDLE_VALUE, namesakeHandle);
    ASSERT_NE(INVALID_HANDLE_VALUE, hookedHandle);

    const std::vector<Listing> expected =
        list(nativeQueryDirectoryFile(), namesakeHandle, sequence);
    const std::vector<Listing> actual =
        list(&usvfs::hook_NtQueryDirectoryFile, hookedHandle, sequence);
    for (size_t index = 0; index < sequence.patterns.size(); ++index) {
      SCOPED_TRACE(std::format("listing {}", index));
      EXPECT_EQ(expected[index].firstResult, actual[index].firstResult);
      EXPECT_EQ(expected[index].names, actual[index].names);
      EXPECT_EQ(expected[index].endResult, actual[index].endResult);
    }

    usvfs::hook_NtClose(namesakeHandle);
    usvfs::hook_NtClose(hookedHandle);
  }
}

// ntdll is the reference for the query flags besides restart and single entry. The I/O
// manager drops SL_INDEX_SPECIFIED and the undefined bits before the filesystem sees
// them, so a query with them lists what the namesake directory lists.
// SL_RETURN_ON_DISK_ENTRIES_ONLY asks a filter that virtualizes the directory to leave
// the query to the filesystem, and the hook does not emulate SL_NO_CURSOR_UPDATE, so a
// query with either lists what ntdll lists of the physical directory.
TEST_F(USVFSTest, NtQueryDirectoryFileExHandlesQueryFlagsLikeNtdll)
{
  // a guard against a listing that does not end
  constexpr size_t MAXIMUM_QUERIES = 100;
  const std::filesystem::path root = test::path_of_test_temp(L"query_flags");
  std::filesystem::remove_all(root);
  const std::filesystem::path namesakeDirectory = root / L"namesakes";
  const std::filesystem::path virtualDirectory  = root / L"virtual";
  const std::filesystem::path sourceDirectory   = root / L"source";
  for (const std::filesystem::path& directory :
       {namesakeDirectory, virtualDirectory, sourceDirectory}) {
    std::filesystem::create_directories(directory);
  }
  auto params = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));
  std::ofstream{namesakeDirectory / L"a_regular.txt"};
  std::ofstream{virtualDirectory / L"a_regular.txt"};
  std::ofstream{namesakeDirectory / L"a_virtual.txt"};
  std::ofstream{sourceDirectory / L"a_virtual.txt"};
  ctx->redirectionTable().addFile(
      (virtualDirectory / L"a_virtual.txt").c_str(),
      usvfs::RedirectionDataLocal((sourceDirectory / L"a_virtual.txt").string()));

  const auto nativeQuery = reinterpret_cast<NtQueryDirectoryFileEx_type>(
      ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtQueryDirectoryFileEx"));
  const auto queryOnceEx = [](NtQueryDirectoryFileEx_type query, HANDLE directory,
                              ULONG flags, std::wstring_view pattern) {
    usvfs::UnicodeString fileName(pattern.data(), pattern.size());
    alignas(8) std::array<std::byte, 0x1000> buffer;
    ListedQuery listed;
    listed.outcome.statusBlock.Status      = UNWRITTEN_STATUS;
    listed.outcome.statusBlock.Information = UNWRITTEN_INFORMATION;
    listed.outcome.result =
        query(directory, nullptr, nullptr, nullptr, &listed.outcome.statusBlock,
              buffer.data(), static_cast<ULONG>(buffer.size()),
              FileDirectoryInformation, flags, static_cast<PUNICODE_STRING>(fileName));
    if (listed.outcome.result == STATUS_SUCCESS) {
      listed.data.assign(buffer.begin(),
                         buffer.begin() + listed.outcome.statusBlock.Information);
    }
    return listed;
  };
  const DirectoryRecordLayout layout =
      findDirectoryRecordLayout(FileDirectoryInformation).value();

  // the flags as kernelbase passes them for FIND_FIRST_EX_ON_DISK_ENTRIES_ONLY, and
  // alone; the pattern matches the virtual entry only, so that a query the hook
  // virtualized would list it where ntdll lists nothing
  for (const ULONG flags :
       {ULONG{SL_RETURN_SINGLE_ENTRY | SL_RETURN_ON_DISK_ENTRIES_ONLY},
        ULONG{SL_NO_CURSOR_UPDATE}}) {
    SCOPED_TRACE(std::format("flags {:#x}", flags));
    HANDLE nativeHandle = openHookedDirectory(virtualDirectory);
    HANDLE hookedHandle = openHookedDirectory(virtualDirectory);
    ASSERT_NE(INVALID_HANDLE_VALUE, nativeHandle);
    ASSERT_NE(INVALID_HANDLE_VALUE, hookedHandle);

    const ListedQuery expected =
        queryOnceEx(nativeQuery, nativeHandle, flags, L"*_virtual.txt");
    const ListedQuery actual = queryOnceEx(&usvfs::hook_NtQueryDirectoryFileEx,
                                           hookedHandle, flags, L"*_virtual.txt");
    expectSameOutcome(expected.outcome, actual.outcome);
    expectSameRecords(layout, expected.data, actual.data);

    usvfs::hook_NtClose(nativeHandle);
    usvfs::hook_NtClose(hookedHandle);
  }

  for (const ULONG flags : {ULONG{SL_INDEX_SPECIFIED}, 0x20ul, 0x80000000ul}) {
    SCOPED_TRACE(std::format("flags {:#x}", flags));
    HANDLE namesakeHandle = openHookedDirectory(namesakeDirectory);
    HANDLE hookedHandle   = openHookedDirectory(virtualDirectory);
    ASSERT_NE(INVALID_HANDLE_VALUE, namesakeHandle);
    ASSERT_NE(INVALID_HANDLE_VALUE, hookedHandle);

    // the virtual entries follow the regular ones in an order of their own, so only
    // which names a listing holds is compared
    const auto listNames = [&](NtQueryDirectoryFileEx_type query, HANDLE directory) {
      std::vector<std::wstring> names;
      for (size_t count = 0; count < MAXIMUM_QUERIES; ++count) {
        const ListedQuery listed = queryOnceEx(query, directory, flags, L"*");
        if (listed.outcome.result != STATUS_SUCCESS) {
          break;
        }
        std::ranges::move(recordNames(layout, listed.data), std::back_inserter(names));
      }
      std::ranges::sort(names);
      return names;
    };
    EXPECT_EQ(listNames(nativeQuery, namesakeHandle),
              listNames(&usvfs::hook_NtQueryDirectoryFileEx, hookedHandle));

    usvfs::hook_NtClose(namesakeHandle);
    usvfs::hook_NtClose(hookedHandle);
  }
}

// ntdll is the reference for a directory that a mapping source is linked onto, as it
// lists files of the virtual names: the files on disk at the virtual path and the
// virtual entries, whichever handle the caller opened. The open hook reroutes the
// handle to the mapping source unless it was opened with backup intent, so the scan
// reads the physical directory through a handle of its own.
TEST_F(USVFSTest,
       NtQueryDirectoryFileListsThePhysicalDirectoryAtTheVirtualPathLikeNtdll)
{
  // a guard against a listing that does not end
  constexpr size_t MAXIMUM_QUERIES = 100;
  const std::filesystem::path root = test::path_of_test_temp(L"physical_at_virtual");
  std::filesystem::remove_all(root);
  const std::filesystem::path namesakeDirectory = root / L"namesakes";
  const std::filesystem::path virtualDirectory  = root / L"virtual";
  const std::filesystem::path sourceDirectory   = root / L"source";
  for (const std::filesystem::path& directory :
       {namesakeDirectory, virtualDirectory, sourceDirectory}) {
    std::filesystem::create_directories(directory);
  }
  auto params = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));
  std::ofstream{namesakeDirectory / L"a_regular.txt"};
  std::ofstream{virtualDirectory / L"a_regular.txt"};
  ctx->redirectionTable().addDirectory(
      virtualDirectory.c_str(), usvfs::RedirectionDataLocal(sourceDirectory.string()));
  for (const std::wstring_view name : {L"a_virtual.txt", L"shadowed.txt"}) {
    std::ofstream{namesakeDirectory / name};
    std::ofstream{sourceDirectory / name};
    ctx->redirectionTable().addFile(
        (virtualDirectory / name).c_str(),
        usvfs::RedirectionDataLocal((sourceDirectory / name).string()));
  }
  // shadowed by the virtual entry of the same name, so listed once
  std::ofstream{virtualDirectory / L"shadowed.txt"};

  const auto listNames = [](NtQueryDirectoryFile_type query, HANDLE directory) {
    const DirectoryRecordLayout layout =
        findDirectoryRecordLayout(FileDirectoryInformation).value();
    std::vector<std::wstring> names;
    for (size_t count = 0; count < MAXIMUM_QUERIES; ++count) {
      const ListedQuery listed =
          queryOnce(query, directory, {FileDirectoryInformation, 0x1000, false, L"*"});
      if (listed.outcome.result != STATUS_SUCCESS) {
        break;
      }
      std::ranges::move(recordNames(layout, listed.data), std::back_inserter(names));
    }
    // the virtual entries follow the regular ones in an order of their own
    std::ranges::sort(names);
    return names;
  };

  HANDLE namesakeHandle = openHookedDirectory(namesakeDirectory);
  ASSERT_NE(INVALID_HANDLE_VALUE, namesakeHandle);
  const std::vector<std::wstring> expected =
      listNames(nativeQueryDirectoryFile(), namesakeHandle);
  usvfs::hook_NtClose(namesakeHandle);

  const std::pair<std::string_view, ULONG> handleKinds[] = {
      {"rerouted handle", 0}, {"backup intent", FILE_OPEN_FOR_BACKUP_INTENT}};
  for (const auto& [handleDescription, backupIntent] : handleKinds) {
    SCOPED_TRACE(handleDescription);
    HANDLE hookedHandle = hooked_NtOpenFile(
        virtualDirectory.c_str(), FILE_GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | backupIntent);
    ASSERT_NE(INVALID_HANDLE_VALUE, hookedHandle);
    EXPECT_EQ(expected, listNames(&usvfs::hook_NtQueryDirectoryFile, hookedHandle));
    usvfs::hook_NtClose(hookedHandle);
  }
}

// ntdll is the reference for a directory whose tree node has children without a link
// target: they map nothing, so the directory lists as it lists on disk, a child that
// exists there with the other entries and one that does not not at all.
TEST_F(USVFSTest, NtQueryDirectoryFileListsUnlinkedTreeDirectoriesLikeNtdll)
{
  const std::filesystem::path directory = test::path_of_test_temp(L"unlinked_tree");
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory / L"usvfs_on_disk");
  auto params = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));
  for (const std::wstring_view name : {L"usvfs_on_disk", L"usvfs_tree_only"}) {
    ctx->redirectionTable().addDirectory((directory / name).c_str(),
                                         usvfs::RedirectionDataLocal(""));
  }
  // a query moves its handle through the directory, so ntdll and the hook each list
  // through a handle of their own
  HANDLE nativeHandle = openHookedDirectory(directory);
  HANDLE hookedHandle = openHookedDirectory(directory);
  ASSERT_NE(INVALID_HANDLE_VALUE, nativeHandle);
  ASSERT_NE(INVALID_HANDLE_VALUE, hookedHandle);

  const ListingQuery listing{FileDirectoryInformation, 0x1000, false, L"*"};
  const std::vector<ListedQuery> expected =
      listQueries(nativeQueryDirectoryFile(), nativeHandle, listing);
  const std::vector<ListedQuery> actual =
      listQueries(&usvfs::hook_NtQueryDirectoryFile, hookedHandle, listing);

  const DirectoryRecordLayout layout =
      findDirectoryRecordLayout(FileDirectoryInformation).value();
  ASSERT_EQ(STATUS_SUCCESS, expected.front().outcome.result);
  ASSERT_EQ(3u, recordOffsets(layout, expected.front().data).size())
      << "\".\", \"..\" and the directory on disk";
  ASSERT_EQ(expected.size(), actual.size());
  for (size_t query = 0; query < expected.size(); ++query) {
    SCOPED_TRACE(query);
    expectSameOutcome(expected[query].outcome, actual[query].outcome);
    expectSameRecords(layout, expected[query].data, actual[query].data);
  }

  usvfs::hook_NtClose(nativeHandle);
  usvfs::hook_NtClose(hookedHandle);
}

namespace
{

// A game directory that two mods are mapped into, and a directory that holds on disk
// what the directories the game directory has only virtually list.
struct ModdedDirectory
{
  std::filesystem::path data;
  std::filesystem::path namesakes;
};

// Lays out two mods and maps them into a game directory as
// usvfsVirtualLinkDirectoryStatic links a mod recursively, the first mapped winning,
// without what the skip lists of Mod Organizer leave out: a ".mohidden" suffix and
// ".git". These stay in the mod directories, which the open hook reroutes the
// directories of the game directory to, as does "x", a file the mod gains after the
// mapping. "sub" and "sub\deep" are in the mods only.
ModdedDirectory mapModsWithSkippedEntries(usvfs::RedirectionTreeContainer& tree,
                                          const std::filesystem::path& root)
{
  std::filesystem::remove_all(root);
  const ModdedDirectory modded{root / L"data", root / L"namesakes"};
  const std::filesystem::path modA = root / L"mod_a";
  const std::filesystem::path modB = root / L"mod_b";
  for (const std::filesystem::path& directory :
       {modded.data, modA / L"sub" / L".git", modA / L"sub" / L"deep", modB / L"sub",
        modded.namesakes / L"sub" / L"deep"}) {
    std::filesystem::create_directories(directory);
  }
  for (const std::filesystem::path& file :
       {modA / L"sub" / L"a.txt", modA / L"sub" / L"hidden.txt.mohidden",
        modA / L"sub" / L".git" / L"HEAD", modA / L"sub" / L"x",
        modA / L"sub" / L"deep" / L"c.txt", modA / L"sub" / L"deep" / L"d.txt.mohidden",
        modB / L"sub" / L"b.txt", modded.namesakes / L"sub" / L"a.txt",
        modded.namesakes / L"sub" / L"b.txt",
        modded.namesakes / L"sub" / L"deep" / L"c.txt"}) {
    std::ofstream{file};
  }

  const auto linkDirectory = [&](const std::filesystem::path& source,
                                 const std::filesystem::path& destination) {
    tree.addDirectory(destination.c_str(),
                      usvfs::RedirectionDataLocal(source.string() + "\\"),
                      usvfs::shared::FLAG_DIRECTORY, false);
  };
  const auto linkFile = [&](const std::filesystem::path& source,
                            const std::filesystem::path& destination) {
    tree.addFile(destination.c_str(), usvfs::RedirectionDataLocal(source.string()));
  };
  for (const std::filesystem::path& mod : {modA, modB}) {
    linkDirectory(mod, modded.data);
    linkDirectory(mod / L"sub", modded.data / L"sub");
  }
  linkDirectory(modA / L"sub" / L"deep", modded.data / L"sub" / L"deep");
  linkFile(modA / L"sub" / L"a.txt", modded.data / L"sub" / L"a.txt");
  linkFile(modB / L"sub" / L"b.txt", modded.data / L"sub" / L"b.txt");
  linkFile(modA / L"sub" / L"deep" / L"c.txt",
           modded.data / L"sub" / L"deep" / L"c.txt");
  return modded;
}

// Lists a directory one entry per query, with the smallest buffer ntdll accepts until
// a record does not fit it, then with a buffer that holds any.
std::vector<ListedQuery>
listEntriesFromTheSmallestBuffer(NtQueryDirectoryFile_type query, HANDLE directory,
                                 FILE_INFORMATION_CLASS informationClass,
                                 std::wstring_view pattern)
{
  ULONG length = findDirectoryRecordLayout(informationClass).value().minimumLength;
  std::vector<ListedQuery> queries;
  for (;;) {
    queries.push_back(
        queryOnce(query, directory, {informationClass, length, true, pattern}));
    const NTSTATUS result = queries.back().outcome.result;
    if (result == STATUS_BUFFER_OVERFLOW) {
      length = 0x1000;
    } else if (result != STATUS_SUCCESS) {
      return queries;
    }
  }
}

// The names of the records a query returned, whole or cut short.
std::vector<std::wstring> namesOf(const DirectoryRecordLayout& layout,
                                  const ListedQuery& query)
{
  return query.data.empty() ? std::vector<std::wstring>()
                            : recordNames(layout, query.data);
}

// Expects the same outcome, but for the status block of a query that fails: ReFS
// writes it when nothing matches, unlike NTFS, and the hook never does, a difference
// of its own.
void expectSameListingOutcome(const QueryOutcome& expected, const QueryOutcome& actual)
{
  const bool isError = (static_cast<ULONG>(expected.result) >> 30) == 3;
  if (isError) {
    EXPECT_EQ(expected.result, actual.result);
  } else {
    expectSameOutcome(expected, actual);
  }
}

// Expects a directory that the tree holds only virtually to list as ntdll lists its
// namesake on disk: query by query when each query lists one entry, and name by name
// otherwise, as the hook returns regular and virtual entries in separate queries. Only
// the names are compared, as "." and ".." carry the fields of the mod directory the
// handle is rerouted to.
void expectListedLikeNamesake(const std::filesystem::path& directory,
                              const std::filesystem::path& namesake)
{
  for (const FILE_INFORMATION_CLASS informationClass : DIRECTORY_INFORMATION_CLASSES) {
    const DirectoryRecordLayout layout =
        findDirectoryRecordLayout(informationClass).value();
    // ".git" and "x" match only entries the tree leaves out, and no dots: ".git"
    // starts like ".", and "x" is as long, which the reader must tell from a dot
    for (const std::wstring_view pattern : {L"*", L"*.txt", L".git", L"x"}) {
      SCOPED_TRACE(std::format("class {}, pattern {}",
                               static_cast<int>(informationClass),
                               std::filesystem::path(pattern).string()));
      // a query moves its handle through the directory, so ntdll and the hook each
      // list through a handle of their own, opened anew for each listing
      HANDLE nativeHandle = openHookedDirectory(namesake);
      HANDLE hookedHandle = openHookedDirectory(directory);
      ASSERT_NE(INVALID_HANDLE_VALUE, nativeHandle);
      ASSERT_NE(INVALID_HANDLE_VALUE, hookedHandle);
      const std::vector<ListedQuery> expectedEntries = listEntriesFromTheSmallestBuffer(
          nativeQueryDirectoryFile(), nativeHandle, informationClass, pattern);
      const std::vector<ListedQuery> actualEntries = listEntriesFromTheSmallestBuffer(
          &usvfs::hook_NtQueryDirectoryFile, hookedHandle, informationClass, pattern);
      ASSERT_EQ(expectedEntries.size(), actualEntries.size());
      for (size_t query = 0; query < expectedEntries.size(); ++query) {
        SCOPED_TRACE(query);
        expectSameListingOutcome(expectedEntries[query].outcome,
                                 actualEntries[query].outcome);
        EXPECT_EQ(namesOf(layout, expectedEntries[query]),
                  namesOf(layout, actualEntries[query]));
      }
      usvfs::hook_NtClose(nativeHandle);
      usvfs::hook_NtClose(hookedHandle);

      nativeHandle = openHookedDirectory(namesake);
      hookedHandle = openHookedDirectory(directory);
      ASSERT_NE(INVALID_HANDLE_VALUE, nativeHandle);
      ASSERT_NE(INVALID_HANDLE_VALUE, hookedHandle);
      const ListingQuery listing{informationClass, 0x1000, false, pattern};
      const std::vector<ListedQuery> expected =
          listQueries(nativeQueryDirectoryFile(), nativeHandle, listing);
      const std::vector<ListedQuery> actual =
          listQueries(&usvfs::hook_NtQueryDirectoryFile, hookedHandle, listing);
      const auto listedNames = [&](const std::vector<ListedQuery>& queries) {
        std::vector<std::wstring> names;
        for (const ListedQuery& query : queries) {
          std::ranges::move(namesOf(layout, query), std::back_inserter(names));
        }
        return names;
      };
      EXPECT_EQ(listedNames(expected), listedNames(actual));
      expectSameListingOutcome(expected.back().outcome, actual.back().outcome);
      usvfs::hook_NtClose(nativeHandle);
      usvfs::hook_NtClose(hookedHandle);
    }
  }
}

}  // namespace

// ntdll is the reference for a directory that only the mods hold, as it lists a
// namesake that holds the mapped files on disk: the open hook reroutes the directory to
// that of the first mod, whose files and directories that the tree leaves out are not
// listed. A file, or an empty directory pending deletion, at its path on disk adds no
// entries either.
TEST_F(USVFSTest, NtQueryDirectoryFileListsPurelyVirtualDirectoriesLikeNtdll)
{
  auto params = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));
  const std::filesystem::path root = test::path_of_test_temp(L"purely_virtual");

  {
    SCOPED_TRACE("nothing on disk");
    const ModdedDirectory modded =
        mapModsWithSkippedEntries(ctx->redirectionTable(), root / L"nothing");
    expectListedLikeNamesake(modded.data / L"sub", modded.namesakes / L"sub");
    expectListedLikeNamesake(modded.data / L"sub" / L"deep",
                             modded.namesakes / L"sub" / L"deep");
  }
  {
    SCOPED_TRACE("a file on disk");
    const ModdedDirectory modded =
        mapModsWithSkippedEntries(ctx->redirectionTable(), root / L"file");
    std::ofstream(modded.data / L"sub");
    expectListedLikeNamesake(modded.data / L"sub", modded.namesakes / L"sub");
    expectListedLikeNamesake(modded.data / L"sub" / L"deep",
                             modded.namesakes / L"sub" / L"deep");
  }
  {
    SCOPED_TRACE("a directory pending deletion on disk");
    const ModdedDirectory modded =
        mapModsWithSkippedEntries(ctx->redirectionTable(), root / L"pending");
    std::filesystem::create_directory(modded.data / L"sub");
    const HANDLE deleter =
        ::CreateFileW((modded.data / L"sub").c_str(), DELETE,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    ASSERT_NE(INVALID_HANDLE_VALUE, deleter);
    FILE_DISPOSITION_INFO disposition{TRUE};
    ASSERT_TRUE(::SetFileInformationByHandle(deleter, FileDispositionInfo, &disposition,
                                             sizeof(disposition)));
    expectListedLikeNamesake(modded.data / L"sub", modded.namesakes / L"sub");
    ::CloseHandle(deleter);
  }
}

namespace
{

constexpr ULONG SHARE_ALL = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

// Gives "sub" of the game directory a file of its own on disk, which its namesake holds
// too, so that "sub" lists the physical directory along with the mapped files.
void putSubOnDisk(const ModdedDirectory& modded)
{
  std::filesystem::create_directory(modded.data / L"sub");
  std::ofstream(modded.data / L"sub" / L"on_disk.txt");
  std::ofstream(modded.namesakes / L"sub" / L"on_disk.txt");
}

// Opens a directory through the NtCreateFile hook, which leaves a directory that both
// the tree and the disk hold on the physical one, unlike the NtOpenFile hook.
HANDLE openThroughCreateHook(const std::filesystem::path& path)
{
  usvfs::UnicodeString ntPath(L"\\??\\" + path.wstring());
  OBJECT_ATTRIBUTES attributes{sizeof(attributes),
                               nullptr,
                               static_cast<PUNICODE_STRING>(ntPath),
                               0,
                               nullptr,
                               nullptr};
  IO_STATUS_BLOCK statusBlock;
  HANDLE handle = INVALID_HANDLE_VALUE;
  // FILE_OPEN of the DDK, which the SDK headers do not declare
  constexpr ULONG OPEN_EXISTING_DISPOSITION = 1;

  const NTSTATUS result = usvfs::hook_NtCreateFile(
      &handle, FILE_GENERIC_READ, &attributes, &statusBlock, nullptr, 0, SHARE_ALL,
      OPEN_EXISTING_DISPOSITION, FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
      nullptr, 0);
  return result >= 0 ? handle : INVALID_HANDLE_VALUE;
}

// Opens a directory through the NtOpenFile hook, sharing delete access as
// FindFirstFileExW does.
HANDLE openThroughOpenHook(const std::filesystem::path& path)
{
  return hooked_NtOpenFile(path.c_str(), FILE_GENERIC_READ, SHARE_ALL,
                           FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
}

// Expects the hook to list a directory through a handle as ntdll lists its namesake,
// name by name.
void expectSameNames(HANDLE nativeHandle, HANDLE hookedHandle)
{
  const ListingQuery listing{FileDirectoryInformation, 0x1000, false, L"*"};
  EXPECT_EQ(sortedNames(listQueries(nativeQueryDirectoryFile(), nativeHandle, listing)),
            sortedNames(
                listQueries(&usvfs::hook_NtQueryDirectoryFile, hookedHandle, listing)));
}

// A listing of a directory that is renamed after its first query.
struct RenamedListing
{
  DWORD renameError;
  std::vector<std::wstring> names;
};

// Lists a directory through a handle of the NtOpenFile hook, renames it after the first
// query, and names it back once the handle is closed.
RenamedListing listRenamingAfterTheFirstQuery(NtQueryDirectoryFile_type query,
                                              const std::filesystem::path& path)
{
  std::filesystem::path renamed = path;
  renamed += L"_renamed";
  const HANDLE handle = openThroughOpenHook(path);
  EXPECT_NE(INVALID_HANDLE_VALUE, handle);
  const ListingQuery listing{FileDirectoryInformation, 0x1000, true, L"*"};
  std::vector<ListedQuery> queries{queryOnce(query, handle, listing)};
  RenamedListing result{};
  result.renameError = ::MoveFileExW(path.c_str(), renamed.c_str(), 0)
                           ? static_cast<DWORD>(ERROR_SUCCESS)
                           : ::GetLastError();
  std::ranges::move(listQueries(query, handle, listing), std::back_inserter(queries));
  result.names = sortedNames(queries);
  usvfs::hook_NtClose(handle);
  if (result.renameError == ERROR_SUCCESS) {
    EXPECT_TRUE(::MoveFileExW(renamed.c_str(), path.c_str(), 0)) << ::GetLastError();
  }
  return result;
}

// Denies the current user listing a directory while it lives. The DACL is set through
// a handle, as SetNamedSecurityInfoW and SetSecurityInfo pass the inheritable entries
// on to the children, which takes listing the directory.
class ListingDenial
{
public:
  explicit ListingDenial(const std::filesystem::path& directory)
      : m_handle(::CreateFileW(directory.c_str(), READ_CONTROL | WRITE_DAC, SHARE_ALL,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
                               nullptr))
  {
    EXPECT_NE(INVALID_HANDLE_VALUE, m_handle);
    EXPECT_EQ(ERROR_SUCCESS,
              ::GetSecurityInfo(m_handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                nullptr, nullptr, &m_dacl, nullptr, &m_descriptor));
    EXPLICIT_ACCESS_W access{};
    access.grfAccessPermissions = FILE_LIST_DIRECTORY;
    access.grfAccessMode        = DENY_ACCESS;
    access.grfInheritance       = NO_INHERITANCE;
    BuildTrusteeWithNameW(&access.Trustee, const_cast<LPWSTR>(L"CURRENT_USER"));
    PACL deniedDacl = nullptr;
    EXPECT_EQ(ERROR_SUCCESS, ::SetEntriesInAclW(1, &access, m_dacl, &deniedDacl));
    setDacl(deniedDacl);
    ::LocalFree(deniedDacl);
  }

  ~ListingDenial()
  {
    setDacl(m_dacl);
    ::LocalFree(m_descriptor);
    ::CloseHandle(m_handle);
  }

  ListingDenial(const ListingDenial&)            = delete;
  ListingDenial& operator=(const ListingDenial&) = delete;

private:
  void setDacl(PACL dacl)
  {
    SECURITY_DESCRIPTOR descriptor;
    EXPECT_TRUE(
        ::InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION));
    EXPECT_TRUE(::SetSecurityDescriptorDacl(&descriptor, TRUE, dacl, FALSE));
    EXPECT_TRUE(
        ::SetKernelObjectSecurity(m_handle, DACL_SECURITY_INFORMATION, &descriptor))
        << ::GetLastError();
  }

  HANDLE m_handle;
  PACL m_dacl                       = nullptr;
  PSECURITY_DESCRIPTOR m_descriptor = nullptr;
};

}  // namespace

// ntdll is the reference for a directory that is renamed while it is listed: the
// rename succeeds and the listing goes on, as FindFirstFileExW shares delete access.
// The hook holds the physical directory open from the first query on to read the
// regular entries from.
TEST_F(USVFSTest, NtQueryDirectoryFileLetsAListedDirectoryBeRenamedLikeNtdll)
{
  auto params = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));
  const ModdedDirectory modded = mapModsWithSkippedEntries(
      ctx->redirectionTable(), test::path_of_test_temp(L"physical_renamed"));
  putSubOnDisk(modded);

  const RenamedListing expected = listRenamingAfterTheFirstQuery(
      nativeQueryDirectoryFile(), modded.namesakes / L"sub");
  const RenamedListing actual = listRenamingAfterTheFirstQuery(
      &usvfs::hook_NtQueryDirectoryFile, modded.data / L"sub");

  ASSERT_EQ(ERROR_SUCCESS, expected.renameError);
  EXPECT_EQ(expected.renameError, actual.renameError);
  EXPECT_EQ(expected.names, actual.names);
}

// ntdll is the reference for a physical directory that a handle with DELETE access
// holds: FindFirstFileExW shares delete access, so the hook opens the directory to read
// the regular entries from as well, also where the NtOpenFile hook rerouted the
// caller's handle to a mod directory.
TEST_F(USVFSTest, NtQueryDirectoryFileListsAPhysicalDirectoryHeldForDeletionLikeNtdll)
{
  auto params = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));
  const ModdedDirectory modded = mapModsWithSkippedEntries(
      ctx->redirectionTable(), test::path_of_test_temp(L"physical_held"));
  putSubOnDisk(modded);
  const HANDLE holder =
      ::CreateFileW((modded.data / L"sub").c_str(), DELETE, SHARE_ALL, nullptr,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  ASSERT_NE(INVALID_HANDLE_VALUE, holder);

  for (const bool isOpenHook : {true, false}) {
    SCOPED_TRACE(isOpenHook ? "NtOpenFile hook" : "NtCreateFile hook");
    const HANDLE hookedHandle = isOpenHook
                                    ? openThroughOpenHook(modded.data / L"sub")
                                    : openThroughCreateHook(modded.data / L"sub");
    const HANDLE nativeHandle = openThroughOpenHook(modded.namesakes / L"sub");
    ASSERT_NE(INVALID_HANDLE_VALUE, hookedHandle);
    ASSERT_NE(INVALID_HANDLE_VALUE, nativeHandle);
    expectSameNames(nativeHandle, hookedHandle);
    usvfs::hook_NtClose(nativeHandle);
    usvfs::hook_NtClose(hookedHandle);
  }
  ::CloseHandle(holder);
}

// ntdll is the reference for a physical directory that the hook cannot open to read
// the regular entries from: here as the current user may no longer list it, which an
// open handle that may still list it is not checked against. The caller's handle is
// read then; the NtCreateFile hook leaves it on the physical directory, as the tree
// holds a directory there too.
TEST_F(USVFSTest, NtQueryDirectoryFileListsAPhysicalDirectoryItCannotOpenLikeNtdll)
{
  auto params = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));
  const ModdedDirectory modded = mapModsWithSkippedEntries(
      ctx->redirectionTable(), test::path_of_test_temp(L"physical_denied"));
  putSubOnDisk(modded);
  const std::filesystem::path directory = modded.data / L"sub";

  const HANDLE hookedHandle = openThroughCreateHook(directory);
  const HANDLE nativeHandle = openThroughOpenHook(modded.namesakes / L"sub");
  ASSERT_NE(INVALID_HANDLE_VALUE, hookedHandle);
  ASSERT_NE(INVALID_HANDLE_VALUE, nativeHandle);

  {
    const ListingDenial denial(directory);
    const HANDLE lister =
        ::CreateFileW(directory.c_str(), FILE_LIST_DIRECTORY, SHARE_ALL, nullptr,
                      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    ASSERT_EQ(INVALID_HANDLE_VALUE, lister);
    ASSERT_EQ(ERROR_ACCESS_DENIED, ::GetLastError());
    expectSameNames(nativeHandle, hookedHandle);
  }

  usvfs::hook_NtClose(nativeHandle);
  usvfs::hook_NtClose(hookedHandle);
}

namespace
{

// A directory whose names are case-sensitive, with two files whose names differ in
// case only. The flag can be set on an empty directory only, and takes
// FILE_DELETE_CHILD, which a volume may give users only with full control, so the
// current user is granted it first.
std::filesystem::path caseSensitiveDirectory()
{
  const std::filesystem::path directory = test::path_of_test_temp(L"case_sensitive");
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);

  EXPLICIT_ACCESS_W access{};
  access.grfAccessPermissions = FILE_DELETE_CHILD;
  access.grfAccessMode        = GRANT_ACCESS;
  access.grfInheritance       = NO_INHERITANCE;
  BuildTrusteeWithNameW(&access.Trustee, const_cast<LPWSTR>(L"CURRENT_USER"));
  PACL dacl                       = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  EXPECT_EQ(ERROR_SUCCESS,
            ::GetNamedSecurityInfoW(directory.c_str(), SE_FILE_OBJECT,
                                    DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl,
                                    nullptr, &descriptor));
  PACL grantedDacl = nullptr;
  EXPECT_EQ(ERROR_SUCCESS, ::SetEntriesInAclW(1, &access, dacl, &grantedDacl));
  EXPECT_EQ(ERROR_SUCCESS,
            ::SetNamedSecurityInfoW(const_cast<LPWSTR>(directory.c_str()),
                                    SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                    nullptr, grantedDacl, nullptr));
  ::LocalFree(grantedDacl);
  ::LocalFree(descriptor);

  const HANDLE handle =
      ::CreateFileW(directory.c_str(), FILE_WRITE_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  EXPECT_NE(INVALID_HANDLE_VALUE, handle);
  FILE_CASE_SENSITIVE_INFO caseSensitivity{FILE_CS_FLAG_CASE_SENSITIVE_DIR};
  EXPECT_TRUE(::SetFileInformationByHandle(handle, FileCaseSensitiveInfo,
                                           &caseSensitivity, sizeof(caseSensitivity)))
      << ::GetLastError();
  ::CloseHandle(handle);

  std::ofstream(directory / L"usvfs_case.txt");
  std::ofstream(directory / L"USVFS_CASE.txt");
  return directory;
}

}  // namespace

// ntdll is the reference for a case-sensitive directory: names that differ in case
// only are different entries, and listed as such
TEST_F(USVFSTest, NtQueryDirectoryFileListsCaseVariantsLikeNtdll)
{
  auto params = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));
  const std::filesystem::path directory = caseSensitiveDirectory();
  SCOPED_TRACE(describeDirectory(directory));
  // a query moves its handle through the directory, so ntdll and the hook each list
  // through a handle of their own
  HANDLE nativeHandle = openHookedDirectory(directory);
  HANDLE hookedHandle = openHookedDirectory(directory);
  ASSERT_NE(INVALID_HANDLE_VALUE, nativeHandle);
  ASSERT_NE(INVALID_HANDLE_VALUE, hookedHandle);

  const ListingQuery listing{FileDirectoryInformation, 0x1000, false, L"*"};
  const std::vector<ListedQuery> expected =
      listQueries(nativeQueryDirectoryFile(), nativeHandle, listing);
  const std::vector<ListedQuery> actual =
      listQueries(&usvfs::hook_NtQueryDirectoryFile, hookedHandle, listing);

  const DirectoryRecordLayout layout =
      findDirectoryRecordLayout(FileDirectoryInformation).value();
  ASSERT_EQ(STATUS_SUCCESS, expected.front().outcome.result);
  ASSERT_EQ(4u, recordOffsets(layout, expected.front().data).size())
      << "\".\", \"..\" and both files";
  ASSERT_EQ(expected.size(), actual.size());
  for (size_t query = 0; query < expected.size(); ++query) {
    SCOPED_TRACE(query);
    expectSameOutcome(expected[query].outcome, actual[query].outcome);
    expectSameRecords(layout, expected[query].data, actual[query].data);
  }

  usvfs::hook_NtClose(nativeHandle);
  usvfs::hook_NtClose(hookedHandle);
}

// ntdll is the reference for which names are one: NTFS and ReFS fold a name one UTF-16
// code unit at a time, through the upcase table of the volume. A virtual entry shadows
// the regular entry whose name folds into its own, and of two virtual entries whose
// names do, only the first is listed. Names that differ in the case of a character
// beyond the BMP only stay apart, as the table leaves a surrogate pair as it is.
TEST_F(USVFSTest, NtQueryDirectoryFileFoldsNamesLikeNtdll)
{
  // DESERET SMALL LETTER LONG I and DESERET CAPITAL LETTER LONG I, which LCMapStringW
  // folds into one another
  constexpr std::wstring_view DESERET_SMALL   = L"\U00010428";
  constexpr std::wstring_view DESERET_CAPITAL = L"\U00010400";
  const std::filesystem::path root = test::path_of_test_temp(L"folded_names");
  std::filesystem::remove_all(root);
  const std::filesystem::path namesakeDirectory = root / L"namesakes";
  const std::filesystem::path virtualDirectory  = root / L"virtual";
  const std::filesystem::path sourceDirectory   = root / L"source";
  for (const std::filesystem::path& directory :
       {namesakeDirectory, virtualDirectory, sourceDirectory}) {
    std::filesystem::create_directories(directory);
  }
  for (const std::wstring& name : {std::wstring(L"usvfs_ärger.txt"),
                                   std::format(L"usvfs_{}.txt", DESERET_CAPITAL)}) {
    std::ofstream(virtualDirectory / name);
  }
  // The tree folds ASCII only, so it holds both entries of each pair. It orders the
  // rest of the names bytewise in UTF-8, so "Ä" (C3 84) comes before "ä" (C3 A4).
  std::vector<std::wstring> virtualNames = {
      L"usvfs_ÄRGER.txt",
      std::format(L"usvfs_{}.txt", DESERET_SMALL),
      L"usvfs_virtual_ä.txt",
      L"usvfs_virtual_Ä.txt",
      std::format(L"usvfs_virtual_{}.txt", DESERET_SMALL),
      std::format(L"usvfs_virtual_{}.txt", DESERET_CAPITAL)};
  std::vector<std::wstring> listedNames = {
      L"usvfs_ÄRGER.txt",
      std::format(L"usvfs_{}.txt", DESERET_CAPITAL),
      std::format(L"usvfs_{}.txt", DESERET_SMALL),
      L"usvfs_virtual_Ä.txt",
      std::format(L"usvfs_virtual_{}.txt", DESERET_SMALL),
      std::format(L"usvfs_virtual_{}.txt", DESERET_CAPITAL)};
  // Enough more names that the sets of names spread over many buckets, where two
  // names that fold into one another meet only if their hashes fold too. They come
  // first in the tree, so the set that drops the second of a pair has grown by then.
  for (int filler = 0; filler < 16; ++filler) {
    const std::wstring name = std::format(L"usvfs_filler_{:02}.txt", filler);
    virtualNames.push_back(name);
    listedNames.push_back(name);
  }
  for (const std::wstring& name : listedNames) {
    std::ofstream(namesakeDirectory / name);
  }
  auto params = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));
  for (size_t index = 0; index < virtualNames.size(); ++index) {
    const std::filesystem::path sourceFile =
        sourceDirectory / std::format(L"source_{}.txt", index);
    std::ofstream{sourceFile};
    ctx->redirectionTable().addFile(
        usvfs::shared::string_cast<std::string>(
            (virtualDirectory / virtualNames[index]).wstring(),
            usvfs::shared::CodePage::UTF8),
        usvfs::RedirectionDataLocal(sourceFile.string()));
  }

  // every name, and the pair that differs in the case of U+00E4 alone, so that no name
  // of the listing lies beyond U+00FF
  const std::pair<std::wstring_view, size_t> listings[] = {
      {L"usvfs_*", listedNames.size()}, {L"usvfs_virtual_?.txt", 1}};
  for (const auto& [pattern, listedCount] : listings) {
    SCOPED_TRACE(usvfs::shared::string_cast<std::string>(
        std::wstring(pattern), usvfs::shared::CodePage::UTF8));
    const ListingQuery listing{FileDirectoryInformation, 0x1000, false, pattern};
    HANDLE namesakeHandle = openHookedDirectory(namesakeDirectory);
    HANDLE hookedHandle   = openHookedDirectory(virtualDirectory);
    ASSERT_NE(INVALID_HANDLE_VALUE, namesakeHandle);
    ASSERT_NE(INVALID_HANDLE_VALUE, hookedHandle);

    const std::vector<ListedQuery> expected =
        listQueries(nativeQueryDirectoryFile(), namesakeHandle, listing);
    const std::vector<ListedQuery> actual =
        listQueries(&usvfs::hook_NtQueryDirectoryFile, hookedHandle, listing);

    ASSERT_EQ(listedCount, sortedNames(expected).size())
        << "ntdll lists other namesakes, as if the volume folded the Deseret names";
    EXPECT_EQ(sortedNames(expected), sortedNames(actual));

    usvfs::hook_NtClose(namesakeHandle);
    usvfs::hook_NtClose(hookedHandle);
  }
}

// ntdll is the reference for a handle that takes the value of one the hooks did not
// see closed, as DuplicateHandle with DUPLICATE_CLOSE_SOURCE into another process
// closes it in the kernel: the new handle lists its own directory from the start,
// whatever the closed one had listed. Into the same process, the duplicate takes the
// value itself, with the same file object, so its scan stays right.
TEST_F(USVFSTest, NtQueryDirectoryFileListsAReissuedHandleLikeNtdll)
{
  auto params = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));
  const std::vector<std::filesystem::path> directories = nativeListingDirectories();
  const ListingQuery listing{FileDirectoryInformation, 0x1000, false, L"*"};

  HANDLE closedHandle = openHookedDirectory(directories[0]);
  ASSERT_NE(INVALID_HANDLE_VALUE, closedHandle);
  listQueries(&usvfs::hook_NtQueryDirectoryFile, closedHandle, listing);
  // the hooks are not installed in the test process, so this close passes them by
  ASSERT_TRUE(::CloseHandle(closedHandle));
  // The handle table hands the value out again, though not necessarily next: opening
  // through the hooks takes other handles too.
  std::vector<HANDLE> otherHandles;
  HANDLE reissuedHandle = openHookedDirectory(directories[1]);
  while (reissuedHandle != closedHandle && otherHandles.size() < 1000) {
    otherHandles.push_back(reissuedHandle);
    reissuedHandle = openHookedDirectory(directories[1]);
  }
  ASSERT_EQ(closedHandle, reissuedHandle) << "the value is not reissued";
  HANDLE nativeHandle = openHookedDirectory(directories[1]);
  ASSERT_NE(INVALID_HANDLE_VALUE, nativeHandle);

  const std::vector<ListedQuery> expected =
      listQueries(nativeQueryDirectoryFile(), nativeHandle, listing);
  const std::vector<ListedQuery> actual =
      listQueries(&usvfs::hook_NtQueryDirectoryFile, reissuedHandle, listing);

  const DirectoryRecordLayout layout =
      findDirectoryRecordLayout(FileDirectoryInformation).value();
  ASSERT_LE(2u, expected.size());
  ASSERT_EQ(expected.size(), actual.size());
  for (size_t query = 0; query < expected.size(); ++query) {
    SCOPED_TRACE(query);
    expectSameOutcome(expected[query].outcome, actual[query].outcome);
    expectSameRecords(layout, expected[query].data, actual[query].data);
  }

  for (const HANDLE handle : otherHandles) {
    usvfs::hook_NtClose(handle);
  }
  usvfs::hook_NtClose(reissuedHandle);
  usvfs::hook_NtClose(nativeHandle);
}

// A hook context that replaces another drops the scans of the one before, as the stock
// code dropped those it kept in the context: the next query on a handle lists the
// mappings of the new context rather than those the scan was built with.
TEST_F(USVFSTest, NtQueryDirectoryFileListsTheMappingsOfAReplacingContext)
{
  const std::filesystem::path root = test::path_of_test_temp(L"replacing_context");
  std::filesystem::remove_all(root);
  const std::filesystem::path data = root / L"data";
  const std::filesystem::path mod  = root / L"mod";
  std::filesystem::create_directories(data);
  std::filesystem::create_directories(mod);
  std::ofstream(mod / L"old.txt");
  std::ofstream(mod / L"new.txt");
  const auto mapIntoData = [&](usvfs::HookContext& context, const wchar_t* fileName) {
    context.redirectionTable().addDirectory(
        data.c_str(), usvfs::RedirectionDataLocal(mod.string() + "\\"),
        usvfs::shared::FLAG_DIRECTORY, false);
    context.redirectionTable().addFile(
        (data / fileName).c_str(),
        usvfs::RedirectionDataLocal((mod / fileName).string()));
  };

  auto params = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(
      usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));
  mapIntoData(*ctx, L"old.txt");
  HANDLE directory = openThroughCreateHook(data);
  ASSERT_NE(INVALID_HANDLE_VALUE, directory);
  const ListingQuery single{FileDirectoryInformation, 0x1000, true, L"*"};
  ASSERT_EQ(
      std::vector<std::wstring>{L"."},
      sortedNames({queryOnce(&usvfs::hook_NtQueryDirectoryFile, directory, single)}));

  ctx.reset();
  ctx.reset(usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));
  mapIntoData(*ctx, L"new.txt");

  // the regular entries start over, as the new scan reads the physical directory
  // through a handle of its own
  const ListingQuery rest{FileDirectoryInformation, 0x1000, false, std::nullopt};
  EXPECT_EQ(
      (std::vector<std::wstring>{L".", L"..", L"new.txt"}),
      sortedNames(listQueries(&usvfs::hook_NtQueryDirectoryFile, directory, rest)));

  usvfs::hook_NtClose(directory);
}
