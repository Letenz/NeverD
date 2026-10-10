//===- RegistrationFrameRealignment.cpp - x86 aligned frame transfer ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationFrame.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86Regs.h"

namespace neverd::registration_state {

std::optional<FrameValue>
FrameTransfer::evaluateRealignment(const LowOp &Op) const {
  if (!Realigned || Realigned->DefinitionVA < 9 ||
      Op.Addr != Realigned->DefinitionVA - 9 || Op.Opcode != NdOp::INT_AND ||
      Op.NumInputs != 2 || !Op.Output.isReg() ||
      Op.Output.Offset != x86reg::RSP || Op.Output.Size != 4 ||
      !Op.Inputs[0].isReg() || Op.Inputs[0].Offset != x86reg::RSP ||
      Op.Inputs[0].Size != 4 || !Op.Inputs[1].isConst() ||
      Op.Inputs[1].Size != 4 || read(Op.Inputs[0]).EntryOffset != -12 ||
      uint32_t(Op.Inputs[1].Offset) != uint32_t(-Realigned->Alignment))
    return std::nullopt;
  const int64_t Offset =
      int64_t(Realigned->BaseOffset) + Realigned->AllocationBytes;
  if (Offset < INT32_MIN || Offset > INT32_MAX)
    return std::nullopt;
  // This AND establishes a new aligned origin. The entry EBP values and cells
  // remain in their own coordinate space; none is rebased into this frame.
  return FrameValue::frame(int32_t(Offset));
}

} // namespace neverd::registration_state
