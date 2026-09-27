//===- SymMBAFactor.cpp - Recovering complete shared factors -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SymMBADetail.h"

#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <array>
#include <map>

namespace neverd::symbolic::detail {
namespace {

bool isBitwise(const SymContext &Ctx, SymRef R) {
  SymOp Op = Ctx.op(R);
  return Op == SymOp::And || Op == SymOp::Or || Op == SymOp::Xor ||
         Op == SymOp::Not;
}

// The optional quotient search needs interaction between its terms. A sum of
// shallow, already canonical bitwise terms on disjoint free variables has no
// shared input for that search to eliminate. Keep this eligibility check local:
// arithmetic, opaque nodes, complements, and larger frontiers use the solver.
bool hasIndependentTerms(const SymContext &Ctx, SymRef E) {
  if (Ctx.width(E) == 1 || Ctx.op(E) != SymOp::Add)
    return false;
  std::array<uint32_t, 64> Inputs;
  size_t Count = 0;
  auto AddInput = [&](SymRef R) {
    if (!Ctx.isVar(R) || Count == Inputs.size())
      return false;
    Inputs[Count++] = R.index();
    return true;
  };
  for (SymRef Term : Ctx.operands(E)) {
    if (Ctx.isConst(Term))
      continue;
    if (Ctx.isVar(Term)) {
      if (!AddInput(Term))
        return false;
      continue;
    }
    SymOp Op = Ctx.op(Term);
    if (Op != SymOp::And && Op != SymOp::Or && Op != SymOp::Xor)
      return false;
    for (SymRef Input : Ctx.operands(Term))
      if (!AddInput(Input))
        return false;
  }
  std::sort(Inputs.begin(), Inputs.begin() + Count);
  return std::adjacent_find(Inputs.begin(), Inputs.begin() + Count) ==
         Inputs.begin() + Count;
}

struct FactorUses {
  unsigned Count = 0;
  bool BitwiseQuotient = false;
};

} // namespace

SymRef solveStructuralFactors(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                              WorkBudget &Budget, SolveReport &Rep) {
  // This optional reading only inspects immediate sums and products. Purely
  // arithmetic factoring belongs to the polynomial normalizer. A small fixed
  // frontier also keeps unsuccessful scans independent of shared tail depth.
  constexpr size_t MaxTerms = 64, MaxEdges = 256;
  if (Ctx.op(E) != SymOp::Add || Ctx.numOperands(E) < 3 ||
      Ctx.numOperands(E) > MaxTerms || Budget.exhausted())
    return E;
  unsigned Products = 0;
  bool Bitwise = false;
  size_t Edges = 0;
  for (SymRef Term : Ctx.operands(E)) {
    if (Ctx.op(Term) != SymOp::Mul)
      continue;
    llvm::ArrayRef<SymRef> Factors = Ctx.operands(Term);
    if (Factors.size() > MaxEdges - Edges)
      return E;
    Edges += Factors.size();
    Products += Factors.size() - unsigned(Ctx.isConst(Factors.front())) > 1;
    for (SymRef Factor : Factors)
      Bitwise |= isBitwise(Ctx, Factor);
  }
  if (Products < 2 || !Bitwise)
    return E;

  SymRef Best = E;
  size_t Storage = Opts.MaxTableBytes;
  const size_t WordBytes =
      (size_t(Ctx.width(E)) / 64 + (Ctx.width(E) % 64 != 0)) * sizeof(uint64_t);
  const size_t SlotBytes = 128 + 3 * WordBytes;
  auto Charge = [&](size_t Work, size_t Slots) {
    if (Slots > Storage / SlotBytes || !Budget.consume(Work)) {
      Rep.BudgetExhausted = true;
      return false;
    }
    Storage -= Slots * SlotBytes;
    return true;
  };
  auto ChargeOperands = [&](SymOp Op, llvm::ArrayRef<SymRef> Operands,
                            size_t Limit) {
    size_t Arity = 0;
    for (SymRef R : Operands) {
      const size_t N = Ctx.op(R) == Op ? Ctx.numOperands(R) : 1;
      if (N > Limit - Arity) {
        Rep.BudgetExhausted = true;
        return false;
      }
      Arity += N;
    }
    return Charge(Arity, Arity);
  };
  // Counts, the original term snapshot, and scratch operand buffers are all
  // charged before allocation. Each scan below is bounded by this frontier.
  const size_t Count = Ctx.numOperands(E);
  if (!Charge(Count + Edges, Count + Edges))
    return Best;
  llvm::SmallVector<SymRef, 16> Terms(Ctx.operands(E).begin(),
                                      Ctx.operands(E).end());
  std::map<uint32_t, FactorUses> Uses;
  for (SymRef Term : Terms) {
    if (Ctx.isConst(Term))
      continue;
    if (Ctx.op(Term) != SymOp::Mul) {
      ++Uses[Term.index()].Count;
      continue;
    }
    llvm::ArrayRef<SymRef> Factors = Ctx.operands(Term);
    const unsigned BitwiseFactors =
        std::count_if(Factors.begin(), Factors.end(),
                      [&](SymRef F) { return isBitwise(Ctx, F); });
    SymRef Previous;
    // Canonical products sort nonconstant factors. A repeated factor counts
    // once per term, while removing one copy can leave another in the quotient.
    for (SymRef Factor : Factors) {
      if (Ctx.isConst(Factor) || Factor == Previous)
        continue;
      Previous = Factor;
      auto &Use = Uses[Factor.index()];
      ++Use.Count;
      Use.BitwiseQuotient |= BitwiseFactors > unsigned(isBitwise(Ctx, Factor));
    }
  }
  llvm::SmallVector<uint32_t, 2> Choices;
  for (const auto &[Id, Use] : Uses) {
    if (Use.Count < 2 ||
        (!Use.BitwiseQuotient && Ctx.op(SymRef(Id)) != SymOp::Not))
      continue;
    auto Position =
        std::find_if(Choices.begin(), Choices.end(),
                     [&](uint32_t I) { return Use.Count > Uses.at(I).Count; });
    Choices.insert(Position, Id);
    if (Choices.size() > 2)
      Choices.pop_back();
  }

  for (uint32_t Id : Choices) {
    if (!Charge(Count + Edges, Count + Edges + 4))
      break;
    SymRef Factor(Id);
    llvm::SmallVector<SymRef, 16> Quotients, Rest;
    for (SymRef Term : Terms) {
      if (Term == Factor) {
        Quotients.push_back(Ctx.mkOne(Ctx.width(E)));
        continue;
      }
      if (Ctx.op(Term) != SymOp::Mul) {
        Rest.push_back(Term);
        continue;
      }
      llvm::ArrayRef<SymRef> Factors = Ctx.operands(Term);
      auto Found = std::find(Factors.begin(), Factors.end(), Factor);
      if (Found == Factors.end()) {
        Rest.push_back(Term);
        continue;
      }
      llvm::SmallVector<SymRef, 8> Remaining(Factors.begin(), Factors.end());
      Remaining.erase(Remaining.begin() + (Found - Factors.begin()));
      Quotients.push_back(Ctx.mkMul(Remaining));
    }
    // Every original term is placed once, and exactly one identical factor
    // is removed from each selected term. Distributivity in the original word
    // ring therefore proves E = Factor * sum(Quotients) + sum(Rest), without
    // treating related factors as independent or inferring anything from
    // samples.
    // Removing a factor can expose an arbitrarily wide sum. Account for the
    // actual canonical flattening before any builder copies its operands.
    if (!ChargeOperands(SymOp::Add, Quotients, MaxTerms))
      continue;
    SymRef Quotient = Ctx.mkAdd(Quotients);
    auto Consider = [&](SymRef Value, unsigned NumAtoms) {
      if (!ChargeOperands(SymOp::Mul, {Factor, Value}, MaxEdges))
        return;
      // Multiplication by one can expose the complete factor as an Add.
      // Check that degeneracy before constructing either candidate node. Rest
      // comes from the canonical original Add and has no direct Add child.
      const size_t Added = Ctx.isConst(Value) &&
                                   Ctx.constValue(Value).isOne() &&
                                   Ctx.op(Factor) == SymOp::Add
                               ? Ctx.numOperands(Factor)
                               : 1;
      if (Added > MaxTerms || Rest.size() > MaxTerms - Added) {
        Rep.BudgetExhausted = true;
        return;
      }
      bool Fits = Charge(Rest.size() + Added, Rest.size() + Added);
      if (!Fits)
        return;
      Rest.push_back(Ctx.mkMul(Factor, Value));
      SymRef Candidate = Ctx.mkAdd(Rest);
      Rest.pop_back();
      if (readingCost(Ctx, Candidate) < readingCost(Ctx, Best)) {
        Best = Candidate;
        Rep.NumAtoms = NumAtoms;
        Rep.Outcome = MBAOutcome::Rewritten;
        Rep.Evidence = MBAEvidence::Derivation;
      }
    };
    // Distributivity already proves this candidate. A failed optional quotient
    // search must not discard a strictly better completed factorization.
    Consider(Quotient, 0);
    if (hasIndependentTerms(Ctx, Quotient))
      continue;
    MBAOptions Inner = Opts;
    Inner.MaxTableBytes = Storage;
    SolveReport Local;
    SymRef Reduced = solveOneRegion(Ctx, Quotient, Inner, Budget, Local);
    Rep.BudgetExhausted |= Local.BudgetExhausted;
    if (Reduced == Quotient || Local.Evidence != MBAEvidence::Derivation)
      continue;
    // solveOneRegion independently proved the quotient replacement. Reusing
    // it under this unchanged factor instantiates that exact identity.
    Consider(Reduced, Local.NumAtoms);
    if (Budget.exhausted())
      break;
  }
  return Best;
}

} // namespace neverd::symbolic::detail
