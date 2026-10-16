//===- IntrinsicValueEffects.h - Intrinsic value effects --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Explicit intrinsic value effects.

#ifndef NEVERD_IR_INTRINSICS_INTRINSICVALUEEFFECTS_H
#define NEVERD_IR_INTRINSICS_INTRINSICVALUEEFFECTS_H

#include "neverd/ir/intrinsics/Intrinsics.h"

#include <optional>

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

/// Operand roles of an intrinsic whose complete effects are its explicit
/// output and one access to the memory at an explicit address operand.  Input
/// indices count the constant intrinsic ID as input 0; AccessSize names the
/// constant access width.  A zero StoredValue marks a read; otherwise it is
/// the value the access writes or combines with the old memory value.
/// ReturnsMemoryValue is set when the output is the old memory value rather
/// than a status.  The exclusive monitor and the memory ordering these
/// operations carry are neither register nor memory values.
struct IntrinsicMemoryAccessRoles {
  uint8_t NumInputs = 0;
  uint8_t Address = 0;
  uint8_t StoredValue = 0;
  uint8_t AccessSize = 0;
  bool ReturnsMemoryValue = false;
};

/// Roles of the AArch64 exclusive loads and stores and of the scalar LSE
/// read-modify-writes, in the shapes the AArch64 lifter emits.  Unknown IDs
/// and unlisted operations retain conservative effect handling.
constexpr std::optional<IntrinsicMemoryAccessRoles>
intrinsicMemoryAccessRoles(Intrinsic Id) {
  switch (Id) {
  case Intrinsic::A64_Ldxr:
  case Intrinsic::A64_Ldaxr:
  case Intrinsic::A64_Ldxp:
  case Intrinsic::A64_Ldaxp:
    // [id, address, width] -> loaded value.
    return IntrinsicMemoryAccessRoles{.NumInputs = 3,
                                      .Address = 1,
                                      .AccessSize = 2,
                                      .ReturnsMemoryValue = true};
  case Intrinsic::A64_Stxr:
  case Intrinsic::A64_Stlxr:
  case Intrinsic::A64_Stxp:
  case Intrinsic::A64_Stlxp:
    // [id, value, address, width] -> status; the store may not happen.
    return IntrinsicMemoryAccessRoles{
        .NumInputs = 4, .Address = 2, .StoredValue = 1, .AccessSize = 3};
  case Intrinsic::A64_AtomicAnd:
  case Intrinsic::A64_AtomicOr:
  case Intrinsic::A64_AtomicXor:
  case Intrinsic::A64_AtomicSmax:
  case Intrinsic::A64_AtomicSmin:
  case Intrinsic::A64_AtomicUmax:
  case Intrinsic::A64_AtomicUmin:
    // [id, operand, address, width, ordering] -> old value.
    return IntrinsicMemoryAccessRoles{.NumInputs = 5,
                                      .Address = 2,
                                      .StoredValue = 1,
                                      .AccessSize = 3,
                                      .ReturnsMemoryValue = true};
  default:
    return std::nullopt;
  }
}

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_INTRINSICVALUEEFFECTS_H
