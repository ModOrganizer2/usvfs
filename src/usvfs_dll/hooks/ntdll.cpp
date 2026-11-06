#include "ntdll.h"

#include <boost/filesystem.hpp>

#include <loghelpers.h>
#include <stringcast.h>
#include <stringutils.h>
#include <unicodestring.h>
#include <usvfs.h>

#include "../hookcallcontext.h"
#include "../hookcontext.h"
#include "../maptracker.h"
#include "../stringcast_boost.h"

#include "directory_scan.h"
#include "handle_tracker.h"

namespace ulog = usvfs::log;
namespace ush  = usvfs::shared;
namespace bfs  = boost::filesystem;

using usvfs::UnicodeString;

// flag definitions below are copied from winternl.h
#define FILE_SUPERSEDE 0x00000000
#define FILE_OPEN 0x00000001
#define FILE_CREATE 0x00000002
#define FILE_OPEN_IF 0x00000003
#define FILE_OVERWRITE 0x00000004
#define FILE_OVERWRITE_IF 0x00000005
#define FILE_MAXIMUM_DISPOSITION 0x00000005

template <typename T>
using unique_ptr_deleter = std::unique_ptr<T, void (*)(T*)>;

class RedirectionInfo
{
public:
  UnicodeString path;
  bool redirected;

  RedirectionInfo() {}
  RedirectionInfo(UnicodeString path, bool redirected)
      : path(path), redirected(redirected)
  {}
};

HandleTracker& ntdllHandleTracker = *new HandleTracker;

UnicodeString CreateUnicodeString(const OBJECT_ATTRIBUTES* objectAttributes)
{
  UnicodeString result = ntdllHandleTracker.lookup(objectAttributes->RootDirectory);
  if (objectAttributes->ObjectName != nullptr) {
    result.appendPath(objectAttributes->ObjectName);
  }
  return result;
}

std::ostream& operator<<(std::ostream& os, const _UNICODE_STRING& str)
{
  try {
    // TODO this does not correctly support surrogate pairs
    // since the size used here is the number of 16-bit characters in the buffer
    // whereas
    // toNarrow expects the actual number of characters.
    // It will always underestimate though, so worst case scenario we truncate
    // the string
    os << ush::string_cast<std::string>(str.Buffer, ush::CodePage::UTF8,
                                        str.Length / sizeof(WCHAR));
  } catch (const std::exception& e) {
    os << e.what();
  }

  return os;
}

std::ostream& operator<<(std::ostream& os, POBJECT_ATTRIBUTES attr)
{
  return operator<<(os, *attr->ObjectName);
}

RedirectionInfo applyReroute(const usvfs::HookContext::ConstPtr& context,
                             const usvfs::HookCallContext& callContext,
                             const UnicodeString& inPath)
{
  RedirectionInfo result;
  result.path       = inPath;
  result.redirected = false;

  if (callContext.active()) {
    // see if the file exists in the redirection tree
    std::string lookupPath = ush::string_cast<std::string>(
        static_cast<LPCWSTR>(result.path) + 4, ush::CodePage::UTF8);
    auto node = context->redirectionTable()->findNode(lookupPath.c_str());
    // if so, replace the file name with the path to the mapped file
    if ((node.get() != nullptr) &&
        (!node->data().linkTarget.empty() || node->isDirectory())) {
      std::wstring reroutePath;

      if (node->data().linkTarget.length() > 0) {
        reroutePath = ush::string_cast<std::wstring>(node->data().linkTarget.c_str(),
                                                     ush::CodePage::UTF8);
      } else {
        reroutePath =
            ush::string_cast<std::wstring>(node->path().c_str(), ush::CodePage::UTF8);
      }
      if ((*reroutePath.rbegin() == L'\\') && (*lookupPath.rbegin() != '\\')) {
        reroutePath.resize(reroutePath.size() - 1);
      }
      std::replace(reroutePath.begin(), reroutePath.end(), L'/', L'\\');
      if (reroutePath[1] == L'\\')
        reroutePath[1] = L'?';
      result.path       = LR"(\??\)" + reroutePath;
      result.redirected = true;
    }
  }
  return result;
}

RedirectionInfo applyReroute(const usvfs::CreateRerouter& rerouter)
{
  RedirectionInfo result;
  result.path       = UnicodeString(rerouter.fileName());
  result.redirected = rerouter.wasRerouted();

  std::wstring reroutePath(static_cast<LPCWSTR>(result.path));
  std::replace(reroutePath.begin(), reroutePath.end(), L'/', L'\\');
  if (reroutePath[1] == L'\\')
    reroutePath[1] = L'?';
  if (!((reroutePath[0] == L'\\') && (reroutePath[1] == L'?') &&
        (reroutePath[2] == L'?') && (reroutePath[3] == L'\\'))) {
    result.path = LR"(\??\)" + reroutePath;
  }

  return result;
}

