/*
Userspace Virtual Filesystem

Copyright (C) 2015 Sebastian Herbord. All rights reserved.

This file is part of usvfs.

usvfs is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

usvfs is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with usvfs. If not, see <http://www.gnu.org/licenses/>.
*/

// this file depends on so many stuff that the easiest way is to include
// pch.h from shared
#include "pch.h"

#include <test_helpers.h>

#pragma warning (push, 3)
#include <iostream>
#include <gtest/gtest.h>

#include <fstream>
#pragma warning (pop)


#include <inject.h>
#include <windows_sane.h>
#include <stringutils.h>

#include <spdlog.h>

#include <hookcontext.h>
#include <unicodestring.h>
#include <stringcast.h>
#include <hooks/kernel32.h>
#include <hooks/ntdll.h>
#include <usvfs.h>
#include <logging.h>



namespace spd = spdlog;

namespace ush = usvfs::shared;


// name of a file to be created in the virtual fs. Shouldn't exist on disc but the directory must exist
static LPCSTR  VIRTUAL_FILEA = "C:/np.exe";
static LPCWSTR VIRTUAL_FILEW = L"C:/np.exe";

// a real file on disc that has to exist
static LPCSTR  REAL_FILEA = "C:/windows/notepad.exe";
static LPCWSTR REAL_FILEW = L"C:/windows/notepad.exe";

static LPCSTR  REAL_DIRA = "C:/windows/Logs";
static LPCWSTR REAL_DIRW = L"C:/windows/Logs";


static std::shared_ptr<spdlog::logger> logger()
{
  std::shared_ptr<spdlog::logger> result = spdlog::get("test");
  if (result.get() == nullptr) {
    result = spdlog::stdout_logger_mt("test");
  }
  return result;
}

auto defaultUsvfsParams(const char* instanceName = "usvfs_test") {
  std::unique_ptr<usvfsParameters, decltype(&usvfsFreeParameters)> parameters{ 
    usvfsCreateParameters(), &usvfsFreeParameters };

  usvfsSetInstanceName(parameters.get(), instanceName);
  usvfsSetDebugMode(parameters.get(), true);
  usvfsSetLogLevel(parameters.get(), LogLevel::Debug);
  usvfsSetCrashDumpType(parameters.get(), CrashDumpsType::None);
  usvfsSetCrashDumpPath(parameters.get(), "");

  return std::move(parameters);
}

class USVFSTest : public testing::Test
{
public:
  void SetUp() {
    SHMLogger::create("usvfs");
    // need to initialize logging in the context of the dll
    usvfsInitLogging();
  }

  void TearDown() {
    std::array<char, 1024> buffer;
    while (SHMLogger::instance().tryGet(buffer.data(), buffer.size())) {
      std::cout << buffer.data() << std::endl;
    }
    SHMLogger::free();
  }

private:
};

class USVFSTestWithReroute : public testing::Test
{
public:
  void SetUp() {
    SHMLogger::create("usvfs");
    // need to initialize logging in the context of the dll
    usvfsInitLogging();

    auto params = defaultUsvfsParams();
    m_Context.reset(usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));
    usvfs::RedirectionTreeContainer &tree = m_Context->redirectionTable();
    tree.addFile(ush::string_cast<std::string>(VIRTUAL_FILEW, ush::CodePage::UTF8).c_str()
                 , usvfs::RedirectionDataLocal(REAL_FILEA));
  }

  void TearDown() {
    std::array<char, 1024> buffer;
    while (SHMLogger::instance().tryGet(buffer.data(), buffer.size())) {
      std::cout << buffer.data() << std::endl;
    }
    m_Context.reset();
    SHMLogger::free();
  }
private:
  std::unique_ptr<usvfs::HookContext> m_Context;
};

class USVFSTestAuto : public testing::Test
{
public:
  void SetUp() {
    auto params = defaultUsvfsParams();
    usvfsConnectVFS(params.get());
    SHMLogger::create("usvfs");
  }

  void TearDown() {
    usvfsDisconnectVFS();

    std::array<char, 1024> buffer;
    while (SHMLogger::instance().tryGet(buffer.data(), buffer.size())) {
      std::cout << buffer.data() << std::endl;
    }
    SHMLogger::free();
  }

private:
};


TEST_F(USVFSTest, CanResizeRedirectiontree)
{
  using usvfs::shared::MissingThrow;
  ASSERT_NO_THROW({
      usvfs::RedirectionTreeContainer container("treetest_shm", 1024);
      for (char i = 'a'; i <= 'z'; ++i) {
        for (char j = 'a'; j <= 'z'; ++j) {
          std::string name = std::string(R"(C:\temp\)") + i + j;
          container.addFile(name, usvfs::RedirectionDataLocal("gaga"), false);
        }
      }

      ASSERT_EQ("gaga", container->node("C:")->node("temp")->node("aa", MissingThrow)->data().linkTarget);
      ASSERT_EQ("gaga", container->node("C:")->node("temp")->node("az", MissingThrow)->data().linkTarget);
  });
}

