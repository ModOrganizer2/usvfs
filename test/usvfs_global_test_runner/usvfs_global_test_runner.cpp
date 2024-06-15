#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>

#include <boost/algorithm/string.hpp>

#include <gtest/gtest.h>

#include "stringcast.h"
#include "test_helpers.h"
#include "usvfs.h"
#include "windows_sane.h"

// find the path to the USVFS DLL that contains gtest entries
//
std::filesystem::path path_to_usvfs_dll()
{
  return test::path_of_usvfs_lib(test::platform_dependant_executable("usvfs", "dll"));
}

// find the path to the executable that contains gtest entries
//
std::filesystem::path path_to_usvfs_global_test()
{
  return test::path_of_test_bin(
      test::platform_dependant_executable("usvfs_global_test", "exe"));
}

// path to the fixture for the given test group
//
std::filesystem::path path_to_usvfs_global_test_figures(std::wstring_view group)
{
  return test::path_of_test_fixtures() / "usvfs_global_test" / group;
}

// spawn the an hook version of the given
//
DWORD spawn_usvfs_hooked_process(
    const std::filesystem::path& app, const std::vector<std::wstring>& arguments = {},
    const std::optional<std::filesystem::path>& working_directory = {})
{
  using namespace usvfs::shared;

  std::wstring command      = app;
  std::filesystem::path cwd = working_directory.value_or(app.parent_path());
  std::vector<wchar_t> env;

  if (!arguments.empty()) {
    command += L" " + boost::algorithm::join(arguments, L" ");
  }

  STARTUPINFO si{0};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{0};

#pragma warning(push)
#pragma warning(disable : 6387)

  if (!usvfsCreateProcessHooked(nullptr, command.data(), nullptr, nullptr, FALSE, 0,
                                nullptr, cwd.c_str(), &si, &pi)) {
    test::throw_testWinFuncFailed(
        "CreateProcessHooked",
        string_cast<std::string>(command, CodePage::UTF8).c_str());
  }

  WaitForSingleObject(pi.hProcess, INFINITE);

  DWORD exit = 99;
  if (!GetExitCodeProcess(pi.hProcess, &exit)) {
    test::WinFuncFailedGenerator failed;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    throw failed("GetExitCodeProcess");
  }

  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);

#pragma warning(pop)

  return exit;
}

struct comparison_result
{
  std::vector<std::filesystem::path> left_only;
  std::vector<std::filesystem::path> right_only;
  std::vector<std::filesystem::path> type_differ;
  std::vector<std::filesystem::path> content_differ;

  bool empty() const
  {
    return left_only.empty() && right_only.empty() && type_differ.empty() &&
           content_differ.empty();
  }
};

comparison_result compare_directories(const std::filesystem::path& left,
                                      const std::filesystem::path& right,
                                      bool content = true)
{
  comparison_result result;
  std::vector<std::filesystem::path> in_both;

  // iterate left, check on right
  for (const auto& it : std::filesystem::recursive_directory_iterator{left}) {
    const auto relpath = relative(it.path(), left);
    if (!exists(right / relpath)) {
      result.left_only.push_back(relpath);
    } else {
      in_both.push_back(relpath);
    }
  }

  // iterate right, check on left
  for (const auto& it : std::filesystem::recursive_directory_iterator{right}) {
    const auto relpath = relative(it.path(), right);
    if (!exists(left / relpath)) {
      result.right_only.push_back(relpath);
    }
  }

  // check contents
  if (content) {
    for (const auto& relpath : in_both) {
      const auto left_path = left / relpath, right_path = right / relpath;

      if (is_directory(left_path) != is_directory(right_path)) {
        result.type_differ.push_back(relpath);
        continue;
      }

      if (is_directory(left_path)) {
        continue;
      }

      if (!test::compare_files(left_path, right_path, true)) {
        result.content_differ.push_back(relpath);
      }
    }
  }

  return result;
}

class usvfs_guard
{
public:
  usvfs_guard(std::string_view instance_name = "usvfs_test", bool logging = false)
      : m_parameters(usvfsCreateParameters(), &usvfsFreeParameters)
  {
    usvfsSetInstanceName(m_parameters.get(), instance_name.data());
    usvfsSetDebugMode(m_parameters.get(), false);
    usvfsSetLogLevel(m_parameters.get(), LogLevel::Debug);
    usvfsSetCrashDumpType(m_parameters.get(), CrashDumpsType::None);
    usvfsSetCrashDumpPath(m_parameters.get(), "");

    usvfsInitLogging(logging);
    usvfsCreateVFS(m_parameters.get());
  }

  ~usvfs_guard() { usvfsDisconnectVFS(); }

private:
  std::unique_ptr<usvfsParameters, decltype(&usvfsFreeParameters)> m_parameters;
};

class usvfs_test_runner
{
public:
  usvfs_test_runner(std::wstring_view test_group)
      : m_group{test_group}, m_temporary_folder{test::path_of_test_temp()}
  {}
  ~usvfs_test_runner() { cleanup(); }