struct FindCreateTarget
{
  usvfs::RedirectionTree::NodePtrT target;
  void operator()(usvfs::RedirectionTree::NodePtrT node)
  {
    if (node->hasFlag(usvfs::shared::FLAG_CREATETARGET)) {
      target = node;
    }
  }
};

std::pair<UnicodeString, UnicodeString>
findCreateTarget(const usvfs::HookContext::ConstPtr& context,
                 const UnicodeString& inPath)
{
  std::pair<UnicodeString, UnicodeString> result;
  result.first  = inPath;
  result.second = UnicodeString();

  std::string lookupPath = ush::string_cast<std::string>(
      static_cast<LPCWSTR>(result.first) + 4, ush::CodePage::UTF8);
  FindCreateTarget visitor;
  usvfs::RedirectionTree::VisitorFunction visitorWrapper =
      [&](const usvfs::RedirectionTree::NodePtrT& node) {
        visitor(node);
      };
  context->redirectionTable()->visitPath(lookupPath, visitorWrapper);
  if (visitor.target.get() != nullptr) {
    bfs::path relativePath =
        ush::make_relative(visitor.target->path(), bfs::path(lookupPath));

    bfs::path target(visitor.target->data().linkTarget.c_str());
    target /= relativePath;

    result.second = UnicodeString(target.wstring().c_str());
    winapi::ex::wide::createPath(target.parent_path());
  }
  return result;
}

RedirectionInfo applyReroute(const usvfs::HookContext::ConstPtr& context,
                             const usvfs::HookCallContext& callContext,
                             POBJECT_ATTRIBUTES inAttributes)
{
  return applyReroute(context, callContext, CreateUnicodeString(inAttributes));
}

int NextDividableBy(int number, int divider)
{
  return static_cast<int>(
      ceilf(static_cast<float>(number) / static_cast<float>(divider)) * divider);
}

// Something is trying to create a variety of files starting with "\Device\",
// such as "\Device\DeviceApi\Dev\Query", "\Device\MMCSS\MmThread",
// "\Device\DeviceApi\CMNotify", etc.
//
// This used to create a bunch of spurious "Device" and "MMCSS" folders when
// using Explorer++. Some of these names seem to part of PnP, others from the
// "Multimedia Class Scheduler Service".
//
// There's basically zero information on this stuff online.
//
// Regardless, these files ended up being rerouted, so they became something
// like C:\game\Data\Somewhere\Device\MMCSS\MmThread, which ended up being
// rerouted to overwrite and created as a fake path
// "overwrite\Somewhere\Device\MMCSS"
//
// These paths are weird, they feel like pipes or something. It's not clear how
// they should be reliably recognized, so this is a hardcoded check for any
// path that starts with "\Device\". These paths will be forwarded directly
// to NtCreateFile/NtOpenFile
//
bool isDeviceFile(std::wstring_view name)
{
  static const std::wstring_view DevicePrefix(L"\\Device\\");

  // starts with
  if (name.size() < DevicePrefix.size()) {
    return false;
  } else {
    return (_wcsnicmp(name.data(), DevicePrefix.data(), DevicePrefix.size()) == 0);
  }
}

NTSTATUS WINAPI usvfs::hook_NtQueryDirectoryFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length,
    FILE_INFORMATION_CLASS FileInformationClass, BOOLEAN ReturnSingleEntry,
    PUNICODE_STRING FileName, BOOLEAN RestartScan)
{
  PreserveGetLastError ntFunctionsDoNotChangeGetLastError;
  const ULONG queryFlags = (ReturnSingleEntry ? SL_RETURN_SINGLE_ENTRY : 0) |
                           (RestartScan ? SL_RESTART_SCAN : 0);
  const DirectoryQuery query{.handle           = FileHandle,
                             .event            = Event,
                             .apcRoutine       = ApcRoutine,
                             .apcContext       = ApcContext,
                             .statusBlock      = IoStatusBlock,
                             .buffer           = FileInformation,
                             .length           = Length,
                             .informationClass = FileInformationClass,
                             .fileName         = FileName,
                             .flags            = queryFlags,
                             .isEx             = false};

  HOOK_START_GROUP(MutExHookGroup::FIND_FILES)
  if (!callContext.active()) {
    return queryNative(query);
  }
  return queryDirectory(query);
  HOOK_END
  return STATUS_NO_MORE_FILES;
}

