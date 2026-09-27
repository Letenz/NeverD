//===- SymExprMask.cpp - Local word-mask reasoning -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SymExprMask.h"

#include <map>
#include <vector>

namespace neverd::symbolic {
namespace {

bool hasConstantMask(const SymContext &Ctx, SymRef R) {
  return Ctx.op(R) == SymOp::And && Ctx.isConst(Ctx.operand(R, 0));
}

struct MaskGroup {
  llvm::APInt Mask;
  llvm::SmallVector<size_t, 4> Positions;
  bool Overlaps = false;
};

} // namespace

SymRef SymContext::internMaskedSource(llvm::ArrayRef<SymRef> Source,
                                      const llvm::APInt &Mask) {
  assert(!Source.empty());
  if (Mask.isZero())
    return mkZero(Mask.getBitWidth());
  if (Mask.isAllOnes() && Source.size() == 1)
    return Source[0];
  llvm::SmallVector<SymRef, 8> Factors;
  if (!Mask.isAllOnes())
    Factors.push_back(mkConst(Mask));
  Factors.append(Source.begin(), Source.end());
  return intern(SymOp::And, Mask.getBitWidth(), Factors, 0);
}

bool SymContext::mergeMaskedOperands(llvm::SmallVectorImpl<SymRef> &Terms,
                                     bool RequireDisjoint) {
  unsigned MaskedCount = 0;
  for (SymRef R : Terms)
    if (hasConstantMask(*this, R) && ++MaskedCount == 2)
      break;
  if (MaskedCount < 2)
    return false;

  // AND is variadic, so its non-constant operand list identifies the source.
  // Copy these immediate edges before any builder can grow the operand pool.
  std::map<std::vector<SymRef>, MaskGroup> Groups;
  for (size_t I = 0; I < Terms.size(); ++I) {
    SymRef R = Terms[I];
    if (!hasConstantMask(*this, R))
      continue;
    auto Ops = operands(R);
    std::vector<SymRef> Source(Ops.begin() + 1, Ops.end());
    llvm::APInt Mask = constValue(Ops[0]);
    auto [It, Inserted] =
        Groups.try_emplace(std::move(Source), MaskGroup{Mask, {}, false});
    MaskGroup &Group = It->second;
    if (!Inserted) {
      Group.Overlaps |= !(Group.Mask & Mask).isZero();
      Group.Mask |= Mask;
    }
    Group.Positions.push_back(I);
  }

  bool Changed = false;
  for (const auto &[Source, Group] : Groups) {
    if (Group.Positions.size() < 2 || (RequireDisjoint && Group.Overlaps))
      continue;
    // Calling mkAnd here could recursively re-enter demanded-add rewriting
    // through the source. Reuse its already canonical factors instead.
    Terms[Group.Positions.front()] = internMaskedSource(Source, Group.Mask);
    for (size_t I = 1; I < Group.Positions.size(); ++I)
      Terms[Group.Positions[I]] = SymRef();
    Changed = true;
  }
  if (Changed)
    llvm::erase_if(Terms, [](SymRef R) { return !R.isValid(); });
  return Changed;
}

SymRef SymContext::simplifyLowMaskedAdd(SymRef Sum, const llvm::APInt &Mask) {
  if (op(Sum) != SymOp::Add || !Mask.isMask() || Mask.isAllOnes())
    return Sum;

  auto Ops = operands(Sum);
  llvm::SmallVector<SymRef, 8> Terms(Ops.begin(), Ops.end());
  bool Changed = false;
  for (SymRef &Term : Terms) {
    if (!hasConstantMask(*this, Term))
      continue;
    auto Factors = operands(Term);
    if ((constValue(Factors[0]) & Mask) != Mask)
      continue;
    llvm::SmallVector<SymRef, 8> Source(Factors.begin() + 1, Factors.end());
    Term =
        internMaskedSource(Source, llvm::APInt::getAllOnes(Mask.getBitWidth()));
    Changed = true;
  }
  // Carries into the low prefix depend only on that prefix of each addend.
  // The caller retains Mask; the new sum's high bits need not match Sum.
  return Changed ? mkAdd(Terms) : Sum;
}

std::optional<unsigned> detail::matchingShiftPower(const SymContext &Ctx,
                                                   SymRef Count,
                                                   const llvm::APInt &Factor) {
  if (!Factor.isPowerOf2() || !Ctx.isConst(Count))
    return std::nullopt;
  unsigned K = Factor.logBase2();
  if (Ctx.constValue(Count).getLimitedValue() != K)
    return std::nullopt;
  return K;
}

} // namespace neverd::symbolic
