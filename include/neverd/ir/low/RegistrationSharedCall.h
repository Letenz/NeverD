//===- RegistrationSharedCall.h - Checked C++ helper occurrences -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_REGISTRATIONSHAREDCALL_H
#define NEVERD_IR_LOW_REGISTRATIONSHAREDCALL_H

#include "neverd/loader/COFF/COFFCxxEpilogue.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"

namespace neverd {
struct LowFunc;
struct LowBlock;

/// Bind a complete source frame receipt to its current immutable helper,
/// rooted entry prefix, decoded boundaries and exact lifted operations.
/// These queries describe physical calls, never ordinary callee ABIs or
/// permission to omit memory effects, cookies or native EH checks.
std::optional<coff_loader::X86CxxPrologueCall>
getCheckedX86RegistrationCxxPrologueCall(const BinaryImage &Image,
                                         const LowFunc &Source);

/// Bind one block-final shared exit to its exact current call and removal
/// receipt. Its entry prologue needs the separate check above. A guarded
/// result still requires preserving its checker and failure path.
std::optional<coff_loader::X86CxxEpilogueCall>
getCheckedX86RegistrationCxxEpilogueCall(const BinaryImage &Image,
                                         const LowFunc &Source,
                                         const LowBlock &Block);

} // namespace neverd

#endif
