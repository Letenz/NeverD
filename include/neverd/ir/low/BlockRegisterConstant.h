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
