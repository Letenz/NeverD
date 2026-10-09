//===- MedReturnValue.h - Propagated integer return values ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDRETURNVALUE_H
#define NEVERD_IR_MED_MEDRETURNVALUE_H

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedIR.h"

namespace neverd {

/// Copy propagation can assemble a return register from overlapping writes
/// into a temporary. That complete reaching value precedes older physical
/// register definitions. A narrow temporary still needs ABI reconstruction.
inline bool hasPropagatedIntegerReturnValue(const MedOp &Return, Arch TheArch,
                                            uint16_t ReturnSize) {
  return Return.Opcode == NdOp::RETURN && Return.NumInputs == 1 &&
         getTargetRegInfo(TheArch).ReturnOperandIsValue && ReturnSize != 0 &&
         Return.Inputs[0].Kind == MedVar::Temp && Return.Inputs[0].Id >= 0 &&
         Return.Inputs[0].Size >= ReturnSize;
}

} // namespace neverd

#endif
