//===- RegistrationFrameEffects.h - PE32 frame effects ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONFRAMEEFFECTS_H
#define NEVERD_IR_LOW_X86_REGISTRATIONFRAMEEFFECTS_H

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/LowIR.h"

namespace neverd::registration_state {

/// WAIT observes pending x87 exceptions without writing GPRs or frame memory.
/// It remains a side-effecting, potentially trapping LowOp, so exceptional
/// dispatch and native emission retain the instruction. This predicate grants
/// only an integer-frame effect; it cannot authorize arbitrary intrinsics.
inline bool isFramePreservingIntrinsic(const LowOp &Op) {
  return Op.Opcode == NdOp::INTRINSIC && Op.NumInputs == 1 && !Op.Output.Size &&
         Op.Inputs[0].isConst() && Op.Inputs[0].Size == 2 &&
         Op.Inputs[0].Offset == static_cast<unsigned>(Intrinsic::X87Wait) &&
         Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
         Op.MemoryOrdering == NdMemoryOrdering::None;
}

} // namespace neverd::registration_state

#endif // NEVERD_IR_LOW_X86_REGISTRATIONFRAMEEFFECTS_H