NTSTATUS WINAPI usvfs::hook_NtQueryDirectoryFileEx(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length,
    FILE_INFORMATION_CLASS FileInformationClass, ULONG QueryFlags,
    PUNICODE_STRING FileName)
{
  PreserveGetLastError ntFunctionsDoNotChangeGetLastError;
  const DirectoryQuery query{.handle           = FileHandle,
                             .event            = Event,
                             .apcRoutine       = ApcRoutine,
                             .apcContext       = ApcContext,
                             .statusBlock      = IoStatusBlock,
                             .buffer           = FileInformation,
                             .length           = Length,
                             .informationClass = FileInformationClass,
                             .fileName         = FileName,
                             .flags            = QueryFlags,
                             .isEx             = true};

  HOOK_START_GROUP(MutExHookGroup::FIND_FILES)
  if (!callContext.active()) {
    return queryNative(query);
  }
  return queryDirectory(query);
  HOOK_END
  return STATUS_NO_MORE_FILES;
}

DLLEXPORT NTSTATUS WINAPI usvfs::hook_NtQueryObject(
    HANDLE Handle, OBJECT_INFORMATION_CLASS ObjectInformationClass,
    PVOID ObjectInformation, ULONG ObjectInformationLength, PULONG ReturnLength)
{
  NTSTATUS res = STATUS_SUCCESS;

  HOOK_START_GROUP(MutExHookGroup::FILE_ATTRIBUTES)
  if (!callContext.active()) {
    return ::NtQueryObject(Handle, ObjectInformationClass, ObjectInformation,
                           ObjectInformationLength, ReturnLength);
  }

  PRE_REALCALL
  res = ::NtQueryObject(Handle, ObjectInformationClass, ObjectInformation,
                        ObjectInformationLength, ReturnLength);
  POST_REALCALL

  // we handle both SUCCESS and BUFFER_OVERFLOW since the fixed name might be
  // smaller than the original one
  //
  // we only handle STATUS_INFO_LENGTH_MISMATCH if ReturnLength is not NULL since
  // this is only returned if the length is too small to hold the structure itself
  // (regardless of the name), in which case, we need to compute our own ReturnLength
  //
  if ((res == STATUS_SUCCESS || res == STATUS_BUFFER_OVERFLOW ||
       (res == STATUS_INFO_LENGTH_MISMATCH && ReturnLength)) &&
      (ObjectInformationClass == ObjectNameInformation)) {
    const auto trackerInfo = ntdllHandleTracker.lookup(Handle);
    const auto redir       = applyReroute(READ_CONTEXT(), callContext, trackerInfo);

    OBJECT_NAME_INFORMATION* info =
        reinterpret_cast<OBJECT_NAME_INFORMATION*>(ObjectInformation);

    if (redir.redirected) {
      // https://learn.microsoft.com/en-us/windows/win32/fileio/displaying-volume-paths
      //

      // TODO: is that always true?
      // path should start with \??\X: - we need to replace this by device name
      //
      WCHAR deviceName[MAX_PATH];
      std::wstring buffer(static_cast<LPCWSTR>(trackerInfo));
      buffer[6] = L'\0';

      QueryDosDeviceW(buffer.data() + 4, deviceName, ARRAYSIZE(deviceName));

      buffer = std::wstring(deviceName) + L'\\' +
               std::wstring(buffer.data() + 7, buffer.size() - 7);

      // the name is put in the buffer AFTER the struct, so the required size if
      // sizeof(OBJECT_NAME_INFORMATION) + the number of bytes for the name + 2 bytes
      // for a wide null character
      const auto requiredLength =
          sizeof(OBJECT_NAME_INFORMATION) + buffer.size() * 2 + 2;
      if (ObjectInformationLength < requiredLength) {
        // if the status was info length mismatch, we keep it, we are just going to
        // update *ReturnLength
        if (res != STATUS_INFO_LENGTH_MISMATCH) {
          res = STATUS_BUFFER_OVERFLOW;
        }

        if (ReturnLength) {
          *ReturnLength = static_cast<ULONG>(requiredLength);
        }
      } else {
        // put the unicode buffer at the end of the object
        const USHORT unicodeBufferLength = static_cast<USHORT>(std::min(
            static_cast<unsigned long long>(std::numeric_limits<USHORT>::max()),
            static_cast<unsigned long long>(ObjectInformationLength -
                                            sizeof(OBJECT_NAME_INFORMATION))));
        LPWSTR unicodeBuffer             = reinterpret_cast<LPWSTR>(
            static_cast<LPSTR>(ObjectInformation) + sizeof(OBJECT_NAME_INFORMATION));

        // copy the path into the buffer
        wmemcpy_s(unicodeBuffer, unicodeBufferLength, buffer.data(), buffer.size());

        // set the null character
        unicodeBuffer[buffer.size()] = L'\0';

        // update the actual unicode string
        info->Name.Buffer        = unicodeBuffer;
        info->Name.Length        = static_cast<USHORT>(buffer.size() * 2);
        info->Name.MaximumLength = unicodeBufferLength;

        res = STATUS_SUCCESS;
      }
    }

    auto logger = LOG_CALL()
                      .PARAMWRAP(res)
                      .PARAM(ObjectInformationLength)
                      .addParam("return_length", ReturnLength ? *ReturnLength : -1)
                      .addParam("tracker_path", trackerInfo)
                      .PARAM(ObjectInformationClass)
                      .PARAM(redir.redirected)
                      .PARAM(redir.path);

    if (res == STATUS_SUCCESS) {
      logger.addParam("name_info", info->Name);
    } else {
      logger.addParam("name_info", "");
    }
  }

  HOOK_END
  return res;
}

