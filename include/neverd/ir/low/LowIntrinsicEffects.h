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

/// Whether \p Op is an intrinsic, in its lifted shape, that writes no memory
/// and no register but its explicit output (intrinsicWritesOnlyItsOutput).
inline bool lowIntrinsicWritesOnlyItsOutput(const LowOp &Op) {
  return Op.Opcode == NdOp::INTRINSIC && Op.NumInputs == 1 &&
         Op.Inputs[0].isConst() && Op.Inputs[0].Size == 2 &&
         Op.Inputs[0].Offset < static_cast<uint64_t>(Intrinsic::_Count) &&
         (Op.Output.isReg() || Op.Output.isTemp()) && Op.Output.Size != 0 &&
         Op.MemoryOrdering == NdMemoryOrdering::None &&
         Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
         intrinsicWritesOnlyItsOutput(
             static_cast<Intrinsic>(Op.Inputs[0].Offset));
}

/// Whether \p Op is an intrinsic that unconditionally traps: no state after
/// it, including the output an opaque lift gives it, is reached on its path.
inline bool lowIntrinsicIsUnconditionalTrap(const LowOp &Op) {
  return Op.Opcode == NdOp::INTRINSIC && Op.NumInputs >= 1 &&
         Op.Inputs[0].isConst() && Op.Inputs[0].Size == 2 &&
         Op.Inputs[0].Offset < static_cast<uint64_t>(Intrinsic::_Count) &&
         isUnconditionalTrapIntrinsic(
             static_cast<Intrinsic>(Op.Inputs[0].Offset));
}

/// The memory operands of an intrinsic whose complete effects
/// intrinsicMemoryAccessRoles describes, checked against its LowIR shape.
/// Complete stays false for every other operation, so an unknown or malformed
/// intrinsic keeps conservative effect handling.
inline LowMemoryOperandView lowIntrinsicMemoryOperands(const LowOp &Op) {
  LowMemoryOperandView View;
  if (Op.Opcode != NdOp::INTRINSIC || Op.NumInputs == 0 || Op.NumInputs > 6 ||
      !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 2 ||
      Op.Inputs[0].Offset >= static_cast<uint64_t>(Intrinsic::_Count) ||
      (!Op.Output.isReg() && !Op.Output.isTemp()) || Op.Output.Size == 0 ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return View;
  const auto Roles =
      intrinsicMemoryAccessRoles(static_cast<Intrinsic>(Op.Inputs[0].Offset));
  if (!Roles || Op.NumInputs != Roles->NumInputs)
    return View;
  for (uint8_t I = 1; I < Op.NumInputs; ++I) {
    const NdVar &Input = Op.Inputs[I];
    const bool Operand = I == Roles->Address || I == Roles->StoredValue;
    if (Input.Size == 0 ||
        (Operand ? !Input.isReg() && !Input.isTemp() && !Input.isConst()
                 : !Input.isConst()))
      return View;
  }
  const uint64_t Width = Op.Inputs[Roles->AccessSize].Offset;
  if (Width == 0 || Width > 16 ||
      (Roles->ReturnsMemoryValue && Op.Output.Size != Width) ||
      (Roles->StoredValue && Op.Inputs[Roles->StoredValue].Size != Width))
    return View;
  View.Address = &Op.Inputs[Roles->Address];
  if (Roles->StoredValue)
    View.StoredValue = &Op.Inputs[Roles->StoredValue];
  View.AccessSize = static_cast<uint16_t>(Width);
  View.Complete = true;
  return View;
}

} // namespace neverd

#endif // NEVERD_IR_LOW_LOWINTRINSICEFFECTS_H
