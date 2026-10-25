//===- AArch64AddressMemory.h - HighIR memory contracts ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validate original memory expressions before source inference or emission.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_AARCH64ADDRESSMEMORY_H
#define NEVERD_IR_HIGH_AARCH64ADDRESSMEMORY_H

#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/intrinsics/AArch64/AArch64AddressMemory.h"

namespace neverd {

inline bool aarch64AddressMemoryHighShapeIsValid(const HighExpr &Expression) {
  if (Expression.Kind != ExprKind::Call ||
      Expression.Operands.size() != kAddressLoadOperandCount ||
      !Expression.IntrinsicOutputs.empty() || Expression.IsIndirectCall ||
      Expression.IndirectTarget ||
      Expression.MemoryOrdering != NdMemoryOrdering::None ||
      Expression.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  const auto Integer = [](const TypeRef &Type) {
    return Type &&
           (Type->Kind == NdTypeKind::Int || Type->Kind == NdTypeKind::Unknown);
  };
  if (!Integer(Expression.Type) &&
      (!Expression.Type || Expression.Type->Kind != NdTypeKind::Ptr))
    return false;
  for (const auto &Operand : Expression.Operands)
    if (!Operand || !Operand->Type ||
        (!Integer(Operand->Type) && Operand->Type->Kind != NdTypeKind::Ptr))
      return false;
  const auto &Word = *Expression.Operands[kAddressMemoryWordOperand];
  if (!Integer(Word.Type) || Word.Kind != ExprKind::Const)
    return false;
  return aarch64AddressMemoryShapeIsValid(
      Expression.IntrinsicId, Expression.Operands.size() + 1,
      Expression.Type->Size,
      Expression.Operands[kAddressMemoryBaseOperand]->Type->Size,
      Expression.Operands[kAddressMemoryIndexOperand]->Type->Size,
      Word.Type->Size, Word.ConstVal,
      Expression.Operands[kAddressMemoryIndexOperand]->Kind == ExprKind::Const
          ? std::optional<uint64_t>(
                Expression.Operands[kAddressMemoryIndexOperand]->ConstVal)
          : std::nullopt);
}

} // namespace neverd

#endif // NEVERD_IR_HIGH_AARCH64ADDRESSMEMORY_H
