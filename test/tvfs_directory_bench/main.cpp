// Measures a full listing of a directory that mods are mapped into, through the hooks,
// against ntdll listing a directory that holds the same files on disk, and against the
// hooks listing that directory.
//
// The hooks are called directly, as tvfs_test calls them: they are not installed in
// this process. The calls a listing makes to ntdll are counted by hooks on ntdll
// itself, which the usvfs hooks call through its exports.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>

#include <hooklib.h>
#include <hooks/ntdll.h>
#include <ntdll_declarations.h>
#include <test_helpers.h>
#include <ttrampolinepool.h>
#include <unicodestring.h>
#include <usvfs.h>

namespace
{

// the calls to ntdll made since the counts were last reset
std::atomic<std::uint64_t> openCount;
std::atomic<std::uint64_t> queryCount;

// the functions the counters call through to
NtOpenFile_type originalNtOpenFile                         = nullptr;
NtCreateFile_type originalNtCreateFile                     = nullptr;
NtQueryDirectoryFile_type originalNtQueryDirectoryFile     = nullptr;
NtQueryDirectoryFileEx_type originalNtQueryDirectoryFileEx = nullptr;

std::vector<HookLib::HOOKHANDLE> counterHooks;

NTSTATUS WINAPI countNtOpenFile(PHANDLE handle, ACCESS_MASK access,
                                POBJECT_ATTRIBUTES attributes,
                                PIO_STATUS_BLOCK statusBlock, ULONG shareAccess,
                                ULONG openOptions)
{
  ++openCount;
  return originalNtOpenFile(handle, access, attributes, statusBlock, shareAccess,
                            openOptions);
}

NTSTATUS WINAPI countNtCreateFile(PHANDLE handle, ACCESS_MASK access,
                                  POBJECT_ATTRIBUTES attributes,
                                  PIO_STATUS_BLOCK statusBlock,
                                  PLARGE_INTEGER allocationSize, ULONG fileAttributes,
                                  ULONG shareAccess, ULONG disposition,
                                  ULONG createOptions, PVOID eaBuffer, ULONG eaLength)
{
  ++openCount;
  return originalNtCreateFile(handle, access, attributes, statusBlock, allocationSize,
                              fileAttributes, shareAccess, disposition, createOptions,
                              eaBuffer, eaLength);
}

NTSTATUS WINAPI countNtQueryDirectoryFile(HANDLE handle, HANDLE event,
                                          PIO_APC_ROUTINE apcRoutine, PVOID apcContext,
                                          PIO_STATUS_BLOCK statusBlock, PVOID buffer,
                                          ULONG length,
                                          FILE_INFORMATION_CLASS informationClass,
                                          BOOLEAN isSingleEntry,
                                          PUNICODE_STRING fileName, BOOLEAN isRestart)
{
  ++queryCount;
  return originalNtQueryDirectoryFile(handle, event, apcRoutine, apcContext,
                                      statusBlock, buffer, length, informationClass,
                                      isSingleEntry, fileName, isRestart);
}

NTSTATUS WINAPI countNtQueryDirectoryFileEx(
    HANDLE handle, HANDLE event, PIO_APC_ROUTINE apcRoutine, PVOID apcContext,
    PIO_STATUS_BLOCK statusBlock, PVOID buffer, ULONG length,
    FILE_INFORMATION_CLASS informationClass, ULONG flags, PUNICODE_STRING fileName)
{
  ++queryCount;
  return originalNtQueryDirectoryFileEx(handle, event, apcRoutine, apcContext,
                                        statusBlock, buffer, length, informationClass,
                                        flags, fileName);
}

// Hooks the ntdll function with the counter, and returns the original function.
template <typename Function>
Function hookNtdll(const char* functionName, Function counter)
{
  HookLib::HookError error = HookLib::ERR_NONE;
  const HookLib::HOOKHANDLE hook =
      HookLib::InstallHook(::GetModuleHandleW(L"ntdll.dll"), functionName,
                           reinterpret_cast<LPVOID>(counter), &error);
  if (hook == HookLib::INVALID_HOOK) {
    throw std::runtime_error(std::format("failed to hook {}: {}", functionName,
                                         HookLib::GetErrorString(error)));
  }
  counterHooks.push_back(hook);
  // looked up once, as GetDetour copies the bookkeeping of the hook, which allocates
  return reinterpret_cast<Function>(HookLib::GetDetour(hook));
}

// Mods of files mapped into an empty data directory, and a directory that holds the
// same files on disk. The names differ across mods, so that every file is listed.
struct Layout
{
  std::filesystem::path data;
  std::filesystem::path physical;
  std::vector<std::filesystem::path> mods;
  // the files, and "." and ".."
  std::size_t entryCount;
};

// Returns the layout of a number of mods with a number of files each, creating it on
// the first call.
const Layout& layoutOf(std::size_t modCount, std::size_t fileCount)
{
  static std::map<std::pair<std::size_t, std::size_t>, Layout> layouts;
  const auto [entry, isNew] = layouts.try_emplace({modCount, fileCount});
  Layout& layout            = entry->second;
  if (!isNew) {
    return layout;
  }

  const std::filesystem::path root = test::path_of_test_temp(
      std::format(L"directory_bench\\{}x{}", modCount, fileCount));
  std::filesystem::remove_all(root);
  layout.data       = root / L"data";
  layout.physical   = root / L"physical";
  layout.entryCount = modCount * fileCount + 2;
  std::filesystem::create_directories(layout.data);
  std::filesystem::create_directories(layout.physical);
  for (std::size_t mod = 0; mod < modCount; ++mod) {
    const std::filesystem::path modPath = root / L"mods" / std::format(L"mod{}", mod);
    std::filesystem::create_directories(modPath);
    for (std::size_t file = 0; file < fileCount; ++file) {
      const std::wstring name = std::format(L"mod{}_file{}.txt", mod, file);
      std::ofstream(modPath / name);
      std::ofstream(layout.physical / name);
    }
    layout.mods.push_back(modPath);
  }
  return layout;
}

// The functions a listing goes through: ntdll's, or the hooks.
struct ListingFunctions
{
  NtOpenFile_type open;
  NtQueryDirectoryFile_type query;
  NtClose_type close;
};

// Opens a directory for listing into handle, and returns the status of the open.
NTSTATUS openDirectory(NtOpenFile_type open, const std::filesystem::path& path,
                       HANDLE& handle)
{
  usvfs::UnicodeString name((L"\\??\\" + path.wstring()).c_str());
  OBJECT_ATTRIBUTES attributes{.Length             = sizeof(attributes),
                               .RootDirectory      = nullptr,
                               .ObjectName         = static_cast<PUNICODE_STRING>(name),
                               .Attributes         = 0,
                               .SecurityDescriptor = nullptr,
                               .SecurityQualityOfService = nullptr};
  IO_STATUS_BLOCK statusBlock;
  return open(&handle, FILE_LIST_DIRECTORY | SYNCHRONIZE, &attributes, &statusBlock,
              FILE_SHARE_READ | FILE_SHARE_WRITE,
              FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
}

// Lists a directory to its end, and returns the number of entries listed.
std::size_t listDirectory(HANDLE directory, NtQueryDirectoryFile_type query)
{
  // many records per query, so that the time goes to the entries rather than to the
  // calls
  alignas(8) static std::array<std::byte, 0x10000> buffer;
  std::size_t entryCount = 0;
  IO_STATUS_BLOCK statusBlock;
  while (query(directory, nullptr, nullptr, nullptr, &statusBlock, buffer.data(),
               static_cast<ULONG>(buffer.size()), FileBothDirectoryInformation, FALSE,
               nullptr, FALSE) == STATUS_SUCCESS) {
    for (std::size_t offset = 0;;) {
      ++entryCount;
      ULONG nextEntryOffset;
      std::memcpy(&nextEntryOffset, buffer.data() + offset, sizeof(nextEntryOffset));
      if (nextEntryOffset == 0) {
        break;
      }
      offset += nextEntryOffset;
    }
  }
  return entryCount;
}

void measureListing(benchmark::State& state, const std::filesystem::path& directory,
                    const ListingFunctions& functions, std::size_t entryCount)
{
  openCount  = 0;
  queryCount = 0;
  for (auto _ : state) {
    HANDLE handle         = INVALID_HANDLE_VALUE;
    const NTSTATUS status = openDirectory(functions.open, directory, handle);
    if (status != STATUS_SUCCESS) {
      state.SkipWithError(std::format("failed to open {}: {:#010x}", directory.string(),
                                      static_cast<ULONG>(status)));
      return;
    }
    const std::size_t listedCount = listDirectory(handle, functions.query);
    functions.close(handle);
    if (listedCount != entryCount) {
      state.SkipWithError(
          std::format("listed {} entries instead of {}", listedCount, entryCount));
      return;
    }
  }
  // averaged per listing, the open and queries of the listing itself included
  state.counters["opens"]   = benchmark::Counter(static_cast<double>(openCount),
                                                 benchmark::Counter::kAvgIterations);
  state.counters["queries"] = benchmark::Counter(static_cast<double>(queryCount),
                                                 benchmark::Counter::kAvgIterations);
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(entryCount));
}

void NativeListing(benchmark::State& state)
{
  const Layout& layout = layoutOf(static_cast<std::size_t>(state.range(0)),
                                  static_cast<std::size_t>(state.range(1)));
  measureListing(state, layout.physical,
                 {::NtOpenFile, ::NtQueryDirectoryFile, ::NtClose}, layout.entryCount);
}

const ListingFunctions HOOKS{&usvfs::hook_NtOpenFile, &usvfs::hook_NtQueryDirectoryFile,
                             &usvfs::hook_NtClose};

// Maps the mods of the layout into its data directory, and nothing else.
void mapMods(const Layout& layout)
{
  usvfsClearVirtualMappings();
  for (const std::filesystem::path& mod : layout.mods) {
    usvfsVirtualLinkDirectoryStatic(mod.c_str(), layout.data.c_str(),
                                    LINKFLAG_RECURSIVE);
  }
}

void VirtualListing(benchmark::State& state)
{
  const Layout& layout = layoutOf(static_cast<std::size_t>(state.range(0)),
                                  static_cast<std::size_t>(state.range(1)));
  mapMods(layout);
  measureListing(state, layout.data, HOOKS, layout.entryCount);
}

// The hooks listing a directory that nothing is mapped into, as most directories a
// process lists are, while the mods are mapped into another one.
void UnmappedListing(benchmark::State& state)
{
  const Layout& layout = layoutOf(static_cast<std::size_t>(state.range(0)),
                                  static_cast<std::size_t>(state.range(1)));
  mapMods(layout);
  measureListing(state, layout.physical, HOOKS, layout.entryCount);
}

void layoutArguments(benchmark::Benchmark* benchmark)
{
  benchmark->ArgNames({"mods", "files"})
      ->ArgsProduct({{1, 8}, {64, 512}})
      ->Unit(benchmark::kMillisecond);
}

BENCHMARK(NativeListing)->Apply(layoutArguments);
BENCHMARK(VirtualListing)->Apply(layoutArguments);
BENCHMARK(UnmappedListing)->Apply(layoutArguments);

}  // namespace

