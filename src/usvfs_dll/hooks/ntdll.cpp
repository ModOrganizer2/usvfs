#include "ntdll.h"
#include "sharedids.h"
#include <loghelpers.h>
#include "../hookcontext.h"
#include "../hookcallcontext.h"
#include "../maptracker.h"
#include "../stringcast_boost.h"
#include <usvfs.h>
#pragma warning(push, 3)
#include <boost/scoped_array.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/locale.hpp>
//#include <boost/thread/mutex.hpp>
#pragma warning(pop)
#include <string>
#include <deque>
#include <queue>
#include <vector>
#include <set>
#include <map>
#include <cstdint>
#include <cwctype>
#include <codecvt>
#include <locale>
#include <windows_error.h>
#include <stringutils.h>
#include <stringcast.h>
#include <scopeguard.h>
#include <addrtools.h>
#include <unicodestring.h>
#include <windows.h>
#include <fileapi.h>
#include <mutex>
#include <shared_mutex>

#pragma warning(disable : 4996)

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
using unique_ptr_deleter = std::unique_ptr<T, void (*)(T *)>;

class RedirectionInfo {
public:
  UnicodeString path;
  bool redirected;

  RedirectionInfo() {}
  RedirectionInfo(UnicodeString path, bool redirected)
    : path(path)
    , redirected(redirected)
  {}
};

class HandleTracker {
public:
  using handle_type = HANDLE;
  using info_type = UnicodeString;

  HandleTracker() { insert_current_directory(); }

  info_type lookup(handle_type handle) const {
    if (valid_handle(handle))
    {
      std::shared_lock<std::shared_mutex> lock(m_mutex);
      auto find = m_map.find(handle);
      if (find != m_map.end())
        return find->second;
    }
    return info_type();
  }

  void insert(handle_type handle, const info_type& info) {
    if (!valid_handle(handle))
      return;
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_map[handle] = info;
  }

  void erase(handle_type handle)
  {
    if (!valid_handle(handle))
      return;
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_map.erase(handle);
  }

private:
  static bool valid_handle(handle_type handle) { return handle && handle != INVALID_HANDLE_VALUE; }

  void insert_current_directory()
  {
    ntdll_declarations_init();

    UNICODE_STRING p{ 0 };
    RTL_RELATIVE_NAME r{ 0 };
    NTSTATUS status = RtlDosPathNameToRelativeNtPathName_U_WithStatus(L"x", &p, nullptr, &r);
    if (status >= 0) {
      if (r.ContainingDirectory && p.Buffer)
      {
        size_t len = p.Length / sizeof(WCHAR);
        size_t trim = strlen("\\x") + (p.Buffer[len - 1] ? 0 : 1);
        if (len > trim)
          insert(r.ContainingDirectory, info_type(p.Buffer, len - trim));
      }
      RtlReleaseRelativeName(&r);
      if (p.Buffer)
        HeapFree(GetProcessHeap(), 0, p.Buffer);
    }
  }

  mutable std::shared_mutex m_mutex;
  std::unordered_map<handle_type, info_type> m_map;
};

HandleTracker ntdllHandleTracker;


UnicodeString CreateUnicodeString(const OBJECT_ATTRIBUTES *objectAttributes)
{
  UnicodeString result = ntdllHandleTracker.lookup(objectAttributes->RootDirectory);
  if (objectAttributes->ObjectName != nullptr) {
    result.appendPath(objectAttributes->ObjectName);
  }
  return result;
}

std::ostream &operator<<(std::ostream &os, const _UNICODE_STRING &str)
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
  } catch (const std::exception &e) {
    os << e.what();
  }

  return os;
}

std::ostream &operator<<(std::ostream &os, POBJECT_ATTRIBUTES attr)
{
  return operator<<(os, *attr->ObjectName);
}

RedirectionInfo
applyReroute(const usvfs::HookContext::ConstPtr &context,
             const usvfs::HookCallContext &callContext,
             const UnicodeString &inPath)
{
  RedirectionInfo result;
  result.path  = inPath;
  result.redirected = false;

  if (callContext.active()) {
    // see if the file exists in the redirection tree
    std::string lookupPath = ush::string_cast<std::string>(
        static_cast<LPCWSTR>(result.path) + 4, ush::CodePage::UTF8);
    auto node = context->redirectionTable()->findNode(lookupPath.c_str());
    // if so, replace the file name with the path to the mapped file
    if ((node.get() != nullptr) && (!node->data().linkTarget.empty() || node->isDirectory())) {
      std::wstring reroutePath;

      if (node->data().linkTarget.length() > 0)
      {
        reroutePath = ush::string_cast<std::wstring>(
          node->data().linkTarget.c_str(), ush::CodePage::UTF8);
      }
      else
      {
        reroutePath = ush::string_cast<std::wstring>(
          node->path().c_str(),
          ush::CodePage::UTF8);
      }
      if ((*reroutePath.rbegin() == L'\\') && (*lookupPath.rbegin() != '\\')) {
        reroutePath.resize(reroutePath.size() - 1);
      }
      std::replace(reroutePath.begin(), reroutePath.end(), L'/', L'\\');
      if (reroutePath[1] == L'\\')
        reroutePath[1] = L'?';
      result.path = LR"(\??\)" + reroutePath;
      result.redirected = true;
    }
  }
  return result;
}

RedirectionInfo
applyReroute(const usvfs::CreateRerouter &rerouter)
{
  RedirectionInfo result;
  result.path = rerouter.fileName();
  result.redirected = rerouter.wasRerouted();

  std::wstring reroutePath(static_cast<LPCWSTR>(result.path));
  std::replace(reroutePath.begin(), reroutePath.end(), L'/', L'\\');
  if (reroutePath[1] == L'\\')
    reroutePath[1] = L'?';
  if (!((reroutePath[0] == L'\\') &&
        (reroutePath[1] == L'?') &&
        (reroutePath[2] == L'?') &&
        (reroutePath[3] == L'\\'))) {
    result.path = LR"(\??\)" + reroutePath;
  }

  return result;
}

struct FindCreateTarget {
  usvfs::RedirectionTree::NodePtrT target;
  void operator()(usvfs::RedirectionTree::NodePtrT node)
  {
    if (node->hasFlag(usvfs::shared::FLAG_CREATETARGET)) {
      target = node;
    }
  }
};

