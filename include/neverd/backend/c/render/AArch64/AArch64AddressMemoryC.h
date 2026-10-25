//===- AArch64AddressMemoryC.h - Original memory C contract -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Both C projections retain the same original memory instruction and reject
/// SP-origin execution without its alignment and tagging environment.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64ADDRESSMEMORYC_H
#define NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64ADDRESSMEMORYC_H

#include "neverd/backend/c/render/AArch64/AArch64PAuthC.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/intrinsics/AArch64/AArch64AddressMemory.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"

#include <stdexcept>
#include <string>

namespace neverd {

#define NEVERD_A64_ADDRESS_MEMORY_C_TEXT(Name, Text)                           \
  inline constexpr char Name[] = Text;
#include "neverd/backend/c/render/AArch64/AArch64AddressMemoryC.def"

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

inline std::string
aarch64AddressMemoryC(const AArch64AddressMemoryInstruction &Instruction,
                      llvm::StringRef Base, llvm::StringRef Index) {
  if (!aarch64AddressMemoryInstruction(Instruction.Word))
    throw std::invalid_argument(kAddressMemoryMalformed);
  if (Instruction.stackBase())
    throw std::invalid_argument(kAddressMemorySPUnsupported);
  const auto ValueName =
      aarch64PAuthTemporary(kAddressLoadCValue, {Base, Index});
  const auto BaseName =
      aarch64PAuthTemporary(kAddressLoadCBase, {Base, Index, ValueName});
  const auto IndexName = aarch64PAuthTemporary(
      kAddressLoadCIndex, {Base, Index, ValueName, BaseName});
  const auto BaseInput = aarch64PAuthTemporary(
      kAddressLoadCBaseInput, {Base, Index, ValueName, BaseName, IndexName});
  const auto IndexInput = aarch64PAuthTemporary(
      kAddressLoadCIndexInput,
      {Base, Index, ValueName, BaseName, IndexName, BaseInput});
  return llvm::formatv(kAddressLoadCExpression, ValueName, BaseName, IndexName,
                       Base, Index, Instruction.scratchWord(), BaseInput,
                       IndexInput)
      .str();
}

} // namespace neverd

#endif // NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64ADDRESSMEMORYC_H