/*
TEST_F(USVFSTest, CreateFileHookReportsCorrectErrorOnMissingFile)
{
  ASSERT_NO_THROW({
    USVFSParameters params;
    USVFSInitParameters(&params, "usvfs_test", true, LogLevel::Debug, CrashDumpsType::None, "");
    std::unique_ptr<usvfs::HookContext> ctx(CreateHookContext(params, ::GetModuleHandle(nullptr)));
    HANDLE res = usvfs::hook_CreateFileW(VIRTUAL_FILEW
                                     , GENERIC_READ
                                     , FILE_SHARE_READ | FILE_SHARE_WRITE
                                     , nullptr
                                     , OPEN_EXISTING
                                     , FILE_ATTRIBUTE_NORMAL
                                     , nullptr);

    ASSERT_EQ(INVALID_HANDLE_VALUE, res);
    ASSERT_EQ(ERROR_FILE_NOT_FOUND, ::GetLastError());
  });
}
*/

/*
TEST_F(USVFSTestWithReroute, CreateFileHookRedirectsFile)
{
  ASSERT_NE(INVALID_HANDLE_VALUE
            , usvfs::hook_CreateFileW(VIRTUAL_FILEW
                                  , GENERIC_READ
                                  , FILE_SHARE_READ | FILE_SHARE_WRITE
                                  , nullptr
                                  , OPEN_EXISTING
                                  , FILE_ATTRIBUTE_NORMAL
                                  , nullptr));
}
*/


TEST_F(USVFSTest, GetFileAttributesHookReportsCorrectErrorOnMissingFile)
{
  ASSERT_NO_THROW({
    try {
        auto params = defaultUsvfsParams();
        std::unique_ptr<usvfs::HookContext> ctx(usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));
        DWORD res = usvfs::hook_GetFileAttributesW(VIRTUAL_FILEW);

        ASSERT_EQ(INVALID_FILE_ATTRIBUTES, res);
        ASSERT_EQ(ERROR_FILE_NOT_FOUND, ::GetLastError());
    } catch (const std::exception& e) {
        logger()->error("Exception: {}", e.what());
        throw;
    }
  });
}

TEST_F(USVFSTest, GetFileAttributesHookRedirectsFile)
{
  auto params = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));
  usvfs::RedirectionTreeContainer &tree = ctx->redirectionTable();

  tree.addFile(ush::string_cast<std::string>(VIRTUAL_FILEW, ush::CodePage::UTF8).c_str()
               , usvfs::RedirectionDataLocal(REAL_FILEA));

  ASSERT_EQ(::GetFileAttributesW(REAL_FILEW)
            , usvfs::hook_GetFileAttributesW(VIRTUAL_FILEW));
}
/*
TEST_F(USVFSTest, GetFullPathNameOnRegularCurrentDirectory)
{
  USVFSParameters params;
  USVFSInitParameters(&params, "usvfs_test", true, LogLevel::Debug, CrashDumpsType::None, "");
  std::unique_ptr<usvfs::HookContext> ctx(CreateHookContext(params, ::GetModuleHandle(nullptr)));

  std::wstring expected = winapi::wide::getCurrentDirectory() + L"\\filename.txt";

  DWORD bufferLength = 32767;
  std::unique_ptr<wchar_t[]> buffer(new wchar_t[bufferLength]);
  LPWSTR filePart = nullptr;

  DWORD res = usvfs::hook_GetFullPathNameW(L"filename.txt", bufferLength, buffer.get(), &filePart);

  ASSERT_NE(0UL, res);
  ASSERT_EQ(expected, std::wstring(buffer.get()));
}*/

// small wrapper to call usvfs::hook_NtOpenFile with a path
//
// at some point in time, changes were made to USVFS such that calling a hooked
// function from a handle obtained from a non-hooked function would not work anymore, 
// meaning that function such as CreateFileW that have no hook equivalent cannot
// be used to test hook functions
// 
// this function is useful to simulate a CreateFileW by internally using the hook
// version of NtOpenFile
//
HANDLE hooked_NtOpenFile(LPWSTR path, ACCESS_MASK accessMask, ULONG shareAccess, ULONG openOptions)
{
  constexpr size_t BUFFER_SIZE = 2048;
  IO_STATUS_BLOCK statusBlock;
  OBJECT_ATTRIBUTES attributes;
  attributes.SecurityDescriptor = 0;
  attributes.SecurityQualityOfService = 0;
  attributes.RootDirectory = 0;
  attributes.Attributes = 0;
  attributes.Length = sizeof(OBJECT_ATTRIBUTES);

  WCHAR stringBuffer[BUFFER_SIZE];
  UNICODE_STRING  string;
  string.Buffer = stringBuffer;
  lstrcpyW(stringBuffer, L"\\??\\");
  lstrcatW(stringBuffer, path);
  string.Length = lstrlenW(stringBuffer) * 2; 
  string.MaximumLength = BUFFER_SIZE;
  attributes.ObjectName = &string;
  
  HANDLE ret = INVALID_HANDLE_VALUE;
  if (usvfs::hook_NtOpenFile(&ret, accessMask | SYNCHRONIZE, 
    &attributes, &statusBlock, shareAccess, 
    openOptions | FILE_SYNCHRONOUS_IO_NONALERT) != STATUS_SUCCESS)
  {
    return INVALID_HANDLE_VALUE;
  }

  return ret;
}