std::pair<UnicodeString, UnicodeString>
findCreateTarget(const usvfs::HookContext::ConstPtr &context,
                 const UnicodeString &inPath)
{
  std::pair<UnicodeString, UnicodeString> result;
  result.first  = inPath;
  result.second = UnicodeString();

  std::string lookupPath = ush::string_cast<std::string>(
      static_cast<LPCWSTR>(result.first) + 4, ush::CodePage::UTF8);
  FindCreateTarget visitor;
  usvfs::RedirectionTree::VisitorFunction visitorWrapper =
      [&](const usvfs::RedirectionTree::NodePtrT &node) { visitor(node); };
  context->redirectionTable()->visitPath(lookupPath, visitorWrapper);
  if (visitor.target.get() != nullptr) {
    bfs::path relativePath
        = ush::make_relative(visitor.target->path(), bfs::path(lookupPath));

    bfs::path target(visitor.target->data().linkTarget.c_str());
    target /= relativePath;

    result.second = UnicodeString(target.wstring().c_str());
    winapi::ex::wide::createPath(target.parent_path());
  }
  return result;
}

RedirectionInfo
applyReroute(const usvfs::HookContext::ConstPtr &context,
             const usvfs::HookCallContext &callContext,
             POBJECT_ATTRIBUTES inAttributes)
{
  return applyReroute(context, callContext, CreateUnicodeString(inAttributes));
}

ULONG StructMinSize(FILE_INFORMATION_CLASS infoClass)
{
  switch (infoClass) {
    case FileBothDirectoryInformation:
      return sizeof(FILE_BOTH_DIR_INFORMATION);
    case FileDirectoryInformation:
      return sizeof(FILE_DIRECTORY_INFORMATION);
    case FileFullDirectoryInformation:
      return sizeof(FILE_FULL_DIR_INFORMATION);
    case FileIdBothDirectoryInformation:
      return sizeof(FILE_ID_BOTH_DIR_INFORMATION);
    case FileIdFullDirectoryInformation:
      return sizeof(FILE_ID_FULL_DIR_INFORMATION);
    case FileNamesInformation:
      return sizeof(FILE_NAMES_INFORMATION);
    case FileObjectIdInformation:
      return sizeof(FILE_OBJECTID_INFORMATION);
    case FileReparsePointInformation:
      return sizeof(FILE_REPARSE_POINT_INFORMATION);
    default:
      return 0;
  }
}

void GetInfoData(LPCVOID address, FILE_INFORMATION_CLASS infoClass,
                 ULONG &offset, std::wstring &fileName)
{
  switch (infoClass) {
    case FileBothDirectoryInformation: {
      const FILE_BOTH_DIR_INFORMATION *info
          = reinterpret_cast<const FILE_BOTH_DIR_INFORMATION *>(address);
      offset = info->NextEntryOffset;
      fileName
          = std::wstring(info->FileName, info->FileNameLength / sizeof(WCHAR));
    } break;
    case FileDirectoryInformation: {
      const FILE_DIRECTORY_INFORMATION *info
          = reinterpret_cast<const FILE_DIRECTORY_INFORMATION *>(address);
      offset = info->NextEntryOffset;
      fileName
          = std::wstring(info->FileName, info->FileNameLength / sizeof(WCHAR));
    } break;
    case FileNamesInformation: {
      const FILE_NAMES_INFORMATION *info
          = reinterpret_cast<const FILE_NAMES_INFORMATION *>(address);
      offset = info->NextEntryOffset;
      fileName
          = std::wstring(info->FileName, info->FileNameLength / sizeof(WCHAR));
    } break;
    case FileIdFullDirectoryInformation: {
      const FILE_ID_FULL_DIR_INFORMATION *info
          = reinterpret_cast<const FILE_ID_FULL_DIR_INFORMATION *>(address);
      offset = info->NextEntryOffset;
      fileName
          = std::wstring(info->FileName, info->FileNameLength / sizeof(WCHAR));
    } break;
    case FileFullDirectoryInformation: {
      const FILE_FULL_DIR_INFORMATION *info
          = reinterpret_cast<const FILE_FULL_DIR_INFORMATION *>(address);
      offset = info->NextEntryOffset;
      fileName
          = std::wstring(info->FileName, info->FileNameLength / sizeof(WCHAR));
    } break;
    case FileIdBothDirectoryInformation: {
      const FILE_ID_BOTH_DIR_INFORMATION *info
          = reinterpret_cast<const FILE_ID_BOTH_DIR_INFORMATION *>(address);
      offset = info->NextEntryOffset;
      fileName
          = std::wstring(info->FileName, info->FileNameLength / sizeof(WCHAR));
    } break;
    case FileObjectIdInformation: {
      offset = sizeof(FILE_OBJECTID_INFORMATION);
    } break;
    case FileReparsePointInformation: {
      offset = sizeof(FILE_REPARSE_POINT_INFORMATION);
    } break;
    default: {
      offset = ULONG_MAX;
    } break;
  }
}

template <typename T>
void SetInfoFilenameImpl(T *info, const std::wstring &fileName)
{
  // not sure if the filename is supposed to be zero terminated but I did get
  // invalid
  // filenames when the buffer wasn't zeroed
  memset(info->FileName, L'\0', info->FileNameLength);

  info->FileNameLength = static_cast<ULONG>(fileName.length() * sizeof(WCHAR));
  memcpy(info->FileName, fileName.c_str(), info->FileNameLength + 1);
}

// like wcsrchr except that the position to start searching can be specified and
// the string doesn't
// need to be 0-terminated
const wchar_t *wcsrevsearch(const wchar_t *cur, const wchar_t *start,
                            wchar_t ch)
{
  for (; cur > start; --cur) {
    if (*cur == ch) {
      return cur;
    }
  }
  return nullptr;
}

template <typename T>
void SetInfoFilenameImplSN(T *info, const std::wstring &fileName)
{
  memset(info->FileName, L'\0', info->FileNameLength);
  info->FileNameLength = static_cast<ULONG>(fileName.length() * sizeof(WCHAR));
  memcpy(info->FileName, fileName.c_str(),
         info->FileNameLength); // doesn't need to be 0-terminated

  if (info->ShortNameLength > 0) {
    info->ShortNameLength = static_cast<CCHAR>(
        GetShortPathNameW(fileName.c_str(), info->ShortName, 8));
  }
}