  // run the test, return the exit code of the google test process
  //
  int run() const;

private:
  // prepare the filesystem by copying files and folders from the relevant fixtures
  // folder to the temporary folder
  //
  // after this operations, the temporary folder will contain
  // - a data folder
  // - [optional] a mods folder containing a set of folders that should be mounted
  // - [optional] an overwrite folder that should be mounted as overwrite
  //
  void prepare_filesystem() const;

  // prepare mapping using the given set of paths
  //
  void prepare_mapping(const std::filesystem::path& data,
                       const std::filesystem::path& mods,
                       const std::filesystem::path& overwrite) const;

  // cleanup the temporary path
  //
  void cleanup() const;

  // name of GTest group (first argument of the TEST macro) to run
  std::wstring m_group;

  // path to the folder containing temporary files
  std::filesystem::path m_temporary_folder;
};

void usvfs_test_runner::cleanup() const
{
  if (exists(m_temporary_folder)) {
    remove_all(m_temporary_folder);
  }
}

void usvfs_test_runner::prepare_filesystem() const
{
  // cleanup in case a previous tests failed to delete its stuff
  cleanup();

  // copy fixtures
  const auto fixtures = path_to_usvfs_global_test_figures(m_group) / "source";
  if (exists(fixtures)) {
    copy(fixtures, m_temporary_folder, std::filesystem::copy_options::recursive);
  }
}

void usvfs_test_runner::prepare_mapping(const std::filesystem::path& data,
                                        const std::filesystem::path& mods,
                                        const std::filesystem::path& overwrite) const
{
  // should not be needed, but just to be safe
  usvfsClearVirtualMappings();

  if (!exists(data)) {
    throw std::runtime_error{std::format("data path missing at {}", data.string())};
  }

  if (exists(mods)) {
    for (const auto& mod : std::filesystem::directory_iterator(mods)) {
      if (!is_directory(mod)) {
        continue;
      }
      usvfsVirtualLinkDirectoryStatic(mod.path().c_str(), data.c_str(),
                                      LINKFLAG_RECURSIVE);
    }
  }

  if (exists(overwrite)) {
    usvfsVirtualLinkDirectoryStatic(overwrite.c_str(), data.c_str(),
                                    LINKFLAG_CREATETARGET | LINKFLAG_RECURSIVE);
  }
}

int usvfs_test_runner::run() const
{
  // copy files from fixtures to a relevant location
  prepare_filesystem();

  // data folder
  const auto data = m_temporary_folder / L"data";

  // prepare usvfs and mapping
  usvfs_guard guard{"usvfs_test", true};

  prepare_mapping(data, m_temporary_folder / L"mods",
                  m_temporary_folder / L"overwrite");

  const auto res = spawn_usvfs_hooked_process(
      path_to_usvfs_global_test(),
      {std::format(L"--gtest_filter={}.*", m_group), data.native()});

  if (res != 0) {
    const auto log_path = test::path_of_test_bin(m_group + L".log");
    std::ofstream os{log_path};
    std::string buffer(1024, '\0');
    std::cout << "process returned " << std::hex << res << ", usvfs logs dumped to "
              << log_path.string() << '\n';
    while (usvfsGetLogMessages(buffer.data(), buffer.size(), false)) {
      os << "  " << buffer.c_str() << "\n";
    }
    return res;
  }

  // check contents of directories
  auto diff =
      compare_directories(path_to_usvfs_global_test_figures(m_group) / "expected",
                          m_temporary_folder, true);

  if (!diff.empty()) {
    std::cout << "expected and actual folder content differs\n";
    std::cout << "  expected but not found:\n";
    for (auto& path : diff.left_only) {
      std::cout << "    " << path.string() << "\n";
    }
    std::cout << "  not expected but found:\n";
    for (auto& path : diff.right_only) {
      std::cout << "    " << path.string() << "\n";
    }
    std::cout << "  content differ:\n";
    for (auto& path : diff.content_differ) {
      std::cout << "    " << path.string() << "\n";
    }
    return -1;
  }

  return res;
}

TEST(BasicTest, Test)
{
  ASSERT_EQ(0, usvfs_test_runner(L"BasicTest").run());
}

TEST(RedFileSystemTest, Test)
{
  ASSERT_EQ(0, usvfs_test_runner(L"RedFileSystemTest").run());
}

int main(int argc, char* argv[])
{
  // load the USVFS DLL
  //
  const auto usvfs_dll = path_to_usvfs_dll();
  test::ScopedLoadLibrary loadDll(usvfs_dll.c_str());
  if (!loadDll) {
    std::wcerr << L"ERROR: failed to load usvfs dll: " << usvfs_dll.c_str() << L", "
               << GetLastError() << L"\n";
    return 3;
  }

  testing::InitGoogleTest(&argc, argv);

  usvfsInitLogging(false);

  return RUN_ALL_TESTS();
}
