//===- LowNoReturn.h - LowIR terminating-path proof --------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_LOWNORETURN_H
#define NEVERD_IR_LOW_LOWNORETURN_H

#include "neverd/ir/low/LowIR.h"

#include "llvm/ADT/STLFunctionalExtras.h"

namespace neverd {

/// Ends the ordinary path; exceptional successors remain reachable.
inline bool isUnconditionalNoReturn(const LowInstructionBoundary &Insn) {
  return hasLowInstructionControlFlag(Insn.ControlFlags,
                                      LowInstructionControlFlag::NoReturn) &&
         !hasLowInstructionControlFlag(Insn.ControlFlags,
                                       LowInstructionControlFlag::Conditional);
}

bool isArchitecturalNoReturn(const LowOp &Op);
inline bool isArchitecturalNoReturn(const LowOp &Op, Arch) {
  return isArchitecturalNoReturn(Op);
}

/// Follow ordinary and exceptional paths using exact instruction boundaries.
/// Returning or unknown exceptional destinations prevent a no-return proof.
/// A direct tail call ends its path where \p TailCalleeNeverReturns answers
/// that its target never returns; without that answer it returns.
bool lowFunctionNeverReturns(
    const LowFunc &Func, Arch TheArch,
    llvm::function_ref<bool(va_t)> TailCalleeNeverReturns = nullptr);

} // namespace neverd

#endif
