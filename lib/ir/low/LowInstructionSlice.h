//===- LowInstructionSlice.h - Checked operation ownership ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_LOWINSTRUCTIONSLICE_H
#define NEVERD_IR_LOW_LOWINSTRUCTIONSLICE_H

#include "neverd/ir/low/LowIR.h"

#include "llvm/ADT/ArrayRef.h"

#include <optional>

namespace neverd::detail {

/// Check only the supplied operation slice. Complete block coverage, control
/// semantics and instruction modes require validateLowInstructionBoundaries.
inline std::optional<llvm::ArrayRef<LowOp>>
checkedInstructionOperations(const LowBlock &Block,
                             const LowInstructionBoundary &Instruction) {
  if (Instruction.FirstOp > Block.Ops.size() ||
      Instruction.OpCount > Block.Ops.size() - Instruction.FirstOp)
    return std::nullopt;
  const auto Ops = llvm::ArrayRef<LowOp>(Block.Ops).slice(Instruction.FirstOp,
                                                          Instruction.OpCount);
  for (const auto &Op : Ops)
    if (Op.Addr != Instruction.Address)
      return std::nullopt;
  return Ops;
}

} // namespace neverd::detail

#endif