DLLEXPORT NTSTATUS WINAPI usvfs::hook_NtQueryInformationFile(
    HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation,
    ULONG Length, FILE_INFORMATION_CLASS FileInformationClass)
{
  NTSTATUS res = STATUS_SUCCESS;

  HOOK_START_GROUP(MutExHookGroup::FILE_ATTRIBUTES)
  if (!callContext.active()) {
    return ::NtQueryInformationFile(FileHandle, IoStatusBlock, FileInformation, Length,
                                    FileInformationClass);
  }

  PRE_REALCALL
  res = ::NtQueryInformationFile(FileHandle, IoStatusBlock, FileInformation, Length,
                                 FileInformationClass);
  POST_REALCALL

  // we handle both SUCCESS and BUFFER_OVERFLOW since the fixed name might be
  // smaller than the original one
  //
  // we do not handle STATUS_INFO_LENGTH_MISMATCH because this is only returned if
  // the length is too small to old the structure itself (regardless of the name)
  //
  // TODO: currently, this does not handle STATUS_BUFFER_OVERLOW for ALL information
  // because most of the structures would need to be manually filled, which is very
  // complicated - this should not pose huge issue
  //
  if (((res == STATUS_SUCCESS || res == STATUS_BUFFER_OVERFLOW) &&
       (FileInformationClass == FileNameInformation ||
        FileInformationClass == FileNormalizedNameInformation)) ||
      (res == STATUS_SUCCESS && FileInformationClass == FileAllInformation)) {

    const auto trackerInfo = ntdllHandleTracker.lookup(FileHandle);
    const auto redir       = applyReroute(READ_CONTEXT(), callContext, trackerInfo);

    // TODO: difference between FileNameInformation and FileNormalizedNameInformation

    // maximum length in bytes - the required length is
    // - for ALL, 100 + the number of bytes in the name (not account for null character)
    // - for NAME, 4 + the number of bytes in the name (not accounting for null
    // character)
    //
    // it is close to the sizeof the structure + the number of bytes in the name, except
    // for the alignment that gives us a bit more space
    //
    ULONG prefixStructLength;
    FILE_NAME_INFORMATION* info;
    if (FileInformationClass == FileAllInformation) {
      info = &reinterpret_cast<FILE_ALL_INFORMATION*>(FileInformation)->NameInformation;
      prefixStructLength = sizeof(FILE_ALL_INFORMATION) - 4;
    } else {
      info               = reinterpret_cast<FILE_NAME_INFORMATION*>(FileInformation);
      prefixStructLength = sizeof(FILE_NAME_INFORMATION) - 4;
    }

    if (redir.redirected) {
      auto requiredLength = prefixStructLength + 2 * (trackerInfo.size() - 6);
      if (Length < requiredLength) {
        res = STATUS_BUFFER_OVERFLOW;
      } else {
        // strip the \??\X: prefix (X being the drive name)
        LPCWSTR filenameFixed = static_cast<LPCWSTR>(trackerInfo) + 6;

        // not using SetInfoFilename because the length is not set and we do not need to
        // 0-out the memory here
        info->FileNameLength = static_cast<ULONG>((trackerInfo.size() - 6) * 2);
        wmemcpy(info->FileName, filenameFixed, trackerInfo.size() - 6);
        res = STATUS_SUCCESS;

        // update status block, Information is the number of bytes written, basically
        // the required length
        IoStatusBlock->Status      = STATUS_SUCCESS;
        IoStatusBlock->Information = static_cast<ULONG_PTR>(requiredLength);
      }
    }

    LOG_CALL()
        .PARAMWRAP(res)
        .addParam("tracker_path", trackerInfo)
        .PARAM(FileInformationClass)
        .PARAM(redir.redirected)
        .PARAM(redir.path)
        .addParam("name_info", res == STATUS_SUCCESS
                                   ? std::wstring{info->FileName,
                                                  info->FileNameLength / sizeof(WCHAR)}
                                   : std::wstring{});
  }

  HOOK_END
  return res;
}

