//===- SymMBAMaskedSum.cpp - Weighted literal-mask sums -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Normalize and independently certify weighted literal masks of one word.

#include "SymMBADetail.h"

#include <map>
#include <vector>

namespace neverd::symbolic::detail {
namespace {

class MaskResources {
  WorkBudget &Work;
  size_t Bytes;

public:
  MaskResources(WorkBudget &Work, size_t Bytes) : Work(Work), Bytes(Bytes) {}

  bool charge(size_t Units, size_t Storage = 0) {
    if (Storage > Bytes || !Work.canConsume(Units))
      return false;
    Work.consume(Units);
    Bytes -= Storage;
    return true;
  }

  bool array(size_t Count, size_t ElementBytes) {
    return (!ElementBytes || Count <= Bytes / ElementBytes) &&
           charge(Count, Count * ElementBytes);
  }

  bool product(size_t A, size_t B) {
    return (!A || B <= std::numeric_limits<size_t>::max() / A) && charge(A * B);
  }
};

struct MaskTerm {
  SymRef Original;
  llvm::APInt Coefficient;
  llvm::APInt Mask;
};

struct CoefficientLess {
  bool operator()(const llvm::APInt &A, const llvm::APInt &B) const {
    return A.ult(B);
  }
};

// Read a proposed equality independently of the coefficient-group builder.
// Only sums, constant scaling and literal masking of this exact carrier are
// linear in its individual bits. Other words must cancel by exact identity;
// nothing inside the carrier or those residual words is reinterpreted.
bool proveMaskIdentity(const SymContext &Ctx, SymRef Before, SymRef After,
                       SymRef Carrier, MaskResources &Resources) {
  const unsigned Width = Ctx.width(Carrier);
  const size_t Words = Width / 64 + (Width % 64 != 0);
  const size_t EntryBytes = 128 + 3 * Words * sizeof(uint64_t);
  struct Item {
    SymRef Expr;
    llvm::APInt Scale;
  };
  if (!Resources.array(2, EntryBytes))
    return false;
  std::vector<Item> Work{{Before, llvm::APInt(Width, 1)},
                         {After, llvm::APInt::getAllOnes(Width)}};
  std::vector<MaskTerm> Terms;
  std::map<uint32_t, llvm::APInt> Residuals;
  llvm::APInt Constant(Width, 0);
  auto append = [&](const llvm::APInt &Scale, const llvm::APInt &Mask) {
    if (!Resources.charge(Words, EntryBytes))
      return false;
    Terms.push_back({{}, Scale, Mask});
    return true;
  };
  auto residual = [&](SymRef R, const llvm::APInt &Scale) {
    if (!Resources.charge(Words, EntryBytes))
      return false;
    auto [It, Added] = Residuals.try_emplace(R.index(), Width, 0);
    It->second += Scale;
    return true;
  };
  while (!Work.empty()) {
    Item Current = std::move(Work.back());
    Work.pop_back();
    SymRef R = Current.Expr;
    if (!Resources.charge(1 + Words) || Ctx.width(R) != Width)
      return false;
    if (R == Carrier) {
      if (!append(Current.Scale, llvm::APInt::getAllOnes(Width)))
        return false;
      continue;
    }
    if (Ctx.isConst(R)) {
      if (!Resources.product(Words, Words))
        return false;
      Constant += Current.Scale * Ctx.constValue(R);
      continue;
    }
    auto Ops = Ctx.operands(R);
    if (Ctx.op(R) == SymOp::Add) {
      if (!Resources.array(Ops.size(), EntryBytes))
        return false;
      for (SymRef Operand : Ops)
        Work.push_back({Operand, Current.Scale});
    } else if (Ctx.op(R) == SymOp::Mul && Ctx.isConst(Ops[0])) {
      if (!Resources.product(Words, Words))
        return false;
      const auto Scale = Current.Scale * Ctx.constValue(Ops[0]);
      if (Ops.size() == 2) {
        if (!Resources.charge(0, EntryBytes))
          return false;
        Work.push_back({Ops[1], Scale});
      } else {
        // The canonical builder flattens K * (x * y) into K * x * y.
        // Match all factors exactly; unrelated products remain unsupported.
        if (!Resources.charge(Ops.size()))
          return false;
        if (Ctx.op(Carrier) == SymOp::Mul &&
            Ops.drop_front() == Ctx.operands(Carrier)) {
          if (!append(Scale, llvm::APInt::getAllOnes(Width)))
            return false;
        } else if (!residual(R, Current.Scale)) {
          return false;
        }
      }
    } else if (Ctx.op(R) == SymOp::And && Ops.size() >= 2 &&
               Ctx.isConst(Ops[0])) {
      auto Inputs = Ops.drop_front();
      if (!Resources.charge(Inputs.size()))
        return false;
      const bool SameCarrier =
          (Inputs.size() == 1 && Inputs[0] == Carrier) ||
          (Ctx.op(Carrier) == SymOp::And && Inputs == Ctx.operands(Carrier));
      if (!(SameCarrier ? append(Current.Scale, Ctx.constValue(Ops[0]))
                        : residual(R, Current.Scale)))
        return false;
    } else if (!residual(R, Current.Scale)) {
      return false;
    }
  }
  if (!Constant.isZero())
    return false;
  for (const auto &[Index, Coefficient] : Residuals)
    if (!Resources.charge(Words) || !Coefficient.isZero())
      return false;
  // A word is the sum of its one-bit basis vectors. The certified grammar
  // above distributes over that sum, even when arithmetic carries occur.
  // Check the actual shifted contribution, including its modular overflow,
  // instead of trusting the synthesizer's unshifted coefficient buckets.
  for (unsigned Bit = 0; Bit != Width; ++Bit) {
    llvm::APInt Difference(Width, 0);
    for (const auto &Term : Terms) {
      if (!Resources.charge(1 + Words))
        return false;
      if (Term.Mask[Bit])
        Difference += Term.Coefficient.shl(Bit);
    }
    if (!Difference.isZero())
      return false;
  }
  return true;
}

} // namespace

bool proveMaskedSumIdentity(const SymContext &Ctx, SymRef Before, SymRef After,
                            SymRef Carrier, WorkBudget &Budget,
                            size_t MaxBytes) {
  if (!Before.isValid() || !After.isValid() || !Carrier.isValid())
    return false;
  MaskResources Resources(Budget, MaxBytes);
  return proveMaskIdentity(Ctx, Before, After, Carrier, Resources);
}

SymRef solveWeightedMasks(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                          WorkBudget &Budget, SolveReport &Rep) {
  if (Ctx.op(E) != SymOp::Add || Opts.MaxTableBytes < 4096)
    return E;
  const auto Summands = Ctx.operands(E);
  if (!Budget.canConsume(Summands.size()))
    return E;
  // A repeated first nonliteral operand is necessary for a shared carrier.
  // This allocation-free hint can have collisions; only the exact grouping
  // and proof below authorize a rewrite. Ordinary arithmetic pays no table
  // construction or coefficient work for an inapplicable reading.
  uint64_t Seen = 0;
  bool Plausible = false;
  for (SymRef Term : Summands) {
    if (Ctx.op(Term) == SymOp::Mul && Ctx.numOperands(Term) == 2 &&
        Ctx.isConst(Ctx.operand(Term, 0))) {
      Term = Ctx.operand(Term, 1);
      // Reassociation can put a common sign or scale around an additive group.
      if (Ctx.op(Term) == SymOp::Add) {
        Plausible = true;
        break;
      }
    }
    if (Ctx.op(Term) != SymOp::And || Ctx.numOperands(Term) < 2 ||
        !Ctx.isConst(Ctx.operand(Term, 0)))
      continue;
    const uint64_t Bit = uint64_t(1) << (Ctx.operand(Term, 1).index() % 64);
    if (Seen & Bit) {
      Plausible = true;
      break;
    }
    Seen |= Bit;
  }
  if (!Plausible)
    return E;
  const unsigned Width = Ctx.width(E);
  const size_t Words = Width / 64 + (Width % 64 != 0);
  const size_t EntryBytes = 192 + 4 * Words * sizeof(uint64_t);
  MaskResources Resources(Budget, Opts.MaxTableBytes);
  if (!Resources.array(Summands.size(), EntryBytes))
    return E;
  // Reconstructing a flattened carrier can grow the context's operand pool.
  // Keep no borrowed operand range across any DAG-building call.
  llvm::SmallVector<SymRef, 8> OwnedSummands(Summands.begin(), Summands.end());
  struct Item {
    SymRef Expr;
    llvm::APInt Scale;
  };
  std::vector<Item> Pending;
  for (SymRef Term : OwnedSummands)
    Pending.push_back({Term, llvm::APInt(Width, 1)});
  bool Expanded = false;
  std::map<uint32_t, std::vector<MaskTerm>> Groups;
  llvm::SmallVector<SymRef, 8> Kept;
  while (!Pending.empty()) {
    auto [Body, Scale] = std::move(Pending.back());
    Pending.pop_back();
    if (!Resources.charge(1 + Words))
      return E;
    if (Ctx.op(Body) == SymOp::Add) {
      auto Children = Ctx.operands(Body);
      if (!Resources.array(Children.size(), EntryBytes))
        return E;
      for (SymRef Child : Children)
        Pending.push_back({Child, Scale});
      Expanded = true;
      continue;
    }
    if (Ctx.op(Body) == SymOp::Mul && Ctx.numOperands(Body) == 2 &&
        Ctx.isConst(Ctx.operand(Body, 0))) {
      if (!Resources.product(Words, Words) || !Resources.charge(0, EntryBytes))
        return E;
      Pending.push_back(
          {Ctx.operand(Body, 1), Scale * Ctx.constValue(Ctx.operand(Body, 0))});
      continue;
    }
    const auto Ops = Ctx.operands(Body);
    if (Ctx.op(Body) != SymOp::And || Ops.size() < 2 || !Ctx.isConst(Ops[0])) {
      Kept.push_back(Scale.isOne() ? Body
                                   : Ctx.mkMul(Ctx.mkConst(Scale), Body));
      continue;
    }
    if (!Resources.array(Ops.size(), 2 * sizeof(SymRef)))
      return E;
    auto Mask = Ctx.constValue(Ops[0]);
    llvm::SmallVector<SymRef, 8> Inputs(Ops.begin() + 1, Ops.end());
    SymRef Carrier = Ctx.mkAnd(Inputs);
    SymRef Original =
        Scale.isOne() ? Body : Ctx.mkMul(Ctx.mkConst(Scale), Body);
    Groups[Carrier.index()].push_back({Original, Scale, Mask});
  }
  // Expanded groups need a proof against the actual input, including every
  // enclosing scale. The independent checker certifies one masked word and
  // cancels unchanged residual words; multiple changed words remain bounded
  // alternatives for the normal deep walk.
  if (Expanded && Groups.size() != 1)
    return E;

  bool Changed = false;
  for (const auto &[Index, Terms] : Groups) {
    auto retain = [&] {
      for (const auto &Term : Terms)
        Kept.push_back(Term.Original);
    };
    if (Terms.size() < 2) {
      retain();
      continue;
    }
    // Cover the coefficient vector, worst-case bucket tree, masks and emitted
    // DAG nodes before allocating. Charges remain cumulative across groups.
    if (!Resources.array(Width, 256 + 8 * Words * sizeof(uint64_t)))
      return E;
    std::vector<llvm::APInt> Coefficients(Width, llvm::APInt(Width, 0));
    for (const auto &Term : Terms) {
      for (unsigned Bit = 0; Bit != Width; ++Bit) {
        if (!Resources.charge(1 + Words))
          return E;
        if (Term.Mask[Bit])
          Coefficients[Bit] += Term.Coefficient;
      }
    }
    std::map<llvm::APInt, llvm::APInt, CoefficientLess> Buckets;
    for (unsigned Bit = 0; Bit != Width; ++Bit) {
      if (!Resources.charge(1 + Words))
        return E;
      const auto &K = Coefficients[Bit];
      if (K.shl(Bit).isZero())
        continue;
      auto [It, Added] = Buckets.try_emplace(K, Width, 0);
      It->second.setBit(Bit);
    }
    SymRef Carrier(Index);
    llvm::SmallVector<SymRef, 8> Parts, Originals;
    for (const auto &[K, Mask] : Buckets) {
      // An even coefficient cannot observe its highest input bits. Restoring
      // those bits can eliminate a mask altogether without modular division.
      auto ExpandedMask =
          Mask | llvm::APInt::getHighBitsSet(Width, K.countr_zero());
      Parts.push_back(Ctx.mkMul(Ctx.mkConst(K),
                                Ctx.mkAnd(Carrier, Ctx.mkConst(ExpandedMask))));
    }
    for (const auto &Term : Terms)
      Originals.push_back(Term.Original);
    SymRef Before = Ctx.mkAdd(Originals);
    SymRef Candidate = Parts.empty() ? Ctx.mkZero(Width) : Ctx.mkAdd(Parts);
    if (readingScore(Ctx, Candidate) >= readingScore(Ctx, Before) ||
        (!Expanded &&
         !proveMaskIdentity(Ctx, Before, Candidate, Carrier, Resources))) {
      retain();
      continue;
    }
    Kept.push_back(Candidate);
    Changed = true;
  }
  if (!Changed)
    return E;
  SymRef Answer = Ctx.mkAdd(Kept);
  if (readingScore(Ctx, Answer) >= readingScore(Ctx, E) ||
      (Expanded &&
       !proveMaskIdentity(Ctx, E, Answer, SymRef(Groups.begin()->first),
                          Resources)))
    return E;
  Rep.Outcome = MBAOutcome::Rewritten;
  Rep.Evidence = MBAEvidence::Derivation;
  return Answer;
}

} // namespace neverd::symbolic::detail
