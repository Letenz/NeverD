//===- COFFRegistrationEH.h - x86-32 registration-chain EH ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recovers Windows exception state for x86-32 images, which carry no
/// `.pdata` directory at all.
///
/// On x86-32 the unwinder walks a linked list of `EXCEPTION_REGISTRATION_
/// RECORD`s rooted at `FS:[0]` that each frame's prologue pushes onto the
/// stack.  The exception tables are therefore not reachable from a directory:
/// they are reachable only from the instructions that install the record, so
/// recovery starts by proving that install sequence in the code and follows
/// the operands it pushes.  Every address that reaches a caller has been
/// checked against the image, and a table whose shape could not be proven is
/// reported rather than guessed.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_COFF_COFFREGISTRATIONEH_H
#define NEVERD_LOADER_COFF_COFFREGISTRATIONEH_H

#include "neverd/loader/BinaryImage.h"

#include <map>
#include <optional>
#include <set>
#include <vector>

namespace neverd::coff_loader {

/// Scan an x86-32 image for `FS:[0]` registration installs and decode the
/// `_except_handler3`/`_except_handler4` scope tables and `__CxxFrameHandler`
/// `FuncInfo` records they reference.  A no-op for other architectures.
///
/// Must run after function discovery and the COFF symbol table are available,
/// because a recovered record is attributed to the function that contains its
/// install site and handler names come from symbols and import veneers.
void parseX86RegistrationExceptions(BinaryImage &Img);

/// Reparse an absolute-pointer FuncInfo and require equality with its checked
/// normalized graph. Only these exact native fields carry runtime callback
/// roles; independent references to the same target keep their ordinary role.
std::optional<std::map<va_t, va_t>>
getCheckedX86CxxCallbackPointerSources(const BinaryImage &Img,
                                       const ExceptionFunction &Function);

/// Image-wide callback pointer roles, reconstructed from the same checked
/// FuncInfo parser. RuntimeOnlyPointerTargets have no independent relocated
/// code-pointer source; exports, stated symbols and ordinary calls can still
/// give them independent function-entry roles.
struct X86CxxCallbackPointerRoles {
  std::map<va_t, va_t> Sources;
  std::set<va_t> RuntimeOnlyPointerTargets;
};
std::optional<X86CxxCallbackPointerRoles>
getCheckedX86CxxCallbackPointerRoles(const BinaryImage &Img);

/// Checked PE32 scalar throw metadata with one simple catchable type, no
/// copy/destructor/forwarding callback and no pointer adjustment. The three
/// metadata records must be immutable mapped data. TypeDescriptor is retained
/// by identity; it does not supply a guessed C++ type or object layout.
struct X86SimpleCxxThrowInfo {
  va_t Address = InvalidVA;
  va_t TypeDescriptorVA = InvalidVA;
  uint32_t Attributes = 0;
  uint32_t ObjectSize = 0;
  std::vector<ExceptionAddressRange> ReadOnlyRanges;
  ExceptionAddressRange TypeDescriptorRange;
};
std::optional<X86SimpleCxxThrowInfo>
getCheckedX86SimpleCxxThrowInfo(const BinaryImage &Img, va_t Address);

/// Authenticate an argument-preserving veneer to the known CRT SEH3 import.
bool isCheckedX86SEH3Personality(const BinaryImage &Img, va_t HandlerVA);

/// Authenticate the direct CRT EH4 forwarding wrapper and return its cookie
/// checker. Names/byte-search observations alone do not authorize rewriting:
/// all four dispatcher arguments, the exact image cookie and the common CRT
/// import must occupy the checked cdecl slots on the sole forwarding path.
std::optional<va_t> getCheckedX86EH4CookieCheck(const BinaryImage &Img,
                                                va_t HandlerVA);

/// Check the leaf success path: compare ECX with the exact image cookie,
/// branch to the original failure code on mismatch, otherwise return without
/// touching stack storage or other registers. This authorizes no elision for
/// an argument whose equality with that cookie has not separately been proved.
bool hasCheckedX86CookieCheckSuccessPath(const BinaryImage &Img, va_t CheckVA);

} // namespace neverd::coff_loader

#endif // NEVERD_LOADER_COFF_COFFREGISTRATIONEH_H