int main(int argc, char** argv)
{
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
    return 1;
  }

  const std::filesystem::path dllPath =
      test::path_of_usvfs_lib(test::platform_dependant_executable("usvfs", "dll"));
  test::ScopedLoadLibrary loadDll(dllPath.c_str());
  if (!loadDll) {
    std::wcerr << L"failed to load usvfs dll: " << dllPath.c_str() << L", "
               << ::GetLastError() << std::endl;
    return 1;
  }

  // logging at the debug level, the default, would measure the logging too
  std::unique_ptr<usvfsParameters, decltype(&usvfsFreeParameters)> parameters{
      usvfsCreateParameters(), &usvfsFreeParameters};
  usvfsSetInstanceName(parameters.get(), "usvfs_directory_bench");
  usvfsSetLogLevel(parameters.get(), LogLevel::Warning);
  usvfsSetCrashDumpType(parameters.get(), CrashDumpsType::None);
  usvfsInitLogging(false);
  usvfsCreateVFS(parameters.get());
  usvfsUpdateParameters(parameters.get());

  // the counting hooks come from a thooklib of their own, which logs to the loggers of
  // this module rather than to those of the usvfs dll
  for (const char* loggerName : {"usvfs", "hooks"}) {
    spdlog::stdout_logger_mt(loggerName)->set_level(spdlog::level::warn);
  }
  HookLib::TrampolinePool::initialize();
  originalNtOpenFile   = hookNtdll("NtOpenFile", &countNtOpenFile);
  originalNtCreateFile = hookNtdll("NtCreateFile", &countNtCreateFile);
  originalNtQueryDirectoryFile =
      hookNtdll("NtQueryDirectoryFile", &countNtQueryDirectoryFile);
  originalNtQueryDirectoryFileEx =
      hookNtdll("NtQueryDirectoryFileEx", &countNtQueryDirectoryFileEx);

  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();

  for (const HookLib::HOOKHANDLE hook : counterHooks) {
    HookLib::RemoveHook(hook);
  }
  usvfsDisconnectVFS();
  return 0;
}