unique_ptr_deleter<OBJECT_ATTRIBUTES>
makeObjectAttributes(RedirectionInfo& redirInfo, POBJECT_ATTRIBUTES attributeTemplate)
{
  if (redirInfo.redirected) {
    unique_ptr_deleter<OBJECT_ATTRIBUTES> result(new OBJECT_ATTRIBUTES,
                                                 [](OBJECT_ATTRIBUTES* ptr) {
                                                   delete ptr;
                                                 });
    memcpy(result.get(), attributeTemplate, sizeof(OBJECT_ATTRIBUTES));
    result->RootDirectory = nullptr;
    result->ObjectName    = static_cast<PUNICODE_STRING>(redirInfo.path);
    return result;
  } else {
    // just reuse the template with a dummy deleter
    return unique_ptr_deleter<OBJECT_ATTRIBUTES>(attributeTemplate,
                                                 [](OBJECT_ATTRIBUTES*) {});
  }
}

DLLEXPORT NTSTATUS WINAPI usvfs::hook_NtQueryInformationByName(
    POBJECT_ATTRIBUTES ObjectAttributes, PIO_STATUS_BLOCK IoStatusBlock,
    PVOID FileInformation, ULONG Length, FILE_INFORMATION_CLASS FileInformationClass)
{
  NTSTATUS res = STATUS_SUCCESS;

  HOOK_START_GROUP(MutExHookGroup::FILE_ATTRIBUTES)

  if (!callContext.active()) {
    res = ::NtQueryInformationByName(ObjectAttributes, IoStatusBlock, FileInformation,
                                     Length, FileInformationClass);
    callContext.updateLastError();
    return res;
  }

  RedirectionInfo redir =
      applyReroute(READ_CONTEXT(), callContext, CreateUnicodeString(ObjectAttributes));

  if (redir.redirected) {
    auto newObjectAttributes = makeObjectAttributes(redir, ObjectAttributes);

    PRE_REALCALL
    res = ::NtQueryInformationByName(newObjectAttributes.get(), IoStatusBlock,
                                     FileInformation, Length, FileInformationClass);
    POST_REALCALL

    LOG_CALL()
        .PARAMWRAP(res)
        .addParam("input_path", *ObjectAttributes->ObjectName)
        .addParam("reroute_path", redir.path);
  } else {
    PRE_REALCALL
    res = ::NtQueryInformationByName(ObjectAttributes, IoStatusBlock, FileInformation,
                                     Length, FileInformationClass);
    POST_REALCALL
  }

  HOOK_END
  return res;
}

NTSTATUS ntdll_mess_NtOpenFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
                               POBJECT_ATTRIBUTES ObjectAttributes,
                               PIO_STATUS_BLOCK IoStatusBlock, ULONG ShareAccess,
                               ULONG OpenOptions)
{
  using namespace usvfs;

  PreserveGetLastError ntFunctionsDoNotChangeGetLastError;

  NTSTATUS res = STATUS_NO_SUCH_FILE;

  HOOK_START_GROUP(MutExHookGroup::OPEN_FILE)
  // Why is the usual if (!callContext.active()... check missing?

  UnicodeString fullName = CreateUnicodeString(ObjectAttributes);

  if (isDeviceFile(static_cast<LPCWSTR>(fullName))) {
    return ::NtOpenFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock,
                        ShareAccess, OpenOptions);
  }

  UnicodeString Path = ntdllHandleTracker.lookup(ObjectAttributes->RootDirectory);

  std::wstring checkpath =
      ush::string_cast<std::wstring>(static_cast<LPCWSTR>(Path), ush::CodePage::UTF8);

  if ((fullName.size() == 0) ||
      (GetFileSize(ObjectAttributes->RootDirectory, nullptr) != INVALID_FILE_SIZE)) {
    //	//relative paths that we don't have permission over will fail here due that we
    // can't get the filesize of the root directory
    //	//We should try again to see if it is a directory using another method
    if ((fullName.size() == 0) ||
        (GetFileAttributesW((LPCWSTR)checkpath.c_str()) == INVALID_FILE_ATTRIBUTES)) {
      return ::NtOpenFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock,
                          ShareAccess, OpenOptions);
    }
  }

  try {
    RedirectionInfo redir = applyReroute(READ_CONTEXT(), callContext, fullName);
    unique_ptr_deleter<OBJECT_ATTRIBUTES> adjustedAttributes =
        makeObjectAttributes(redir, ObjectAttributes);

    PRE_REALCALL
    res = ::NtOpenFile(FileHandle, DesiredAccess, adjustedAttributes.get(),
                       IoStatusBlock, ShareAccess, OpenOptions);
    POST_REALCALL

    if (redir.redirected) {
      LOG_CALL()
          .addParam("source", ObjectAttributes)
          .addParam("rerouted", adjustedAttributes.get())
          .PARAM(*FileHandle)
          .PARAM(OpenOptions)
          .PARAMWRAP(res);
    }
  } catch (const std::exception&) {
    PRE_REALCALL
    res = ::NtOpenFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock,
                       ShareAccess, OpenOptions);
    POST_REALCALL
  }
  HOOK_END

  return res;
}

