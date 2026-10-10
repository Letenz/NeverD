//===- LowNoReturn.h - LowIR terminating-path proof --------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_LOWNORETURN_H
#define NEVERD_IR_LOW_LOWNORETURN_H

#include "neverd/ir/low/LowIR.h"

namespace neverd {

bool isArchitecturalNoReturn(const LowOp &Op);
inline bool isArchitecturalNoReturn(const LowOp &Op, Arch) {
  return isArchitecturalNoReturn(Op);
}

/// Follow ordinary and exceptional paths using exact instruction boundaries.
/// Returning or unknown exceptional destinations prevent a no-return proof.
bool lowFunctionNeverReturns(const LowFunc &Func, Arch TheArch);

} // namespace neverd

#endif