void SetInfoFilename(LPVOID address, FILE_INFORMATION_CLASS infoClass,
                     const std::wstring &fileName)
{
  switch (infoClass) {
    case FileBothDirectoryInformation: {
      SetInfoFilenameImplSN(
          reinterpret_cast<FILE_BOTH_DIR_INFORMATION *>(address), fileName);
    } break;
    case FileDirectoryInformation: {
      SetInfoFilenameImpl(
          reinterpret_cast<FILE_DIRECTORY_INFORMATION *>(address), fileName);
    } break;
    case FileNamesInformation: {
      SetInfoFilenameImpl(reinterpret_cast<FILE_NAMES_INFORMATION *>(address),
                          fileName);
    } break;
    case FileIdFullDirectoryInformation: {
      SetInfoFilenameImpl(
          reinterpret_cast<FILE_ID_FULL_DIR_INFORMATION *>(address), fileName);
    } break;
    case FileFullDirectoryInformation: {
      SetInfoFilenameImpl(
          reinterpret_cast<FILE_FULL_DIR_INFORMATION *>(address), fileName);
    } break;
    case FileIdBothDirectoryInformation: {
      SetInfoFilenameImplSN(
          reinterpret_cast<FILE_ID_BOTH_DIR_INFORMATION *>(address),
          fileName);
    } break;
    default: {
      // NOP
    } break;
  }
}

void SetInfoOffset(LPVOID address, FILE_INFORMATION_CLASS infoClass,
                   ULONG offset)
{
  switch (infoClass) {
    case FileBothDirectoryInformation: {
      reinterpret_cast<FILE_BOTH_DIR_INFORMATION *>(address)->NextEntryOffset
          = offset;
    } break;
    case FileDirectoryInformation: {
      reinterpret_cast<FILE_DIRECTORY_INFORMATION *>(address)->NextEntryOffset
          = offset;
    } break;
    case FileNamesInformation: {
      reinterpret_cast<FILE_NAMES_INFORMATION *>(address)->NextEntryOffset
          = offset;
    } break;
    case FileIdFullDirectoryInformation: {
      reinterpret_cast<FILE_ID_FULL_DIR_INFORMATION *>(address)->NextEntryOffset
          = offset;
    } break;
    case FileFullDirectoryInformation: {
      reinterpret_cast<FILE_FULL_DIR_INFORMATION *>(address)->NextEntryOffset
          = offset;
    } break;
    case FileIdBothDirectoryInformation: {
      reinterpret_cast<FILE_ID_BOTH_DIR_INFORMATION *>(address)->NextEntryOffset
          = offset;
    } break;
    default: {
      // NOP
    } break;
  }
}

int NextDividableBy(int number, int divider)
{
  return static_cast<int>(
      ceilf(static_cast<float>(number) / static_cast<float>(divider))
      * divider);
}

std::string toHex(PVOID buffer, ULONG size)
{
  fmt::MemoryWriter stream;
  unsigned char *bufferChar = reinterpret_cast<unsigned char *>(buffer);
  for (size_t i = 0; i < size; ++i) {
    stream << fmt::pad(fmt::hex(bufferChar[i]), 2, '0');
    if (i < size - 1) {
      stream << ((i % 16 == 15) ? "\n" : " ");
    }
  }

  return stream.str();
}

NTSTATUS addNtSearchData(HANDLE hdl, PUNICODE_STRING FileName,
                         const std::wstring &fakeName,
                         FILE_INFORMATION_CLASS FileInformationClass,
                         PVOID &buffer, ULONG &bufferSize,
                         std::set<std::wstring> &foundFiles, HANDLE event,
                         PIO_APC_ROUTINE apcRoutine, PVOID apcContext,
                         BOOLEAN returnSingleEntry)
{
  NTSTATUS res = STATUS_NO_SUCH_FILE;
  if (hdl != INVALID_HANDLE_VALUE) {
    PVOID lastValidRecord = nullptr;
    PVOID bufferInit = buffer;
    IO_STATUS_BLOCK status;
    res = NtQueryDirectoryFile(hdl, event, apcRoutine, apcContext, &status,
                               buffer, bufferSize, FileInformationClass,
                               returnSingleEntry, FileName, FALSE);

    if ((res != STATUS_SUCCESS) || (status.Information <= 0)) {
      bufferSize = 0UL;
    } else {
      ULONG totalOffset   = 0;
      PVOID lastSkipPos   = nullptr;

      while (totalOffset < status.Information) {
        ULONG offset;
        std::wstring fileName;
        GetInfoData(buffer, FileInformationClass, offset, fileName);
        // in case this is a single-file search result and the specified
        // filename differs from the file name found, replace it in the
        // information structure
        if ((totalOffset == 0) && (offset == 0) && (fakeName.length() > 0)) {
          // if the fake name is larger than what is in the buffer and there is
          // not enough room, that's a buffer overflow
          if ((fakeName.length() > fileName.length())
              && ((fakeName.length() - fileName.length())
                  > (bufferSize - status.Information))) {
            res = STATUS_BUFFER_OVERFLOW;
            break;
          }
          // WARNING for the case where the fake name is longer this needs to
          // move back all further results and update the offset first
          SetInfoFilename(buffer, FileInformationClass, fakeName);
          fileName = fakeName;
        }
        bool add = true;
        if (fileName.length() > 0) {
          auto insertRes = foundFiles.insert(ush::to_upper(fileName));
          add      = insertRes.second; // add only if we didn't find this file before
        }
        if (!add) {
          if (lastSkipPos == nullptr) {
            lastSkipPos = buffer;
          }
        } else {
          if (lastSkipPos != nullptr) {
            memmove(lastSkipPos, buffer, status.Information - totalOffset);
            ULONG delta = static_cast<ULONG>(ush::AddrDiff(buffer, lastSkipPos));
            totalOffset -= delta;

            buffer = lastSkipPos;
            lastSkipPos = nullptr;
          }
          lastValidRecord = buffer;
        }

        if (offset == 0) {
          offset = static_cast<ULONG>(status.Information) - totalOffset;
        }
        buffer = ush::AddrAdd(buffer, offset);
        totalOffset += offset;
      }

      if (lastSkipPos != nullptr) {
        buffer = lastSkipPos;
        bufferSize = static_cast<ULONG>(ush::AddrDiff(buffer, bufferInit));
        // null out the unused rest if there is some
        memset(lastSkipPos, 0, status.Information - bufferSize);
      } else {
        bufferSize = static_cast<ULONG>(ush::AddrDiff(buffer, bufferInit));
      }
    }
    if (lastValidRecord != nullptr) {
      SetInfoOffset(lastValidRecord, FileInformationClass, 0);
    }
  }
  return res;
}