NTSTATUS WINAPI usvfs::hook_NtOpenFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
                                       POBJECT_ATTRIBUTES ObjectAttributes,
                                       PIO_STATUS_BLOCK IoStatusBlock,
                                       ULONG ShareAccess, ULONG OpenOptions)
{
  if (isInternalDirectoryIo()) {
    return ::NtOpenFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock,
                        ShareAccess, OpenOptions);
  }
  NTSTATUS res = ntdll_mess_NtOpenFile(FileHandle, DesiredAccess, ObjectAttributes,
                                       IoStatusBlock, ShareAccess, OpenOptions);
  if (res >= 0 && FileHandle) {
    eraseDirectoryScan(*FileHandle);
    destroyErasedDirectoryScans();
  }
  if (res >= 0 && ObjectAttributes && FileHandle &&
      GetFileType(*FileHandle) == FILE_TYPE_DISK)
    ntdllHandleTracker.insert(*FileHandle, CreateUnicodeString(ObjectAttributes));
  return res;
}

NTSTATUS ntdll_mess_NtCreateFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
                                 POBJECT_ATTRIBUTES ObjectAttributes,
                                 PIO_STATUS_BLOCK IoStatusBlock,
                                 PLARGE_INTEGER AllocationSize, ULONG FileAttributes,
                                 ULONG ShareAccess, ULONG CreateDisposition,
                                 ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength)
{
  using namespace usvfs;

  NTSTATUS res = STATUS_NO_SUCH_FILE;

  PreserveGetLastError ntFunctionsDoNotChangeGetLastError;

  HOOK_START_GROUP(MutExHookGroup::OPEN_FILE)
  if (!callContext.active()) {
    return ::NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock,
                          AllocationSize, FileAttributes, ShareAccess,
                          CreateDisposition, CreateOptions, EaBuffer, EaLength);
  }

  UnicodeString inPath = CreateUnicodeString(ObjectAttributes);
  LPCWSTR inPathW      = static_cast<LPCWSTR>(inPath);

  if (inPath.size() == 0) {
    spdlog::get("hooks")->info(
        "failed to set from handle: {0}",
        ush::string_cast<std::string>(ObjectAttributes->ObjectName->Buffer));
    return ::NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock,
                          AllocationSize, FileAttributes, ShareAccess,
                          CreateDisposition, CreateOptions, EaBuffer, EaLength);
  }

  if (isDeviceFile(inPathW)) {
    return ::NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock,
                          AllocationSize, FileAttributes, ShareAccess,
                          CreateDisposition, CreateOptions, EaBuffer, EaLength);
  }

  DWORD convertedDisposition = OPEN_EXISTING;
  switch (CreateDisposition) {
  case FILE_SUPERSEDE:
    convertedDisposition = CREATE_ALWAYS;
    break;
  case FILE_OPEN:
    convertedDisposition = OPEN_EXISTING;
    break;
  case FILE_CREATE:
    convertedDisposition = CREATE_NEW;
    break;
  case FILE_OPEN_IF:
    convertedDisposition = OPEN_ALWAYS;
    break;
  case FILE_OVERWRITE:
    convertedDisposition = TRUNCATE_EXISTING;
    break;
  case FILE_OVERWRITE_IF:
    convertedDisposition = CREATE_ALWAYS;
    break;
  default:
    spdlog::get("hooks")->error("invalid disposition: {0}", CreateDisposition);
    break;
  }

  DWORD convertedAccess = 0;
  if ((DesiredAccess & FILE_GENERIC_READ) == FILE_GENERIC_READ)
    convertedAccess |= GENERIC_READ;
  if ((DesiredAccess & FILE_GENERIC_WRITE) == FILE_GENERIC_WRITE)
    convertedAccess |= GENERIC_WRITE;
  if ((DesiredAccess & FILE_GENERIC_EXECUTE) == FILE_GENERIC_EXECUTE)
    convertedAccess |= GENERIC_EXECUTE;
  if ((DesiredAccess & FILE_ALL_ACCESS) == FILE_ALL_ACCESS)
    convertedAccess |= GENERIC_ALL;

  ULONG originalDisposition = CreateDisposition;
  CreateRerouter rerouter;
  if (rerouter.rerouteCreate(
          READ_CONTEXT(), callContext, inPathW, convertedDisposition, convertedAccess,
          (LPSECURITY_ATTRIBUTES)ObjectAttributes->SecurityDescriptor)) {
    switch (convertedDisposition) {
    case CREATE_NEW:
      CreateDisposition = FILE_CREATE;
      break;
    case CREATE_ALWAYS:
      if (CreateDisposition != FILE_SUPERSEDE)
        CreateDisposition = FILE_OVERWRITE_IF;
      break;
    case OPEN_EXISTING:
      CreateDisposition = FILE_OPEN;
      break;
    case OPEN_ALWAYS:
      CreateDisposition = FILE_OPEN_IF;
      break;
    case TRUNCATE_EXISTING:
      CreateDisposition = FILE_OVERWRITE;
      break;
    }

    RedirectionInfo redir = applyReroute(rerouter);

    unique_ptr_deleter<OBJECT_ATTRIBUTES> adjustedAttributes =
        makeObjectAttributes(redir, ObjectAttributes);

    PRE_REALCALL
    res = ::NtCreateFile(FileHandle, DesiredAccess, adjustedAttributes.get(),
                         IoStatusBlock, AllocationSize, FileAttributes, ShareAccess,
                         CreateDisposition, CreateOptions, EaBuffer, EaLength);
    POST_REALCALL
    rerouter.updateResult(callContext, res == STATUS_SUCCESS);

    if (res == STATUS_SUCCESS && rerouter.newReroute()) {
      rerouter.insertMapping(WRITE_CONTEXT());
    }

    if (rerouter.wasRerouted() || rerouter.changedError() ||
        originalDisposition != CreateDisposition) {
      LOG_CALL()
          .PARAM(inPathW)
          .PARAM(rerouter.fileName())
          .PARAMHEX(DesiredAccess)
          .PARAMHEX(originalDisposition)
          .PARAMHEX(CreateDisposition)
          .PARAMHEX(FileAttributes)
          .PARAMHEX(res)
          .PARAMHEX(*FileHandle)
          .PARAM(rerouter.originalError())
          .PARAM(rerouter.error());
    }
  } else {
    // make the original call to set up the proper errors and return statuses
    PRE_REALCALL
    res = ::NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock,
                         AllocationSize, FileAttributes, ShareAccess, CreateDisposition,
                         CreateOptions, EaBuffer, EaLength);
    POST_REALCALL
  }

  HOOK_END

  return res;
}

