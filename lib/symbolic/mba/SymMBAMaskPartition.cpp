//===- SymMBAMaskPartition.cpp - Exact partitioned-mask completion --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Completes sums of complementary bitwise masks before measurement or after
/// candidate selection.
///
//===----------------------------------------------------------------------===//

#include "SymMBADetail.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

#include <algorithm>

namespace neverd::symbolic {

bool detail::isDirectComplement(const SymContext &Ctx, SymRef A, SymRef B) {
  return (Ctx.op(A) == SymOp::Not && Ctx.operand(A, 0) == B) ||
         (Ctx.op(B) == SymOp::Not && Ctx.operand(B, 0) == A);
}

bool detail::isBitwiseComplement(const SymContext &Ctx, SymRef A, SymRef B) {
  if (isDirectComplement(Ctx, A, B))
    return true;
  if (Ctx.op(A) == SymOp::And)
    std::swap(A, B);
  if (Ctx.op(A) != SymOp::Or || Ctx.op(B) != SymOp::And)
    return false;
  llvm::ArrayRef<SymRef> OrTerms = Ctx.operands(A);
  llvm::ArrayRef<SymRef> AndTerms = Ctx.operands(B);
  if (OrTerms.size() != AndTerms.size())
    return false;
  if (OrTerms.size() <= 8)
    return llvm::all_of(OrTerms, [&](SymRef R) {
      return llvm::any_of(
          AndTerms, [&](SymRef S) { return isDirectComplement(Ctx, R, S); });
    });
  llvm::DenseSet<uint32_t> Exact, Negated;
  for (SymRef Term : AndTerms) {
    if (Ctx.op(Term) == SymOp::Not)
      Negated.insert(Ctx.operand(Term, 0).index());
    else
      Exact.insert(Term.index());
  }
  return llvm::all_of(OrTerms, [&](SymRef R) {
    return Ctx.op(R) == SymOp::Not ? Exact.contains(Ctx.operand(R, 0).index())
                                   : Negated.contains(R.index());
  });
}

namespace {

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
  if (detail::isBitwiseComplement(Ctx, A, B) || isXorComplement(Ctx, A, B))
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

SymRef sharedPartitionFactor(SymContext &Ctx, SymRef A, SymRef B, SymOp Op) {
  if (Ctx.op(A) != Op || Ctx.op(B) != Op)
    return SymRef();
  llvm::ArrayRef<SymRef> AF = Ctx.operands(A);
  llvm::ArrayRef<SymRef> BF = Ctx.operands(B);
  if (AF.size() == 2 && BF.size() == 2) {
    for (unsigned I = 0; I < 2; ++I)
      for (unsigned J = 0; J < 2; ++J)
        if (AF[I] == BF[J] && isMaskComplement(Ctx, AF[1 - I], BF[1 - J]))
          return AF[I];
    return SymRef();
  }

  // The outer operator may have flattened one mask. Rebuild only its
  // nonshared operands, then prove that the two residual masks complement.
  const SymOp Dual = Op == SymOp::And ? SymOp::Or : SymOp::And;
  auto ContainsDual = [&](llvm::ArrayRef<SymRef> Terms) {
    return llvm::any_of(Terms, [&](SymRef R) { return Ctx.op(R) == Dual; });
  };
  if (!ContainsDual(AF) && !ContainsDual(BF))
    return SymRef();
  llvm::SmallVector<SymRef, 8> Shared, Left, Right;
  size_t I = 0, J = 0;
  while (I < AF.size() && J < BF.size()) {
    if (AF[I] == BF[J]) {
      Shared.push_back(AF[I]);
      ++I;
      ++J;
    } else if (AF[I] < BF[J]) {
      Left.push_back(AF[I++]);
    } else {
      Right.push_back(BF[J++]);
    }
  }
  Left.append(AF.begin() + I, AF.end());
  Right.append(BF.begin() + J, BF.end());
  if (Shared.empty() || Left.empty() || Right.empty())
    return SymRef();
  SymRef LeftMask = Op == SymOp::And ? Ctx.mkAnd(Left) : Ctx.mkOr(Left);
  SymRef RightMask = Op == SymOp::And ? Ctx.mkAnd(Right) : Ctx.mkOr(Right);
  if (!isMaskComplement(Ctx, LeftMask, RightMask))
    return SymRef();
  return Op == SymOp::And ? Ctx.mkAnd(Shared) : Ctx.mkOr(Shared);
}

} // namespace

SymRef detail::foldPartitionedMaskSum(SymContext &Ctx, SymRef A, SymRef B,
                                      const llvm::APInt &Offset) {
  if (isXorComplement(Ctx, A, B))
    return Ctx.mkConst(Offset - llvm::APInt(Ctx.width(A), 1));
  if (SymRef Factor = sharedPartitionFactor(Ctx, A, B, SymOp::And))
    return Offset.isZero() ? Factor : Ctx.mkAdd(Ctx.mkConst(Offset), Factor);
  if (SymRef Factor = sharedPartitionFactor(Ctx, A, B, SymOp::Or))
    return Ctx.mkAdd(Ctx.mkConst(Offset - llvm::APInt(Ctx.width(A), 1)),
                     Factor);
  return SymRef();
}

} // namespace neverd::symbolic