TEST_F(USVFSTest, NtQueryDirectoryFileRegularFile)
{
  auto params = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));

  HANDLE hdl = hooked_NtOpenFile(
    L"C:\\"
    , FILE_GENERIC_READ
    , FILE_SHARE_READ | FILE_SHARE_WRITE
    , OPEN_EXISTING);
  ASSERT_NE(INVALID_HANDLE_VALUE, hdl);

  IO_STATUS_BLOCK status;
  char buffer[1024];

  usvfs::hook_NtQueryDirectoryFile(hdl
                               , nullptr
                               , nullptr
                               , nullptr
                               , &status
                               , buffer
                               , 1024
                               , FileDirectoryInformation
                               , TRUE
                               , nullptr
                               , TRUE);

  ASSERT_EQ(STATUS_SUCCESS, status.Status);
}

TEST_F(USVFSTest, NtQueryDirectoryFileFindsVirtualFile)
{
  auto params = defaultUsvfsParams();
  std::unique_ptr<usvfs::HookContext> ctx(usvfsCreateHookContext(*params, ::GetModuleHandle(nullptr)));
  usvfs::RedirectionTreeContainer &tree = ctx->redirectionTable();

  tree.addFile(L"C:\\np.exe", usvfs::RedirectionDataLocal(REAL_FILEA));

  HANDLE hdl = hooked_NtOpenFile(
    L"C:\\"
    , FILE_GENERIC_READ
    , FILE_SHARE_READ | FILE_SHARE_WRITE
    , OPEN_EXISTING);
  ASSERT_NE(INVALID_HANDLE_VALUE, hdl);

  IO_STATUS_BLOCK status;
  char buffer[1024];

  usvfs::UnicodeString fileName(L"np.exe");

  usvfs::hook_NtQueryDirectoryFile(hdl
                               , nullptr
                               , nullptr
                               , nullptr
                               , &status
                               , buffer
                               , 1024
                               , FileDirectoryInformation
                               , TRUE
                               , static_cast<PUNICODE_STRING>(fileName)
                               , TRUE);

  FILE_DIRECTORY_INFORMATION *info = reinterpret_cast<FILE_DIRECTORY_INFORMATION*>(buffer);
  ASSERT_EQ(STATUS_SUCCESS, status.Status);
  ASSERT_EQ(0, wcscmp(info->FileName, L"np.exe"));
}

TEST_F(USVFSTestAuto, CannotCreateLinkToFileInNonexistantDirectory)
{
  ASSERT_EQ(FALSE, usvfsVirtualLinkFile(REAL_FILEW, L"c:/this_directory_shouldnt_exist/np.exe", FALSE));
}

TEST_F(USVFSTestAuto, CanCreateMultipleLinks)
{
  static LPCWSTR outFile = LR"(C:\np.exe)";
  static LPCWSTR outDir  = LR"(C:\logs)";
  static LPCWSTR outDirCanonizeTest = LR"(C:\.\not/../logs\.\a\.\b\.\c\..\.\..\.\..\)";
  ASSERT_EQ(TRUE, usvfsVirtualLinkFile(REAL_FILEW, outFile, 0));
  ASSERT_EQ(TRUE, usvfsVirtualLinkDirectoryStatic(REAL_DIRW, outDir, 0));

  // both file and dir exist and have the correct type
  ASSERT_NE(INVALID_FILE_ATTRIBUTES, usvfs::hook_GetFileAttributesW(outFile));
  ASSERT_NE(INVALID_FILE_ATTRIBUTES, usvfs::hook_GetFileAttributesW(outDir));
  ASSERT_EQ(0UL, usvfs::hook_GetFileAttributesW(outFile) & FILE_ATTRIBUTE_DIRECTORY);
  ASSERT_NE(0UL, usvfs::hook_GetFileAttributesW(outDir)  & FILE_ATTRIBUTE_DIRECTORY);
  ASSERT_NE(0UL, usvfs::hook_GetFileAttributesW(outDirCanonizeTest) & FILE_ATTRIBUTE_DIRECTORY);
}

int main(int argc, char **argv) {
  using namespace test;

  auto dllPath = path_of_usvfs_lib(platform_dependant_executable("usvfs", "dll"));
  ScopedLoadLibrary loadDll(dllPath.c_str());
  if (!loadDll) {
    std::wcerr << L"failed to load usvfs dll: " << dllPath.c_str() << L", " << GetLastError() << std::endl;
    return 1;
  }

  // note: this makes the logger available only to functions statically linked to the test binary, not those
  // called in the dll
  auto logger = spdlog::stdout_logger_mt("usvfs");
  logger->set_level(spdlog::level::warn);
  testing::InitGoogleTest(&argc, argv);
  int res = RUN_ALL_TESTS();

  return res;
}
