//===- BlockRegisterConstant.h - Constant registers -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_BLOCKREGISTERCONSTANT_H
#define NEVERD_IR_LOW_BLOCKREGISTERCONSTANT_H

#include "neverd/ir/low/LowIR.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace neverd {

/// Whether \p Op XORs a register or temporary with itself: a machine zero
/// idiom at any width, 128/256/512-bit vector containers included, which
/// reads nothing of its input.  An effect-bearing operation or a slice of
/// another width is no such idiom.
inline bool isLowSelfXor(const LowOp &Op) {
  return Op.Opcode == NdOp::INT_XOR && Op.NumInputs == 2 &&
         Op.MemoryOrdering == NdMemoryOrdering::None &&
         Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
         (Op.Output.isReg() || Op.Output.isTemp()) && Op.Output.Size &&
         (Op.Inputs[0].isReg() || Op.Inputs[0].isTemp()) &&
         Op.Inputs[0] == Op.Inputs[1] && Op.Output.Size == Op.Inputs[0].Size;
}

/// The constant `or r, -1` or `and r, 0` writes whatever the register held
/// (MSVC sets a register to all ones or zero this way): such an op reads
/// nothing of its register input.
inline std::optional<uint64_t> lowAbsorbingConstant(const LowOp &Op) {
  if ((Op.Opcode != NdOp::INT_OR && Op.Opcode != NdOp::INT_AND) ||
      Op.NumInputs != 2 || Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      !(Op.Output.isReg() || Op.Output.isTemp()) || !Op.Output.Size ||
      Op.Output.Size > 8)
    return std::nullopt;
  const uint64_t Mask = Op.Output.Size == 8
                            ? ~uint64_t{0}
                            : (uint64_t{1} << (8 * Op.Output.Size)) - 1;
  for (unsigned I = 0; I < 2; ++I) {
    const NdVar &C = Op.Inputs[I];
    if (!C.isConst())
      continue;
    const uint64_t V = C.Offset & Mask;
    if (Op.Opcode == NdOp::INT_OR && V == Mask)
      return Mask;
    if (Op.Opcode == NdOp::INT_AND && V == 0)
      return 0;
  }
  return std::nullopt;
}

/// The constant register \p Offset of \p Size holds before Ops[End], when
/// the block sets it to one there (`mov w8, #93` writes w8, then extends it
/// into x8).  A write this cannot follow leaves the value unknown.
inline std::optional<uint64_t>
blockRegisterConstant(const std::vector<LowOp> &Ops, size_t End,
                      uint64_t Offset, uint16_t Size) {
  for (size_t K = End; K-- > 0;) {
    const LowOp &Op = Ops[K];
    if (!Op.Output.isReg() || Op.Output.Offset >= Offset + Size ||
        Offset >= Op.Output.Offset + Op.Output.Size)
      continue;
    if (Op.Output.Offset != Offset || Op.Output.Size != Size ||
        Op.NumInputs != 1)
      return std::nullopt;
    if (Op.Opcode == NdOp::COPY && Op.Inputs[0].isConst())
      return Size >= 8
                 ? Op.Inputs[0].Offset
                 : Op.Inputs[0].Offset & ((uint64_t{1} << (8 * Size)) - 1);
    if (Op.Opcode != NdOp::INT_ZEXT || !Op.Inputs[0].isReg() ||
        Op.Inputs[0].Offset != Offset || Op.Inputs[0].Size >= Size)
      return std::nullopt;
    Size = Op.Inputs[0].Size;
  }
  return std::nullopt;
}

} // namespace neverd

#endif // NEVERD_IR_LOW_BLOCKREGISTERCONSTANT_H
