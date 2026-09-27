//===- SymMBAFactor.cpp - Recovering complete shared factors -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SymMBADetail.h"

#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <map>

namespace neverd::symbolic::detail {
namespace {

bool isBitwise(const SymContext &Ctx, SymRef R) {
  SymOp Op = Ctx.op(R);
  return Op == SymOp::And || Op == SymOp::Or || Op == SymOp::Xor ||
         Op == SymOp::Not;
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
    if (Use.Count < 2 || !Use.BitwiseQuotient)
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
    SymRef Quotient = Ctx.mkAdd(Quotients);
    MBAOptions Inner = Opts;
    Inner.MaxTableBytes = Storage;
    SolveReport Local;
    SymRef Reduced = solveOneRegion(Ctx, Quotient, Inner, Budget, Local);
    Rep.BudgetExhausted |= Local.BudgetExhausted;
    if (Reduced == Quotient || Local.Evidence != MBAEvidence::Derivation)
      continue;
    // solveOneRegion independently proved the quotient replacement. Reusing
    // it under this unchanged factor instantiates that exact identity.
    Rest.push_back(Ctx.mkMul(Factor, Reduced));
    SymRef Candidate = Ctx.mkAdd(Rest);
    if (readingCost(Ctx, Candidate) < readingCost(Ctx, Best)) {
      Best = Candidate;
      Rep.NumAtoms = Local.NumAtoms;
      Rep.Outcome = MBAOutcome::Rewritten;
      Rep.Evidence = MBAEvidence::Derivation;
    }
    if (Budget.exhausted())
      break;
  }
  return Best;
}

} // namespace neverd::symbolic::detail