NTSTATUS WINAPI usvfs::hook_NtCreateFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
                                         POBJECT_ATTRIBUTES ObjectAttributes,
                                         PIO_STATUS_BLOCK IoStatusBlock,
                                         PLARGE_INTEGER AllocationSize,
                                         ULONG FileAttributes, ULONG ShareAccess,
                                         ULONG CreateDisposition, ULONG CreateOptions,
                                         PVOID EaBuffer, ULONG EaLength)
{
  NTSTATUS res = ntdll_mess_NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes,
                                         IoStatusBlock, AllocationSize, FileAttributes,
                                         ShareAccess, CreateDisposition, CreateOptions,
                                         EaBuffer, EaLength);
  if (res >= 0 && FileHandle) {
    eraseDirectoryScan(*FileHandle);
    destroyErasedDirectoryScans();
  }
  if (res >= 0 && ObjectAttributes && FileHandle &&
      GetFileType(*FileHandle) == FILE_TYPE_DISK)
    ntdllHandleTracker.insert(*FileHandle, CreateUnicodeString(ObjectAttributes));
  return res;
}

NTSTATUS WINAPI usvfs::hook_NtClose(HANDLE Handle)
{
  // Runs wherever NtClose is called: under the lock of the process heap, inside ntdll,
  // or while a library installing hooks holds every other thread suspended. So it takes
  // no lock, allocates nothing and logs nothing, and calls nothing that could wait for
  // another thread; erasing from the registries is wait-free (see HandleRegistry). Not
  // in a hook call context either, as making one allocates on the first hook a thread
  // calls.
  eraseDirectoryScan(Handle);
  ntdllHandleTracker.erase(Handle);
  return ::NtClose(Handle);
}

