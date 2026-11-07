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
#include "hookcallcontext.h"
#include "hookcontext.h"
#include <logging.h>

#include <climits>
#include <cstdint>

namespace usvfs
{

// The hook groups active on each thread, kept as the bits of the value of a TLS slot.
// Nothing is allocated per thread, so nothing needs freeing when a thread exits, and
// hooks running after that cannot leave behind state that is never freed.
class HookStack
{
public:
  // Marks the group active on the calling thread. Returns false if it or ALL_GROUPS
  // already is, or if no TLS slot was left, in which case no hook becomes active.
  static bool setGroup(MutExHookGroup group)
  {
    if (s_slot == TLS_OUT_OF_INDEXES) {
      return false;
    }
    const uintptr_t groups = activeGroups();
    if ((groups & (bitOf(group) | bitOf(MutExHookGroup::ALL_GROUPS))) != 0) {
      return false;
    }
    ::TlsSetValue(s_slot, reinterpret_cast<LPVOID>(groups | bitOf(group)));
    return true;
  }

  static void unsetGroup(MutExHookGroup group)
  {
    if (s_slot == TLS_OUT_OF_INDEXES) {
      return;
    }
    ::TlsSetValue(s_slot, reinterpret_cast<LPVOID>(activeGroups() & ~bitOf(group)));
  }

private:
  static_assert(static_cast<size_t>(MutExHookGroup::LAST) <=
                    sizeof(uintptr_t) * CHAR_BIT,
                "the hook groups must fit in the value of a TLS slot");

  static uintptr_t bitOf(MutExHookGroup group)
  {
    return uintptr_t{1} << static_cast<int>(group);
  }

  static uintptr_t activeGroups()
  {
    PreserveGetLastError tlsGetValueClearsLastError;
    return reinterpret_cast<uintptr_t>(::TlsGetValue(s_slot));
  }

  static const DWORD s_slot;
};

const DWORD HookStack::s_slot = ::TlsAlloc();

HookCallContext::HookCallContext() : m_Active(true), m_Group(MutExHookGroup::NO_GROUP)
{
  updateLastError();
}

HookCallContext::HookCallContext(MutExHookGroup group)
    : m_Active(HookStack::setGroup(group)), m_Group(group)
{
  updateLastError();
}

HookCallContext::~HookCallContext()
{
  if (m_Active && (m_Group != MutExHookGroup::NO_GROUP)) {
    HookStack::unsetGroup(m_Group);
  }
  SetLastError(m_LastError);
}

void HookCallContext::restoreLastError()
{
  SetLastError(m_LastError);
}

void HookCallContext::updateLastError(DWORD lastError)
{
  m_LastError = lastError;
}

bool HookCallContext::active() const
{
  return m_Active;
}

FunctionGroupLock::FunctionGroupLock(MutExHookGroup group) : m_Group(group)
{
  m_Active = HookStack::setGroup(m_Group);
}

FunctionGroupLock::~FunctionGroupLock()
{
  if (m_Active) {
    HookStack::unsetGroup(m_Group);
  }
}

}  // namespace usvfs
