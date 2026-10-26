//===- AArch64ImmediateMemoryImage.h - Image base witnesses ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exact original-memory base witnesses for image-address provenance proofs.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_AARCH64IMMEDIATEMEMORYIMAGE_H
#define NEVERD_IR_LOW_AARCH64IMMEDIATEMEMORYIMAGE_H

#include "neverd/ir/low/LowIntrinsicEffects.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/loader/AArch64MemoryImage.h"
#include "neverd/support/BinaryEncoding.h"

#include <cstdint>

namespace neverd {

struct LowImageBaseMemoryWitness {
  const NdVar *Base = nullptr;
  uint16_t AccessSize = 0;
  bool Complete = false;
};

/// This view authenticates the address-forming base, not an ordinary effective
/// address. Only an exact image-value proof may use it: generic memory, alias
/// and no-fault analyses must continue to see the raw instruction as opaque.
inline LowImageBaseMemoryWitness
lowImageBaseMemoryWitness(const BinaryImage &Image, const LowOp &Op,
                          va_t OriginalBase, va_t Owner = InvalidVA) {
  const auto Memory = lowMemoryOperands(Op);
  if (Memory.Complete)
    return {Memory.Address, Memory.AccessSize, true};
  if (!lowAArch64AddressMemoryShapeIsValid(Op))
    return {};
  const auto Instruction = *aarch64AddressMemoryInstruction(
      Op.Inputs[kAddressMemoryWordOperand + 1].Offset);
  const auto &Base = Op.Inputs[kAddressMemoryBaseOperand + 1];
  const auto *Bytes = Image.readVA(Op.Addr, kAddressMemoryWordBytes);
  if (!Base.isReg() || Base.Size != kAddressMemoryPointerBytes ||
      Base.Offset !=
          a64reg::X0 + Instruction.base() * kAddressMemoryPointerBytes ||
      !Bytes || readLE<uint32_t>(Bytes) != Instruction.Word ||
      !aarch64ImmediateMemoryImageSpan(Image, Instruction, OriginalBase,
                                       ConstantAddressProvenance::DataAddress,
                                       Owner))
    return {};
  return {&Base, static_cast<uint16_t>(Instruction.accessBytes()), true};
}

} // namespace neverd

#endif // NEVERD_IR_LOW_AARCH64IMMEDIATEMEMORYIMAGE_H