DATA_ID(SearchInfo);

struct Searches {
  struct Info {
    struct VirtualMatch {
      // full path to where the file/directory actually is
      std::wstring realPath;
      // virtual filename (only filename since it has to be within the searched
      // directory)
      // this is left empty when a folder with all its content is mapped to the
      // search directory
      std::wstring virtualName;
    };

    Info() : currentSearchHandle(INVALID_HANDLE_VALUE)
    {
    }
    std::set<std::wstring> foundFiles;
    HANDLE currentSearchHandle;
    std::queue<VirtualMatch> virtualMatches;
    UnicodeString searchPattern;
    bool regularComplete{false};
  };

  Searches() = default;

  // must provide a special copy constructor because boost::mutex is
  // non-copyable
  Searches(const Searches &reference) : info(reference.info)
  {
  }

  Searches &operator=(const Searches &) = delete;

  std::recursive_mutex queryMutex;

  std::map<HANDLE, Info> info;
};

void gatherVirtualEntries(const UnicodeString &dirName,
                          const usvfs::RedirectionTreeContainer &redir,
                          PUNICODE_STRING FileName, Searches::Info &info)
{
  LPCWSTR dirNameW = static_cast<LPCWSTR>(dirName);
  // fix directory name. I'd love to know why microsoft sometimes uses "\??\" vs
  // "\\?\"
  if ((wcsncmp(dirNameW, LR"(\\?\)", 4) == 0)
      || (wcsncmp(dirNameW, LR"(\??\)", 4) == 0)) {
    dirNameW += 4;
  }
  auto node = redir->findNode(boost::filesystem::path(dirNameW));
  if (node.get() != nullptr) {
    std::string searchPattern = FileName != nullptr
                                    ? ush::string_cast<std::string>(
                                          FileName->Buffer, ush::CodePage::UTF8)
                                    : "*.*";

    boost::replace_all(searchPattern, "\"", ".");

    for (const auto &subNode : node->find(searchPattern)) {
      if (((subNode->data().linkTarget.length() > 0) || subNode->isDirectory())
          && !subNode->hasFlag(usvfs::shared::FLAG_DUMMY)) {
        std::wstring vName = ush::string_cast<std::wstring>(
            subNode->name(), ush::CodePage::UTF8);

        Searches::Info::VirtualMatch m;
        if (subNode->data().linkTarget.length() > 0)
        {
          m = { ush::string_cast<std::wstring>(subNode->data().linkTarget.c_str(),
                                         ush::CodePage::UTF8), vName };
        }
        else
        {
          m = { ush::string_cast<std::wstring>(subNode->path().c_str(),
            ush::CodePage::UTF8), vName };
        }

        info.virtualMatches.push(m);
        info.foundFiles.insert(ush::to_upper(vName));
      }
    }
  }
}

/**
 * @brief insert a virtual entry into the search result
 * @param FileInformation
 * @param FileInformationClass
 * @param info
 * @param realPath path were the actual file resides
 * @param virtualName virtual file name (without path). will often be the same
 *        as the name component of realpath
 * @param ReturnSingleEntry
 * @param dataRead
 * @return true if a virtual result was added, false if the search handle in the
 *         info object yields no more results
 */
