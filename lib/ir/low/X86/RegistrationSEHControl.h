//===- RegistrationSEHControl.h - Physical finally control ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONSEHCONTROL_H
#define NEVERD_IR_LOW_X86_REGISTRATIONSEHCONTROL_H

#include "neverd/Common.h"

#include "llvm/ADT/ArrayRef.h"

namespace neverd {
struct BinaryImage;
struct LowOp;
struct RegistrationChainInfo;

namespace registration_state {

/// Bind an ordinary near CALL to a scope-owned EH3 or EH4 finally. Partition
/// its physical return PC before analyzing the invocation's frame effects.
bool isSEHFinallyCallInstruction(const BinaryImage &Image,
                                 const RegistrationChainInfo &Chain,
                                 va_t Address, uint16_t Size,
                                 llvm::ArrayRef<LowOp> Ops);

} // namespace registration_state
} // namespace neverd

#endif // NEVERD_IR_LOW_X86_REGISTRATIONSEHCONTROL_H
