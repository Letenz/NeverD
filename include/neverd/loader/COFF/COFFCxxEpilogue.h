//===- COFFCxxEpilogue.h - Physical PE32 C++ exits ---------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_COFF_COFFCXXEPILOGUE_H
#define NEVERD_LOADER_COFF_COFFCXXEPILOGUE_H

#include "neverd/loader/ExceptionCommon.h"

#include <optional>

namespace neverd {
struct BinaryImage;
struct ExceptionFunction;

namespace coff_loader {

/// The optional wrapper decodes [EBP-16] with EBP, calls the checked leaf,
/// and tail-jumps to Restore. Its successful CMP sets ZF/PF and clears
/// CF/AF/SF/OF; no caller flag value may survive in those six flags.
struct X86CxxEpilogueCookieCheck {
  ExceptionAddressRange Checker;
  va_t SecurityCookieVA = 0;
  va_t FailureVA = 0;
};

/// The shared epilogue restores FS:[0] from EBP-12, discards the
/// word at caller ESP, and loads EDI/ESI/EBX from caller ESP+4/+8/+12.
/// It restores EBP from [EBP], returns with ESP at old EBP+4, and preserves
/// EAX and EDX. Without CookieCheck it also preserves flags.
/// ECX contains Caller.End. The transient helper return
/// address also overwrites [old EBP], below the returned stack pointer.
///
/// Current bytes authenticate this physical protocol only. The caller must
/// separately prove the active node, live saves, original return address and
/// exact decoded invocation. This grants no ordinary callee or native ABI.
struct X86CxxEpilogueCall {
  ExceptionAddressRange Caller;
  ExceptionAddressRange Helper;
  ExceptionAddressRange Restore;
  std::optional<X86CxxEpilogueCookieCheck> CookieCheck;
};

std::optional<X86CxxEpilogueCall>
getCheckedX86CxxEpilogueCall(const BinaryImage &Image,
                             const ExceptionFunction &Function, va_t CallVA);

} // namespace coff_loader
} // namespace neverd

#endif