bool addVirtualSearchResult(PVOID &FileInformation,
                            FILE_INFORMATION_CLASS FileInformationClass,
                            Searches::Info &info, const std::wstring &realPath,
                            const std::wstring &virtualName,
                            BOOLEAN ReturnSingleEntry, ULONG &dataRead)
{
  // this opens a search in the real location, then copies the information about
  // files we care about (the ones being mapped) to the result we intend to
  // return
  bfs::path fullPath(realPath);
  if (fullPath.filename().wstring() == L".") {
    fullPath = fullPath.parent_path();
  }
  if (info.currentSearchHandle == INVALID_HANDLE_VALUE) {
    std::wstring dirName     = fullPath.parent_path().wstring();
    if (dirName.length() >= MAX_PATH && !ush::startswith(dirName.c_str(), LR"(\\?\)"))
      dirName = LR"(\\?\)" + dirName;
    info.currentSearchHandle = CreateFileW(
        dirName.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  }
  std::wstring fileName = ush::string_cast<std::wstring>(
      fullPath.filename().string(), ush::CodePage::UTF8);
  NTSTATUS subRes = addNtSearchData(
      info.currentSearchHandle,
      (fileName != L".")
          ? static_cast<PUNICODE_STRING>(UnicodeString(fileName.c_str()))
          : nullptr,
      virtualName, FileInformationClass, FileInformation, dataRead,
      info.foundFiles, nullptr, nullptr, nullptr, ReturnSingleEntry);
  if (subRes == STATUS_SUCCESS) {
    return true;
  } else {
    // STATUS_NO_MORE_FILES merely means the search ended, everything else is an
    // error message. Either way, the search here is finished and we should
    // resume in the next mapped directory
    if (subRes != STATUS_NO_MORE_FILES) {
      spdlog::get("hooks")->warn("error reported listing files in {0}: {1:x}",
                                 fullPath.string(),
                                 static_cast<uint32_t>(subRes));
    }
    return false;
  }
}

NTSTATUS WINAPI usvfs::hook_NtQueryDirectoryFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine,
    PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation,
    ULONG Length, FILE_INFORMATION_CLASS FileInformationClass,
    BOOLEAN ReturnSingleEntry, PUNICODE_STRING FileName, BOOLEAN RestartScan)
{
  PreserveGetLastError ntFunctionsDoNotChangeGetLastError;

  // this is quite messy...
  // first, this will gather the virtual locations mapping to the iterated one
  // then we return results from the real location, skipping those that exist
  //   in the virtual locations, as those take precedence
  // finally the virtual results are returned, adding each result to a skip
  //   list, so they don't get added twice
  //
  // if we don't add the regular files first, "." and ".." wouldn't be in the
  //   first search result of wildcard searches which may confuse the caller
  NTSTATUS res = STATUS_NO_MORE_FILES;
  HOOK_START_GROUP(MutExHookGroup::FIND_FILES)
  if (!callContext.active()) {
    return ::NtQueryDirectoryFile(FileHandle, Event, ApcRoutine, ApcContext,
                                  IoStatusBlock, FileInformation, Length,
                                  FileInformationClass, ReturnSingleEntry,
                                  FileName, RestartScan);
  }

//  std::unique_lock<std::recursive_mutex> queryLock;
  std::map<HANDLE, Searches::Info>::iterator infoIter;
  bool firstSearch = false;

  { // scope to limit context lifetime
    HookContext::ConstPtr context = READ_CONTEXT();
    Searches &activeSearches = context->customData<Searches>(SearchInfo);
//    queryLock = std::unique_lock<std::recursive_mutex>(activeSearches.queryMutex);

    if (RestartScan) {
      auto iter = activeSearches.info.find(FileHandle);
      if (iter != activeSearches.info.end()) {
        activeSearches.info.erase(iter);
      }
    }

    // see if we already have a running search
    infoIter = activeSearches.info.find(FileHandle);
    firstSearch = (infoIter == activeSearches.info.end());
  }

  if (firstSearch) {
    HookContext::Ptr context = WRITE_CONTEXT();
    Searches &activeSearches = context->customData<Searches>(SearchInfo);
    // tradeoff time: we store this search status even if no virtual results
    // were found. This causes a little extra cost here and in NtClose every
    // time a non-virtual dir is being searched. However if we don't,
    // whenever NtQueryDirectoryFile is called another time on the same handle,
    // this (expensive) block would be run again.
    infoIter = activeSearches.info.insert(std::make_pair(FileHandle,
                                                         Searches::Info()))
                   .first;
    infoIter->second.searchPattern.appendPath(FileName);

    SearchHandleMap &searchMap
        = context->customData<SearchHandleMap>(SearchHandles);
    SearchHandleMap::iterator iter = searchMap.find(FileHandle);

    UnicodeString searchPath;
    if (iter != searchMap.end()) {
      searchPath = UnicodeString(iter->second.c_str());
      infoIter->second.currentSearchHandle =
          CreateFileW(iter->second.c_str(), GENERIC_READ,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    } else {
      searchPath = ntdllHandleTracker.lookup(FileHandle);
    }
    gatherVirtualEntries(searchPath, context->redirectionTable(), FileName,
                         infoIter->second);
  }

  ULONG dataRead = Length;
  PVOID FileInformationCurrent = FileInformation;

  // add regular search results, skipping those files we have in a virtual
  // location
  bool moreRegular  = !infoIter->second.regularComplete;
  bool dataReturned = false;
  while (moreRegular && !dataReturned) {
    dataRead        = Length;

    HANDLE handle = infoIter->second.currentSearchHandle;
    if (handle == INVALID_HANDLE_VALUE) {
      handle = FileHandle;
    }
    NTSTATUS subRes = addNtSearchData(
        handle, FileName, L"", FileInformationClass, FileInformationCurrent,
        dataRead, infoIter->second.foundFiles, Event, ApcRoutine, ApcContext,
        ReturnSingleEntry);
    moreRegular = subRes == STATUS_SUCCESS;
    if (moreRegular) {
      dataReturned = dataRead != 0;
    } else {
      infoIter->second.regularComplete = true;
      infoIter->second.foundFiles.clear();
      if (infoIter->second.currentSearchHandle != INVALID_HANDLE_VALUE) {
        ::CloseHandle(infoIter->second.currentSearchHandle);
        infoIter->second.currentSearchHandle = INVALID_HANDLE_VALUE;
      }
    }
  }
  if (!moreRegular) {
    // add virtual results
    while (!dataReturned && infoIter->second.virtualMatches.size() > 0) {
      auto match = infoIter->second.virtualMatches.front();
      if (match.realPath.size() != 0) {
        dataRead = Length;
        if (addVirtualSearchResult(FileInformationCurrent, FileInformationClass,
                                   infoIter->second, match.realPath,
                                   match.virtualName, ReturnSingleEntry,
                                   dataRead)) {
          // a positive result here means the call returned data and there may
          // be further objects to be retrieved by repeating the call
          dataReturned = true;
        } else {
          // proceed to next search handle

          // TODO: doesn't append search results from more than one redirection
          // per call. This is bad for performance but otherwise we'd need to
          // re-write the offsets between information objects
          infoIter->second.virtualMatches.pop();
          CloseHandle(infoIter->second.currentSearchHandle);
          infoIter->second.currentSearchHandle = INVALID_HANDLE_VALUE;
        }
      }
    }
  }

  if (!dataReturned) {
    if (firstSearch) {
      res = STATUS_NO_SUCH_FILE;
    } else {
      res = STATUS_NO_MORE_FILES;
    }
  } else {
    res = STATUS_SUCCESS;
  }
  IoStatusBlock->Status      = res;
  IoStatusBlock->Information = dataRead;

  size_t numVirtualFiles = infoIter->second.virtualMatches.size();
  if (true) {
    LOG_CALL()
        .addParam("path", ntdllHandleTracker.lookup(FileHandle))
        .PARAM(FileInformationClass)
        .PARAMWRAP(FileName)
        .PARAM(numVirtualFiles)
        .PARAMWRAP(res);
  }

  HOOK_END
  return res;
}

NTSTATUS WINAPI usvfs::hook_NtQueryDirectoryFileEx(
    HANDLE FileHandle,
    HANDLE Event,
    PIO_APC_ROUTINE ApcRoutine,
    PVOID ApcContext,
    PIO_STATUS_BLOCK IoStatusBlock,
    PVOID FileInformation,
    ULONG Length,
    FILE_INFORMATION_CLASS FileInformationClass,
    ULONG QueryFlags,
    PUNICODE_STRING FileName)
{
  PreserveGetLastError ntFunctionsDoNotChangeGetLastError;
  NTSTATUS res = STATUS_NO_MORE_FILES;

  // this is quite messy...
  // first, this will gather the virtual locations mapping to the iterated one
  // then we return results from the real location, skipping those that exist
  //   in the virtual locations, as those take precedence
  // finally the virtual results are returned, adding each result to a skip
  //   list, so they don't get added twice
  //
  // if we don't add the regular files first, "." and ".." wouldn't be in the
  //   first search result of wildcard searches which may confuse the caller
  HOOK_START_GROUP(MutExHookGroup::FIND_FILES)
  if (!callContext.active()) {
    return ::NtQueryDirectoryFileEx(FileHandle, Event, ApcRoutine, ApcContext,
      IoStatusBlock, FileInformation, Length,
      FileInformationClass, QueryFlags, FileName);
  }

  //  std::unique_lock<std::recursive_mutex> queryLock;
  std::map<HANDLE, Searches::Info>::iterator infoIter;
  bool firstSearch = false;

  { // scope to limit context lifetime
    HookContext::ConstPtr context = READ_CONTEXT();
    Searches &activeSearches = context->customData<Searches>(SearchInfo);
    //    queryLock = std::unique_lock<std::recursive_mutex>(activeSearches.queryMutex);

    if (QueryFlags & SL_RESTART_SCAN) {
      auto iter = activeSearches.info.find(FileHandle);
      if (iter != activeSearches.info.end()) {
        activeSearches.info.erase(iter);
      }
    }

    // see if we already have a running search
    infoIter = activeSearches.info.find(FileHandle);
    firstSearch = (infoIter == activeSearches.info.end());
  }

  if (firstSearch) {
    HookContext::Ptr context = WRITE_CONTEXT();
    Searches &activeSearches = context->customData<Searches>(SearchInfo);
    // tradeoff time: we store this search status even if no virtual results
    // were found. This causes a little extra cost here and in NtClose every
    // time a non-virtual dir is being searched. However if we don't,
    // whenever NtQueryDirectoryFile is called another time on the same handle,
    // this (expensive) block would be run again.
    infoIter = activeSearches.info.insert(std::make_pair(FileHandle,
      Searches::Info()))
      .first;
    infoIter->second.searchPattern.appendPath(FileName);

    SearchHandleMap &searchMap
      = context->customData<SearchHandleMap>(SearchHandles);
    SearchHandleMap::iterator iter = searchMap.find(FileHandle);

    UnicodeString searchPath;
    if (iter != searchMap.end()) {
      searchPath = UnicodeString(iter->second.c_str());
      infoIter->second.currentSearchHandle =
        CreateFileW(iter->second.c_str(), GENERIC_READ,
          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
          OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    }
    else {
      searchPath = ntdllHandleTracker.lookup(FileHandle);
    }
    gatherVirtualEntries(searchPath, context->redirectionTable(), FileName,
      infoIter->second);
  }

  ULONG dataRead = Length;
  PVOID FileInformationCurrent = FileInformation;

  // add regular search results, skipping those files we have in a virtual
  // location
  bool moreRegular = !infoIter->second.regularComplete;
  bool dataReturned = false;
  while (moreRegular && !dataReturned) {
    dataRead = Length;

    HANDLE handle = infoIter->second.currentSearchHandle;
    if (handle == INVALID_HANDLE_VALUE) {
      handle = FileHandle;
    }
    NTSTATUS subRes = addNtSearchData(
      handle, FileName, L"", FileInformationClass, FileInformationCurrent,
      dataRead, infoIter->second.foundFiles, Event, ApcRoutine, ApcContext,
      QueryFlags & SL_RETURN_SINGLE_ENTRY);
    moreRegular = subRes == STATUS_SUCCESS;
    if (moreRegular) {
      dataReturned = dataRead != 0;
    }
    else {
      infoIter->second.regularComplete = true;
      infoIter->second.foundFiles.clear();
      if (infoIter->second.currentSearchHandle != INVALID_HANDLE_VALUE) {
        ::CloseHandle(infoIter->second.currentSearchHandle);
        infoIter->second.currentSearchHandle = INVALID_HANDLE_VALUE;
      }
    }
  }
  if (!moreRegular) {
    // add virtual results
    while (!dataReturned && infoIter->second.virtualMatches.size() > 0) {
      auto match = infoIter->second.virtualMatches.front();
      if (match.realPath.size() != 0) {
        dataRead = Length;
        if (addVirtualSearchResult(FileInformationCurrent, FileInformationClass,
          infoIter->second, match.realPath,
          match.virtualName, QueryFlags & SL_RETURN_SINGLE_ENTRY,
          dataRead)) {
          // a positive result here means the call returned data and there may
          // be further objects to be retrieved by repeating the call
          dataReturned = true;
        }
        else {
          // proceed to next search handle

          // TODO: doesn't append search results from more than one redirection
          // per call. This is bad for performance but otherwise we'd need to
          // re-write the offsets between information objects
          infoIter->second.virtualMatches.pop();
          CloseHandle(infoIter->second.currentSearchHandle);
          infoIter->second.currentSearchHandle = INVALID_HANDLE_VALUE;
        }
      }
    }
  }

  if (!dataReturned) {
    if (firstSearch) {
      res = STATUS_NO_SUCH_FILE;
    }
    else {
      res = STATUS_NO_MORE_FILES;
    }
  }
  else {
    res = STATUS_SUCCESS;
  }
  IoStatusBlock->Status = res;
  IoStatusBlock->Information = dataRead;

  size_t numVirtualFiles = infoIter->second.virtualMatches.size();
  if (true) {
    LOG_CALL()
      .addParam("path", ntdllHandleTracker.lookup(FileHandle))
      .PARAM(FileInformationClass)
      .PARAMWRAP(FileName)
      .PARAM(QueryFlags)
      .PARAM(numVirtualFiles)
      .PARAMWRAP(res);
  }

  HOOK_END
  return res;
}

unique_ptr_deleter<OBJECT_ATTRIBUTES>
makeObjectAttributes(RedirectionInfo &redirInfo,
                     POBJECT_ATTRIBUTES attributeTemplate)
{
  if (redirInfo.redirected) {
    unique_ptr_deleter<OBJECT_ATTRIBUTES> result(
        new OBJECT_ATTRIBUTES, [](OBJECT_ATTRIBUTES *ptr) { delete ptr; });
    memcpy(result.get(), attributeTemplate, sizeof(OBJECT_ATTRIBUTES));
    result->RootDirectory = nullptr;
    result->ObjectName    = static_cast<PUNICODE_STRING>(redirInfo.path);
    return result;
  } else {
    // just reuse the template with a dummy deleter
    return unique_ptr_deleter<OBJECT_ATTRIBUTES>(attributeTemplate,
                                                 [](OBJECT_ATTRIBUTES *) {});
  }
}

NTSTATUS ntdll_mess_NtOpenFile(PHANDLE FileHandle,
                                         ACCESS_MASK DesiredAccess,
                                         POBJECT_ATTRIBUTES ObjectAttributes,
                                         PIO_STATUS_BLOCK IoStatusBlock,
                                         ULONG ShareAccess, ULONG OpenOptions)
{
  using namespace usvfs;

  PreserveGetLastError ntFunctionsDoNotChangeGetLastError;

  NTSTATUS res = STATUS_NO_SUCH_FILE;

  HOOK_START_GROUP(MutExHookGroup::OPEN_FILE)
  // Why is the usual if (!callContext.active()... check missing?

  bool storePath = false;
  if (((OpenOptions & FILE_DIRECTORY_FILE) != 0UL)
      && ((OpenOptions & FILE_OPEN_FOR_BACKUP_INTENT) != 0UL)) {
    // this may be an attempt to open a directory handle for iterating.
    // If so we need to treat it a little bit differently
/*    usvfs::FunctionGroupLock lock(usvfs::MutExHookGroup::FILE_ATTRIBUTES);
    FILE_BASIC_INFORMATION dummy;
    storePath = FAILED(NtQueryAttributesFile(ObjectAttributes, &dummy));*/
    storePath = true;
  }

  UnicodeString fullName = CreateUnicodeString(ObjectAttributes);

  UnicodeString Path = ntdllHandleTracker.lookup(ObjectAttributes->RootDirectory);

  std::wstring checkpath = ush::string_cast<std::wstring>(
    static_cast<LPCWSTR>(Path), ush::CodePage::UTF8);

  if ((fullName.size() == 0)
      || (GetFileSize(ObjectAttributes->RootDirectory, nullptr)
          != INVALID_FILE_SIZE)) {
	  //	//relative paths that we don't have permission over will fail here due that we can't get the filesize of the root directory
	  //	//We should try again to see if it is a directory using another method
	  if ((fullName.size() == 0) || (GetFileAttributesW((LPCWSTR)checkpath.c_str()) == INVALID_FILE_ATTRIBUTES)) {
          return ::NtOpenFile(FileHandle, DesiredAccess, ObjectAttributes,
                        IoStatusBlock, ShareAccess, OpenOptions);
	  }
  }

  try {
    RedirectionInfo redir
        = applyReroute(READ_CONTEXT(), callContext, fullName);
    unique_ptr_deleter<OBJECT_ATTRIBUTES> adjustedAttributes
        = makeObjectAttributes(redir, ObjectAttributes);

    PRE_REALCALL
    res = ::NtOpenFile(FileHandle, DesiredAccess, adjustedAttributes.get(),
                       IoStatusBlock, ShareAccess, OpenOptions);
    POST_REALCALL
    if (SUCCEEDED(res) && storePath) {
      // store the original search path for use during iteration
      READ_CONTEXT()
          ->customData<SearchHandleMap>(SearchHandles)[*FileHandle]
          = static_cast<LPCWSTR>(fullName);
#pragma message("need to clean up this handle in CloseHandle call")
    }

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
    res = ::NtOpenFile(FileHandle, DesiredAccess, ObjectAttributes,
                       IoStatusBlock, ShareAccess, OpenOptions);
    POST_REALCALL
  }
  HOOK_END

  return res;
}

NTSTATUS WINAPI usvfs::hook_NtOpenFile(PHANDLE FileHandle,
                                         ACCESS_MASK DesiredAccess,
                                         POBJECT_ATTRIBUTES ObjectAttributes,
                                         PIO_STATUS_BLOCK IoStatusBlock,
                                         ULONG ShareAccess, ULONG OpenOptions)
{
  NTSTATUS res = ntdll_mess_NtOpenFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, ShareAccess, OpenOptions);
  if (res >= 0 && ObjectAttributes && FileHandle && GetFileType(*FileHandle) == FILE_TYPE_DISK)
    ntdllHandleTracker.insert(*FileHandle, CreateUnicodeString(ObjectAttributes));
  return res;
}

NTSTATUS ntdll_mess_NtCreateFile(
    PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
    POBJECT_ATTRIBUTES ObjectAttributes, PIO_STATUS_BLOCK IoStatusBlock,
    PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess,
    ULONG CreateDisposition, ULONG CreateOptions, PVOID EaBuffer,
    ULONG EaLength)
{
  using namespace usvfs;

  NTSTATUS res = STATUS_NO_SUCH_FILE;

  PreserveGetLastError ntFunctionsDoNotChangeGetLastError;

  HOOK_START_GROUP(MutExHookGroup::OPEN_FILE)
  if (!callContext.active()) {
    return ::NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes,
                          IoStatusBlock, AllocationSize, FileAttributes,
                          ShareAccess, CreateDisposition, CreateOptions,
                          EaBuffer, EaLength);
  }

  UnicodeString inPath = CreateUnicodeString(ObjectAttributes);
  LPCWSTR inPathW = static_cast<LPCWSTR>(inPath);

  if (inPath.size() == 0) {
    spdlog::get("hooks")->info(
        "failed to set from handle: {0}",
        ush::string_cast<std::string>(ObjectAttributes->ObjectName->Buffer));
    return ::NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes,
                          IoStatusBlock, AllocationSize, FileAttributes,
                          ShareAccess, CreateDisposition, CreateOptions,
                          EaBuffer, EaLength);
  }

  DWORD convertedDisposition = OPEN_EXISTING;
  switch(CreateDisposition) {
    case FILE_SUPERSEDE: convertedDisposition = CREATE_ALWAYS; break;
    case FILE_OPEN: convertedDisposition = OPEN_EXISTING; break;
    case FILE_CREATE: convertedDisposition = CREATE_NEW; break;
    case FILE_OPEN_IF: convertedDisposition = OPEN_ALWAYS; break;
    case FILE_OVERWRITE: convertedDisposition = TRUNCATE_EXISTING; break;
    case FILE_OVERWRITE_IF: convertedDisposition = CREATE_ALWAYS; break;
    default: spdlog::get("hooks")->error("invalid disposition: {0}", CreateDisposition); break;
  }

  DWORD convertedAccess = 0;
  if ((DesiredAccess & FILE_GENERIC_READ) == FILE_GENERIC_READ) convertedAccess |= GENERIC_READ;
  if ((DesiredAccess & FILE_GENERIC_WRITE) == FILE_GENERIC_WRITE) convertedAccess |= GENERIC_WRITE;
  if ((DesiredAccess & FILE_GENERIC_EXECUTE) == FILE_GENERIC_EXECUTE) convertedAccess |= GENERIC_EXECUTE;
  if ((DesiredAccess & FILE_ALL_ACCESS) == FILE_ALL_ACCESS) convertedAccess |= GENERIC_ALL;

  ULONG originalDisposition = CreateDisposition;
  CreateRerouter rerouter;
  if (rerouter.rerouteCreate(READ_CONTEXT(), callContext, inPathW, convertedDisposition,
                             convertedAccess, (LPSECURITY_ATTRIBUTES)ObjectAttributes->SecurityDescriptor)) {
    switch(convertedDisposition) {
      case CREATE_NEW: CreateDisposition = FILE_CREATE; break;
      case CREATE_ALWAYS: if (CreateDisposition != FILE_SUPERSEDE) CreateDisposition = FILE_OVERWRITE_IF; break;
      case OPEN_EXISTING: CreateDisposition = FILE_OPEN; break;
      case OPEN_ALWAYS: CreateDisposition = FILE_OPEN_IF; break;
      case TRUNCATE_EXISTING: CreateDisposition = FILE_OVERWRITE; break;
    }

    RedirectionInfo redir = applyReroute(rerouter);

    unique_ptr_deleter<OBJECT_ATTRIBUTES> adjustedAttributes
      = makeObjectAttributes(redir, ObjectAttributes);

    PRE_REALCALL
      res = ::NtCreateFile(FileHandle, DesiredAccess, adjustedAttributes.get(),
        IoStatusBlock, AllocationSize, FileAttributes,
        ShareAccess, CreateDisposition, CreateOptions, EaBuffer,
        EaLength);
    POST_REALCALL
    rerouter.updateResult(callContext, res == STATUS_SUCCESS);

    if (res == STATUS_SUCCESS) {
      if (rerouter.newReroute())
        rerouter.insertMapping(WRITE_CONTEXT());

      if (rerouter.isDir() && rerouter.wasRerouted() && ((FileAttributes & FILE_OPEN_FOR_BACKUP_INTENT) == FILE_OPEN_FOR_BACKUP_INTENT)) {
        // store the original search path for use during iteration
        WRITE_CONTEXT()
          ->customData<SearchHandleMap>(SearchHandles)[*FileHandle] = inPathW;
      }
    }

    if (rerouter.wasRerouted() || rerouter.changedError() || originalDisposition != CreateDisposition) {
      LOG_CALL()
        .PARAMWRAP(inPathW)
        .PARAMWRAP(rerouter.fileName())
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
      res = ::NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes,
        IoStatusBlock, AllocationSize, FileAttributes,
        ShareAccess, CreateDisposition, CreateOptions, EaBuffer,
        EaLength);
    POST_REALCALL
  }

  HOOK_END

  return res;
}

