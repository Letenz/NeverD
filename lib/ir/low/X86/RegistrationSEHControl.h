//===- RegistrationSEHControl.h - Physical finally control ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONSEHCONTROL_H
#define NEVERD_IR_LOW_X86_REGISTRATIONSEHCONTROL_H

#include "neverd/Common.h"
#include "neverd/ir/RegistrationCall.h"

#include "llvm/ADT/ArrayRef.h"

#include <vector>

namespace neverd {
struct BinaryImage;
struct ExceptionFunction;
struct LowOp;
struct LowBlock;
struct RegistrationChainInfo;

/// Authenticate a complete direct parent epilogue. This proves source bytes,
/// not the identities of the saved registers and return cell it consumes.
std::optional<RegistrationSEHControlContract>
getRegistrationSEHParentEpilogue(const BinaryImage &Image,
                                 const LowBlock &Block);

namespace registration_state {

/// The registration record that the function at \p Entry installed, or null.
/// A record's range runs from its installer on to the next function the
/// loader knew of, so an entry inside it is another function, or a call into
/// the installer's own finally block: the record's scopes name handlers that
/// run on the installer's frame, and none of them is that entry's code.
const ExceptionFunction *installedRegistration(const BinaryImage &Image,
                                               va_t Entry);

/// Preserve an adjacent CALL into a checked finally as an invocation, rather
/// than treating its pushed PC as an ordinary fallthrough value.
void restoreAdjacentSEHFinallyCall(const BinaryImage &Image,
                                   const ExceptionFunction &Exception,
                                   va_t Address, uint16_t Size,
                                   std::vector<LowOp> &Ops);

/// Bind an ordinary near CALL to a scope-owned EH3 or EH4 finally. Partition
/// its physical return PC before analyzing the invocation's frame effects.
bool isSEHFinallyCallInstruction(const BinaryImage &Image,
                                 const RegistrationChainInfo &Chain,
                                 va_t Address, uint16_t Size,
                                 llvm::ArrayRef<LowOp> Ops);

} // namespace registration_state
} // namespace neverd

#endif // NEVERD_IR_LOW_X86_REGISTRATIONSEHCONTROL_H
