//===- AArch64PAuthLoadC.h - Combined load C projection ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The fixed registers and original instruction preserve combined PAC failure,
/// memory ordering and writeback. SP-origin requires an unavailable execution
/// environment and must not be silently changed into a GPR-origin load.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64PAUTHLOADC_H
#define NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64PAUTHLOADC_H

#include "neverd/backend/c/render/AArch64/AArch64PAuthC.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuthLoad.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"

#include <stdexcept>
#include <string>

namespace neverd {

#define NEVERD_A64_PAUTH_LOAD_C_TEXT(Name, Text)                               \
  inline constexpr char Name[] = Text;
#include "neverd/backend/c/render/AArch64/AArch64PAuthLoadC.def"

inline bool aarch64PAuthLoadHighShapeIsValid(const HighExpr &Expression) {
  if (Expression.Kind != ExprKind::Call ||
      Expression.Operands.size() != kPAuthLoadOperandCount ||
      !Expression.IntrinsicOutputs.empty() || Expression.IsIndirectCall ||
      Expression.IndirectTarget || !Expression.Operands[0] ||
      !Expression.Operands[1] ||
      Expression.MemoryOrdering != NdMemoryOrdering::None ||
      Expression.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  const auto Integer = [](const TypeRef &Type) {
    return Type &&
           (Type->Kind == NdTypeKind::Int || Type->Kind == NdTypeKind::Unknown);
  };
  const auto &Base = *Expression.Operands[0];
  const auto &Word = *Expression.Operands[1];
  if (!Integer(Expression.Type) || !Base.Type ||
      (!Integer(Base.Type) && Base.Type->Kind != NdTypeKind::Ptr) ||
      !Integer(Word.Type) || Word.Kind != ExprKind::Const)
    return false;
  return aarch64PAuthLoadShapeIsValid(
      Expression.IntrinsicId, Expression.Operands.size() + 1,
      Expression.Type->Size, Base.Type->Size, Word.Type->Size, Word.ConstVal);
}

inline std::string
aarch64PAuthLoadC(const AArch64PAuthLoadInstruction &Instruction,
                  llvm::StringRef Base) {
  const auto Validated = aarch64PAuthLoadInstruction(Instruction.Word);
  if (!Validated || Validated->Id != Instruction.Id)
    throw std::invalid_argument(kPAuthLoadMalformed);
  if (Instruction.stackBase())
    throw std::invalid_argument(kPAuthLoadSPUnsupported);
  const auto ValueName = aarch64PAuthTemporary(kPAuthLoadCValue, {Base});
  const auto BaseName =
      aarch64PAuthTemporary(kPAuthLoadCBase, {Base, ValueName});
  return llvm::formatv(kPAuthLoadCExpression, ValueName, BaseName, Base,
                       Instruction.scratchWord())
      .str();
}

} // namespace neverd

#endif // NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64PAUTHLOADC_H