NTSTATUS WINAPI usvfs::hook_NtCreateFile(
  PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
  POBJECT_ATTRIBUTES ObjectAttributes, PIO_STATUS_BLOCK IoStatusBlock,
  PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess,
  ULONG CreateDisposition, ULONG CreateOptions, PVOID EaBuffer,
  ULONG EaLength)
{
  NTSTATUS res = ntdll_mess_NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize, FileAttributes, ShareAccess, CreateDisposition, CreateOptions, EaBuffer, EaLength);
  if (res >= 0 && ObjectAttributes && FileHandle && GetFileType(*FileHandle) == FILE_TYPE_DISK)
    ntdllHandleTracker.insert(*FileHandle, CreateUnicodeString(ObjectAttributes));
  return res;
}

NTSTATUS WINAPI usvfs::hook_NtClose(HANDLE Handle)
{
  PreserveGetLastError ntFunctionsDoNotChangeGetLastError;

  NTSTATUS res = STATUS_NO_SUCH_FILE;

  HOOK_START_GROUP(MutExHookGroup::ALL_GROUPS)
  bool log = false;

  if ((::GetFileType(Handle) == FILE_TYPE_DISK)) {
    HookContext::Ptr context = WRITE_CONTEXT();

    { // clean up search data associated with this handle part 1
      Searches &activeSearches = context->customData<Searches>(SearchInfo);
//      std::lock_guard<std::recursive_mutex> lock(activeSearches.queryMutex);
      auto iter = activeSearches.info.find(Handle);
      if (iter != activeSearches.info.end()) {
        if (iter->second.currentSearchHandle != INVALID_HANDLE_VALUE) {
          ::CloseHandle(iter->second.currentSearchHandle);
        }

        activeSearches.info.erase(iter);
        log = true;
      }
    }

    {
      SearchHandleMap &searchHandles
          = context->customData<SearchHandleMap>(SearchHandles);
      auto iter = searchHandles.find(Handle);
      if (iter != searchHandles.end()) {
        searchHandles.erase(iter);
        log = true;
      }
    }
  }

  if (GetFileType(Handle) == FILE_TYPE_DISK)
    ntdllHandleTracker.erase(Handle);

  PRE_REALCALL
  res = ::NtClose(Handle);
  POST_REALCALL

  if (log) {
    LOG_CALL().PARAM(Handle).PARAMWRAP(res);
  }

  HOOK_END

  return res;
}

