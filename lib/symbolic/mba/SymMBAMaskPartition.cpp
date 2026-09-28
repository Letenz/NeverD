//===- SymMBAMaskPartition.cpp - Exact partitioned-mask completion --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Completes bounded sums of complementary XOR masks after candidate selection.
///
//===----------------------------------------------------------------------===//

#include "SymMBADetail.h"

#include <algorithm>

namespace neverd::symbolic {
namespace {

bool isDirectComplement(const SymContext &Ctx, SymRef A, SymRef B) {
  return (Ctx.op(A) == SymOp::Not && Ctx.operand(A, 0) == B) ||
         (Ctx.op(B) == SymOp::Not && Ctx.operand(B, 0) == A);
}

bool isXorComplement(const SymContext &Ctx, SymRef A, SymRef B) {
  if (Ctx.op(A) != SymOp::Xor || Ctx.op(B) != SymOp::Xor)
    return false;
  llvm::ArrayRef<SymRef> ATerms = Ctx.operands(A);
  llvm::ArrayRef<SymRef> BTerms = Ctx.operands(B);
  if (ATerms.size() != BTerms.size() || ATerms.size() > 8)
    return false;
  bool Used[8] = {};
  unsigned Flips = 0;
  for (SymRef Term : ATerms) {
    unsigned Match = BTerms.size();
    for (unsigned J = 0; J < BTerms.size(); ++J)
      if (!Used[J] && BTerms[J] == Term) {
        Match = J;
        break;
      }
    if (Match == BTerms.size()) {
      for (unsigned J = 0; J < BTerms.size(); ++J)
        if (!Used[J] && isDirectComplement(Ctx, Term, BTerms[J])) {
          Match = J;
          ++Flips;
          break;
        }
    }
    if (Match == BTerms.size())
      return false;
    Used[Match] = true;
  }
  return Flips & 1;
}

SymRef negatedOperand(const SymContext &Ctx, SymRef R) {
  return Ctx.op(R) == SymOp::Mul && Ctx.numOperands(R) == 2 &&
                 Ctx.isConstOnes(Ctx.operand(R, 0))
             ? Ctx.operand(R, 1)
             : SymRef();
}

SymRef minusTwoMinusOperand(const SymContext &Ctx, SymRef R) {
  if (Ctx.op(R) != SymOp::Add || Ctx.numOperands(R) != 2 ||
      !Ctx.isConst(Ctx.operand(R, 0)) ||
      Ctx.constValue(Ctx.operand(R, 0)) != -llvm::APInt(Ctx.width(R), 2))
    return SymRef();
  return negatedOperand(Ctx, Ctx.operand(R, 1));
}

bool isMaskComplement(const SymContext &Ctx, SymRef A, SymRef B) {
  if (isXorComplement(Ctx, A, B))
    return true;
  for (unsigned Swap = 0; Swap < 2; ++Swap) {
    SymRef Negated = negatedOperand(Ctx, A);
    SymRef Offset = minusTwoMinusOperand(Ctx, B);
    // If P is the complement of Q, then -P and -2-Q sum to all ones.
    if (Negated.isValid() && Offset.isValid() &&
        isXorComplement(Ctx, Negated, Offset))
      return true;
    std::swap(A, B);
  }
  return false;
}

SymRef sharedPartitionFactor(const SymContext &Ctx, SymRef A, SymRef B) {
  if (Ctx.op(A) != SymOp::And || Ctx.op(B) != SymOp::And ||
      Ctx.numOperands(A) != 2 || Ctx.numOperands(B) != 2)
    return SymRef();
  llvm::ArrayRef<SymRef> AF = Ctx.operands(A);
  llvm::ArrayRef<SymRef> BF = Ctx.operands(B);
  for (unsigned I = 0; I < 2; ++I)
    for (unsigned J = 0; J < 2; ++J)
      if (AF[I] == BF[J] && isMaskComplement(Ctx, AF[1 - I], BF[1 - J]))
        return AF[I];
  return SymRef();
}

} // namespace

SymRef detail::foldPartitionedMaskSum(SymContext &Ctx, SymRef A, SymRef B,
                                      const llvm::APInt &Offset) {
  if (isXorComplement(Ctx, A, B))
    return Ctx.mkConst(Offset - llvm::APInt(Ctx.width(A), 1));
  SymRef Factor = sharedPartitionFactor(Ctx, A, B);
  if (!Factor.isValid())
    return SymRef();
  return Offset.isZero() ? Factor : Ctx.mkAdd(Ctx.mkConst(Offset), Factor);
}

} // namespace neverd::symbolic
