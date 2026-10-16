//===- IntrinsicValueEffects.h - Intrinsic value effects ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Explicit intrinsic value effects.

#ifndef NEVERD_IR_INTRINSICS_INTRINSICVALUEEFFECTS_H
#define NEVERD_IR_INTRINSICS_INTRINSICVALUEEFFECTS_H

#include "neverd/ir/intrinsics/Intrinsics.h"

namespace neverd {

/// Positive contracts for intrinsic operations whose complete effects are
/// explicit input/output values. This is stronger than isSideeffectIntrinsic:
/// a removable read-only memory operation still observes its address operand.
/// Unknown IDs and unlisted operations retain conservative effect handling.
/// Callers must also validate scalar operand kinds, the ID encoding, and the
/// absence of memory ordering/address-space annotations.
constexpr bool intrinsicHasOnlyValueEffects(Intrinsic Id, uint8_t NumInputs,
                                            uint16_t OutputSize,
                                            uint16_t FirstInputSize,
                                            uint16_t SecondInputSize = 0) {
  if (isPdepPextIntrinsic(Id)) {
    return intrinsicPdepPextShapeIsValid(Id, {.NumInputs = NumInputs,
                                              .IntrinsicIdIsConst = true,
                                              .IntrinsicIdSize = 2,
                                              .OutputIsWritableScalar = true,
                                              .OutputSize = OutputSize,
                                              .SourceIsScalar = true,
                                              .SourceSize = FirstInputSize,
                                              .MaskIsScalar = true,
                                              .MaskSize = SecondInputSize});
  }
  switch (Id) {
  case Intrinsic::X87Fxam:
    // ST0 and the previous status word are explicit values. The x87 CFG owner
    // resolves physical tags; no operand is published or treated as memory.
    return NumInputs == 3 && OutputSize == 2 && FirstInputSize == 10 &&
           SecondInputSize == 2;
  case Intrinsic::ArmRbit:
    return NumInputs == 2 && OutputSize == 4 && FirstInputSize == 4;
  case Intrinsic::A64_Rbit:
    // Vector RBIT is lifted as independent one-byte scalar operations.
    return NumInputs == 2 && FirstInputSize == OutputSize &&
           (OutputSize == 1 || OutputSize == 4 || OutputSize == 8);
  default:
    return false;
  }
}

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_INTRINSICVALUEEFFECTS_H
