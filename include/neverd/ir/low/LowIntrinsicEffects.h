//===- LowIntrinsicEffects.h - Validate LowIR intrinsic effects -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validate LowIR intrinsic effects.

#ifndef NEVERD_IR_LOW_LOWINTRINSICEFFECTS_H
#define NEVERD_IR_LOW_LOWINTRINSICEFFECTS_H

#include "neverd/ir/intrinsics/IntrinsicValueEffects.h"
#include "neverd/ir/low/LowIR.h"

namespace neverd {

/// One shared effect classification for LowIR consumers. A malformed or
/// unknown intrinsic cannot grant address-suppression or call-barrier proofs.
inline bool lowIntrinsicHasOnlyValueEffects(const LowOp &Op) {
  if (Op.Opcode != NdOp::INTRINSIC || Op.NumInputs == 0 || Op.NumInputs > 6 ||
      !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 2 ||
      Op.Inputs[0].Offset >= static_cast<uint64_t>(Intrinsic::_Count) ||
      (!Op.Output.isReg() && !Op.Output.isTemp()) || Op.Output.Size == 0 ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  for (uint8_t I = 1; I < Op.NumInputs; ++I)
    if ((!Op.Inputs[I].isReg() && !Op.Inputs[I].isTemp() &&
         !Op.Inputs[I].isConst()) ||
        Op.Inputs[I].Size == 0)
      return false;
  return intrinsicHasOnlyValueEffects(
      static_cast<Intrinsic>(Op.Inputs[0].Offset), Op.NumInputs, Op.Output.Size,
      Op.NumInputs > 1 ? Op.Inputs[1].Size : 0,
      Op.NumInputs > 2 ? Op.Inputs[2].Size : 0);
}

} // namespace neverd

#endif // NEVERD_IR_LOW_LOWINTRINSICEFFECTS_H
