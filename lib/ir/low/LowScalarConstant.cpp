//===- LowScalarConstant.cpp - Bounded integer constant evaluation -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/LowScalarConstant.h"

namespace neverd {

std::optional<uint64_t> lowScalarConstantResult(
    const LowOp &Op,
    llvm::function_ref<std::optional<uint64_t>(const NdVar &)> ValueOf) {
  if (!Op.Output.Size || Op.Output.Size > 8 ||
      (!Op.Output.isReg() && !Op.Output.isTemp()) ||
      Op.NumInputs > std::size(Op.Inputs))
    return std::nullopt;
  const auto Mask = [](uint16_t Width) {
    return UINT64_MAX >> ((8 - Width) * 8);
  };
  std::optional<uint64_t> Result;
  if (Op.NumInputs == 1 &&
      (Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT)) {
    const auto Width = Op.Inputs[0].Size;
    if (!Width || Width > Op.Output.Size ||
        (Op.Opcode == NdOp::COPY && Width != Op.Output.Size))
      return std::nullopt;
    if (auto Value = ValueOf(Op.Inputs[0]))
      Result = *Value & Mask(Width);
  } else if (Op.NumInputs == 2 &&
             (Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB ||
              Op.Opcode == NdOp::INT_XOR) &&
             Op.Inputs[0].Size == Op.Output.Size &&
             Op.Inputs[1].Size == Op.Output.Size) {
    if (Op.Opcode == NdOp::INT_XOR && Op.Inputs[0] == Op.Inputs[1] &&
        (Op.Inputs[0].isReg() || Op.Inputs[0].isTemp()))
      Result = 0;
    else {
      const auto Left = ValueOf(Op.Inputs[0]);
      const auto Right = ValueOf(Op.Inputs[1]);
      if (Left && Right)
        Result = Op.Opcode == NdOp::INT_ADD   ? *Left + *Right
                 : Op.Opcode == NdOp::INT_SUB ? *Left - *Right
                                              : *Left ^ *Right;
    }
  }
  return Result ? std::optional<uint64_t>{*Result & Mask(Op.Output.Size)}
                : std::nullopt;
}

} // namespace neverd