NTSTATUS WINAPI usvfs::hook_NtQueryAttributesFile(
    POBJECT_ATTRIBUTES ObjectAttributes,
    PFILE_BASIC_INFORMATION FileInformation)
{
  PreserveGetLastError ntFunctionsDoNotChangeGetLastError;

  NTSTATUS res = STATUS_SUCCESS;

  HOOK_START_GROUP(MutExHookGroup::FILE_ATTRIBUTES)
  // Why is the usual if (!callContext.active()... check missing?

  UnicodeString inPath = CreateUnicodeString(ObjectAttributes);

  RedirectionInfo redir
      = applyReroute(READ_CONTEXT(), callContext, inPath);
  unique_ptr_deleter<OBJECT_ATTRIBUTES> adjustedAttributes
      = makeObjectAttributes(redir, ObjectAttributes);

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
    POBJECT_ATTRIBUTES ObjectAttributes,
    PFILE_NETWORK_OPEN_INFORMATION FileInformation)
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
  } catch (const std::exception &) {
    return ::NtQueryFullAttributesFile(ObjectAttributes, FileInformation);
  }

  RedirectionInfo redir
      = applyReroute(READ_CONTEXT(), callContext, inPath);
  unique_ptr_deleter<OBJECT_ATTRIBUTES> adjustedAttributes
      = makeObjectAttributes(redir, ObjectAttributes);

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

NTSTATUS WINAPI usvfs::hook_NtTerminateProcess(
  HANDLE ProcessHandle,
  NTSTATUS ExitStatus)
{
  NTSTATUS res = STATUS_SUCCESS;

  HOOK_START

  DisconnectVFS();

  res = ::NtTerminateProcess(ProcessHandle, ExitStatus);

  HOOK_END

  return res;
}
