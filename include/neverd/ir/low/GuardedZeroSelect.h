//===- GuardedZeroSelect.h - Selects a zero test has decided ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// `bsr r8, rdx` leaves R8 unchanged when RDX is zero, which the lifter spells
// `R8 = (RDX == 0) ? R8 : 63 - clz(RDX)`.  Compiled code tests the source
// first (`test rdx, rdx; je` before the bsr), and on the path where it is
// nonzero the old R8 is never the result: reading it would make R8 an
// incoming value the function never uses.
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_GUARDEDZEROSELECT_H
#define NEVERD_IR_LOW_GUARDEDZEROSELECT_H

#include "neverd/ir/low/LowIR.h"

namespace neverd {

/// In each block of \p F whose one predecessor branches on whether a
/// register is zero, and reaches it only where the register is nonzero,
/// replace `SELECT d, (x == 0), a, b` on that register (or a wider view of
/// it) by `COPY d, b` while the register keeps its value.  Returns the
/// number of selects replaced.
unsigned foldGuardedZeroSelects(LowFunc &F);

} // namespace neverd

#endif // NEVERD_IR_LOW_GUARDEDZEROSELECT_H
