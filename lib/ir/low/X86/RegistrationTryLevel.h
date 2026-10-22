//===- RegistrationTryLevel.h - Decoded PE32 state stores -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONTRYLEVEL_H
#define NEVERD_IR_LOW_X86_REGISTRATIONTRYLEVEL_H

#include "llvm/ADT/ArrayRef.h"

namespace neverd {
struct LowOp;
struct RegistrationChainInfo;

namespace registration_state {

/// An instruction-local register or bitwise write to the state coordinate.
/// This only identifies a possible CFG cut. The state solver must prove the
/// actual frame address and stored value on every incoming path. A bitwise
/// update retains its memory read, write and flag effects.
const LowOp *stateStoreCandidate(const RegistrationChainInfo &Chain,
                                 llvm::ArrayRef<LowOp> Instruction);

} // namespace registration_state
} // namespace neverd

#endif
