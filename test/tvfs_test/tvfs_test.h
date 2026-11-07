#pragma once

#include <array>
#include <iostream>
#include <memory>

#include <gtest/gtest.h>

#include <hooks/ntdll.h>
#include <shmlogger.h>
#include <usvfs.h>

// a real file on disc that has to exist
inline LPCSTR REAL_FILEA  = "C:/windows/notepad.exe";
inline LPCWSTR REAL_FILEW = L"C:/windows/notepad.exe";

inline auto defaultUsvfsParams(const char* instanceName = "usvfs_test")
{
  std::unique_ptr<usvfsParameters, decltype(&usvfsFreeParameters)> parameters{
      usvfsCreateParameters(), &usvfsFreeParameters};

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
  void SetUp()
  {
    SHMLogger::create("usvfs");
    // need to initialize logging in the context of the dll
    usvfsInitLogging();
  }

  void TearDown()
  {
    std::array<char, 1024> buffer;
    while (SHMLogger::instance().tryGet(buffer.data(), buffer.size())) {
      std::cout << buffer.data() << std::endl;
    }
    SHMLogger::free();
  }

private:
};

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
inline HANDLE hooked_NtOpenFile(LPCWSTR path, ACCESS_MASK accessMask, ULONG shareAccess,
                                ULONG openOptions)
{
  constexpr size_t BUFFER_SIZE = 2048;
  IO_STATUS_BLOCK statusBlock;
  OBJECT_ATTRIBUTES attributes;
  attributes.SecurityDescriptor       = 0;
  attributes.SecurityQualityOfService = 0;
  attributes.RootDirectory            = 0;
  attributes.Attributes               = 0;
  attributes.Length                   = sizeof(OBJECT_ATTRIBUTES);

  WCHAR stringBuffer[BUFFER_SIZE];
  UNICODE_STRING string;
  string.Buffer = stringBuffer;
  lstrcpyW(stringBuffer, L"\\??\\");
  lstrcatW(stringBuffer, path);
  string.Length         = static_cast<USHORT>(lstrlenW(stringBuffer) * 2);
  string.MaximumLength  = BUFFER_SIZE;
  attributes.ObjectName = &string;

  HANDLE ret = INVALID_HANDLE_VALUE;
  if (usvfs::hook_NtOpenFile(&ret, accessMask, &attributes, &statusBlock, shareAccess,
                             openOptions) != STATUS_SUCCESS) {
    return INVALID_HANDLE_VALUE;
  }

  return ret;
}
