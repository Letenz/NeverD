//===- MedCallClobber.h - What a call leaves in a register -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The clobber a call leaves in a register whose result it no longer writes.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDCALLCLOBBER_H
#define NEVERD_IR_MED_MEDCALLCLOBBER_H

#include "neverd/ir/med/MedIR.h"

namespace neverd {

/// \p Call, whose output was \p Replaced, now writes another register: a
/// floating-point result.  The call still clobbers \p Replaced's register, so
/// a read of the old value reads what the callee left there; record it as the
/// call's clobber of that register when anything reads it.
inline void keepReplacedCallOutput(MedFunc &Func, const MedOp &Call,
                                   const MedVar &Replaced) {
  if (Replaced.Kind != MedVar::Reg || Replaced.Size == 0)
    return;
  for (const MedBlock &Block : Func.Blocks) {
    for (const PhiNode &Phi : Block.Phis)
      for (const auto &Arg : Phi.Args)
        if (Arg.second == Replaced) {
          Func.CallClobbers.push_back({Replaced, Call.CallSiteId});
          return;
        }
    for (const MedOp &Op : Block.Ops)
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        if (Op.Inputs[I] == Replaced) {
          Func.CallClobbers.push_back({Replaced, Call.CallSiteId});
          return;
        }
  }
}

} // namespace neverd

#endif // NEVERD_IR_MED_MEDCALLCLOBBER_H
