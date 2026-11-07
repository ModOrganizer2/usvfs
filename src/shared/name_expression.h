#pragma once

#include "ntdll_declarations.h"

namespace usvfs::shared
{

// RtlIsNameInExpression for an ntdll that does not export it, as Wine's does not, with
// the same contract: whether Name matches Expression, which may hold the wildcards *
// and ? and the DOS wildcards < > " that file systems take in directory queries. With
// IgnoreCase, the characters of Name are upcased through UpcaseTable, or through
// RtlUpcaseUnicodeChar without one, while those of Expression are compared as they are,
// so that it has to be upcased already.
BOOLEAN NTAPI isNameInExpression(PUNICODE_STRING Expression, PUNICODE_STRING Name,
                                 BOOLEAN IgnoreCase, PWCH UpcaseTable);

}  // namespace usvfs::shared