NTSTATUS WINAPI usvfs::hook_NtQueryAttributesFile(
    POBJECT_ATTRIBUTES ObjectAttributes, PFILE_BASIC_INFORMATION FileInformation)
{
  PreserveGetLastError ntFunctionsDoNotChangeGetLastError;

  NTSTATUS res = STATUS_SUCCESS;

  HOOK_START_GROUP(MutExHookGroup::FILE_ATTRIBUTES)
  // Why is the usual if (!callContext.active()... check missing?

  UnicodeString inPath = CreateUnicodeString(ObjectAttributes);

  RedirectionInfo redir = applyReroute(READ_CONTEXT(), callContext, inPath);
  unique_ptr_deleter<OBJECT_ATTRIBUTES> adjustedAttributes =
      makeObjectAttributes(redir, ObjectAttributes);

  PRE_REALCALL
  res = ::NtQueryAttributesFile(adjustedAttributes.get(), FileInformation);
  POST_REALCALL

  if (redir.redirected) {
    LOG_CALL()
        .addParam("source", ObjectAttributes)
        .addParam("rerouted", adjustedAttributes.get())
        .PARAMWRAP(res);
  }

  HOOK_END

  return res;
}

NTSTATUS WINAPI usvfs::hook_NtQueryFullAttributesFile(
    POBJECT_ATTRIBUTES ObjectAttributes, PFILE_NETWORK_OPEN_INFORMATION FileInformation)
{
  PreserveGetLastError ntFunctionsDoNotChangeGetLastError;

  NTSTATUS res = STATUS_SUCCESS;

  HOOK_START_GROUP(MutExHookGroup::FILE_ATTRIBUTES)

  if (!callContext.active()) {
    return ::NtQueryFullAttributesFile(ObjectAttributes, FileInformation);
  }

  UnicodeString inPath;
  try {
    inPath = CreateUnicodeString(ObjectAttributes);
  } catch (const std::exception&) {
    return ::NtQueryFullAttributesFile(ObjectAttributes, FileInformation);
  }

  RedirectionInfo redir = applyReroute(READ_CONTEXT(), callContext, inPath);
  unique_ptr_deleter<OBJECT_ATTRIBUTES> adjustedAttributes =
      makeObjectAttributes(redir, ObjectAttributes);

  PRE_REALCALL
  res = ::NtQueryFullAttributesFile(adjustedAttributes.get(), FileInformation);
  POST_REALCALL

  if (redir.redirected) {
    LOG_CALL()
        .addParam("source", ObjectAttributes)
        .addParam("rerouted", adjustedAttributes.get())
        .PARAMWRAP(res);
  }

  HOOK_END

  return res;
}

NTSTATUS WINAPI usvfs::hook_NtTerminateProcess(HANDLE ProcessHandle,
                                               NTSTATUS ExitStatus)
{
  // this hook is normally called when terminating another process, in which
  // case there's nothing to do
  //
  // if the current process exits normally, the ExitProcess() hook is called
  // and disconnects from the vfs; if the current process crashes, this hook
  // is not called, the process just dies
  //
  // but a process can also terminate itself, bypassing ExitProcess(), and
  // ending up here, in which case the vfs should be disconnected
  //
  // NtTerminateProcess() can be called two different ways to terminate the
  // current process:
  //   - with a valid handle for the current process, or
  //   - with -1, because that's what GetCurrentProcess() returns
  //
  // it's unclear what a NULL handle represents, the behaviour is not
  // documented anywhere, but looking at ReactOS, it's also interpreted as the
  // current process

  NTSTATUS res = STATUS_SUCCESS;

  HOOK_START

  const bool isCurrentProcess = ProcessHandle == (HANDLE)-1 || ProcessHandle == 0 ||
                                GetProcessId(ProcessHandle) == GetCurrentProcessId();

  if (isCurrentProcess) {
    usvfsDisconnectVFS();
  }

  res = ::NtTerminateProcess(ProcessHandle, ExitStatus);

  HOOK_END

  return res;
}
