//===- RegistrationFrameEffects.h - PE32 frame effects ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONFRAMEEFFECTS_H
#define NEVERD_IR_LOW_X86_REGISTRATIONFRAMEEFFECTS_H

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86Regs.h"

namespace neverd::registration_state {

/// WAIT and checked FP arithmetic write no integer registers or frame memory.
/// Their FP state effects and traps remain in LowIR. A memory arithmetic form
/// reads its operand; it cannot grant memory privacy or arbitrary intrinsics.
inline bool isFramePreservingIntrinsic(const LowOp &Op) {
  if (Op.Opcode != NdOp::INTRINSIC || !Op.NumInputs ||
      !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 2 ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Op.MemoryOrdering != NdMemoryOrdering::None)
    return false;
  const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
  const bool NonIntegerOutput =
      Op.Output.isTemp() ||
      (Op.Output.isReg() && Op.Output.Offset >= 8 * x86reg::GeneralRegStride);
  if (Id == Intrinsic::X86ReadMXCSR)
    return Op.NumInputs == 1 && Op.Output.Size == 4 && NonIntegerOutput;
  if (Id == Intrinsic::X86WriteMXCSR)
    return Op.NumInputs == 2 && !Op.Output.Size && Op.Inputs[1].Size == 4 &&
           (Op.Inputs[1].isTemp() || Op.Inputs[1].isReg() ||
            Op.Inputs[1].isConst());
  if (Id == Intrinsic::X86FPArithState ||
      Id == Intrinsic::X86FPArithMemoryState)
    return NonIntegerOutput &&
           x86FPStateShapeIsValid(Id, x86FPStateLowShape(Op, Arch::X86));
  return Op.NumInputs == 1 && !Op.Output.Size && Op.Inputs[0].isConst() &&
         Op.Inputs[0].Size == 2 &&
         Op.Inputs[0].Offset == static_cast<unsigned>(Intrinsic::X87Wait) &&
         Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
         Op.MemoryOrdering == NdMemoryOrdering::None;
}

} // namespace neverd::registration_state

#endif // NEVERD_IR_LOW_X86_REGISTRATIONFRAMEEFFECTS_H
