//===- AArch64WritebackMemory.h - HighIR writeback contracts ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validate writeback before source inference, filtering or emission.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_AARCH64WRITEBACKMEMORY_H
#define NEVERD_IR_HIGH_AARCH64WRITEBACKMEMORY_H

#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/intrinsics/AArch64/AArch64WritebackMemory.h"

namespace neverd {

inline bool aarch64WritebackMemoryHighShapeIsValid(const HighExpr &Expression) {
  if (Expression.Kind != ExprKind::Call ||
      Expression.Operands.size() !=
          aarch64WritebackMemoryOperandCount(Expression.IntrinsicId) ||
      !Expression.IntrinsicOutputs.empty() || Expression.IsIndirectCall ||
      Expression.IndirectTarget ||
      Expression.MemoryOrdering != NdMemoryOrdering::None ||
      Expression.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  const auto Integer = [](const TypeRef &Type) {
    return Type &&
           (Type->Kind == NdTypeKind::Int || Type->Kind == NdTypeKind::Unknown);
  };
  const bool Store = Expression.IntrinsicId == Intrinsic::A64_StoreWriteback;
  if (!Integer(Expression.Type) &&
      !(Store && Expression.Type && Expression.Type->Kind == NdTypeKind::Ptr))
    return false;
  for (const auto &Operand : Expression.Operands)
    if (!Operand || !Operand->Type ||
        (!Integer(Operand->Type) && Operand->Type->Kind != NdTypeKind::Ptr))
      return false;
  const auto &Word = *Expression.Operands[kWritebackWordOperand];
  if (!Integer(Word.Type) || Word.Kind != ExprKind::Const)
    return false;
  const auto &Data = Expression.Operands.back();
  return aarch64WritebackMemoryShapeIsValid(
      Expression.IntrinsicId, Expression.Operands.size() + 1,
      Expression.Type->Size,
      Expression.Operands[kWritebackBaseOperand]->Type->Size, Word.Type->Size,
      Word.ConstVal, Store ? Data->Type->Size : 0,
      Store && Data->Kind == ExprKind::Const
          ? std::optional<uint64_t>(Data->ConstVal)
          : std::nullopt);
}

} // namespace neverd

#endif // NEVERD_IR_HIGH_AARCH64WRITEBACKMEMORY_H
