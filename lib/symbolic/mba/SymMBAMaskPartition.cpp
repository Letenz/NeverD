//===- SymMBAMaskPartition.cpp - Exact partitioned-mask completion --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Completes sums of complementary XOR masks before measurement or after
/// candidate selection.
///
//===----------------------------------------------------------------------===//

#include "SymMBADetail.h"

#include "llvm/ADT/DenseMap.h"

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
  if (ATerms.size() != BTerms.size())
    return false;
  // XOR construction has removed duplicate and complementary pairs. Match
  // each surviving operand once, independently of its position in the two
  // canonical lists. This also keeps wide parity masks linear in their arity.
  llvm::DenseMap<uint32_t, unsigned> Exact, Negated;
  for (unsigned J = 0; J < BTerms.size(); ++J) {
    Exact[BTerms[J].index()] = J;
    if (Ctx.op(BTerms[J]) == SymOp::Not)
      Negated[Ctx.operand(BTerms[J], 0).index()] = J;
  }
  llvm::SmallVector<uint8_t, 8> Used(BTerms.size(), 0);
  unsigned Flips = 0;
  for (SymRef Term : ATerms) {
    auto Same = Exact.find(Term.index());
    unsigned Match = Same == Exact.end() ? BTerms.size() : Same->second;
    if (Match == BTerms.size()) {
      unsigned Opposite = BTerms.size();
      if (Ctx.op(Term) == SymOp::Not) {
        auto It = Exact.find(Ctx.operand(Term, 0).index());
        if (It != Exact.end())
          Opposite = It->second;
      } else {
        auto It = Negated.find(Term.index());
        if (It != Negated.end())
          Opposite = It->second;
      }
      if (Opposite != BTerms.size()) {
        Match = Opposite;
        ++Flips;
      }
    }
    if (Match == BTerms.size() || Used[Match])
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
  if (isDirectComplement(Ctx, A, B) || isXorComplement(Ctx, A, B))
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
