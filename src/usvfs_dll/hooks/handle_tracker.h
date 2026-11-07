#pragma once

#include <cstring>
#include <memory>
#include <utility>

#include <ntdll_declarations.h>
#include <unicodestring.h>

#include "handle_registry.h"

inline bool isValidHandle(HANDLE handle)
{
  return handle && handle != INVALID_HANDLE_VALUE;
}

// The paths that handles were opened with, for the hooks that query a handle for its
// path, or open a path relative to it.
class HandleTracker
{
public:
  using handle_type = HANDLE;
  using info_type   = usvfs::UnicodeString;

  HandleTracker() { insert_current_directory(); }

  info_type lookup(handle_type handle) const
  {
    // copied out of the shared path, after the lock of the registry is released
    const auto info = m_paths.find(handle);
    return info ? *info : info_type();
  }

  void insert(handle_type handle, info_type info)
  {
    m_paths.insert(handle, std::make_shared<const info_type>(std::move(info)));
  }

  // Wait-free, so that NtClose can call it wherever it is called (see HandleRegistry).
  void erase(handle_type handle) noexcept { m_paths.erase(handle); }

private:
  void insert_current_directory()
  {
    ntdll_declarations_init();

    UNICODE_STRING p{0};
    RTL_RELATIVE_NAME r{0};
    NTSTATUS status =
        RtlDosPathNameToRelativeNtPathName_U_WithStatus(L"x", &p, nullptr, &r);
    if (status >= 0) {
      if (r.ContainingDirectory && p.Buffer) {
        size_t len  = p.Length / sizeof(WCHAR);
        size_t trim = strlen("\\x") + (p.Buffer[len - 1] ? 0 : 1);
        if (len > trim)
          insert(r.ContainingDirectory, info_type(p.Buffer, len - trim));
      }
      RtlReleaseRelativeName(&r);
      if (p.Buffer)
        HeapFree(GetProcessHeap(), 0, p.Buffer);
    }
  }

  HandleRegistry<const info_type> m_paths;
};

// Never destroyed, so that hooks still running while the DLL unloads do not touch a
// destroyed tracker.
extern HandleTracker& ntdllHandleTracker;
