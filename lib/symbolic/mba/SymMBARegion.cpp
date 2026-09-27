//===- SymMBARegion.cpp - Solving one MBA region --------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the region solver: the linear and polynomial readings of one
/// expression, the ranking between them, and the split into groups of summands
/// that share no input before paying for a whole-region measurement.
///
/// Both readings run and the shorter proved answer wins.  Ranking uses a
/// strict comparison, so where two candidates cost the same the one produced
/// first is kept — which makes the order the candidates are appended in part
/// of the result.
///
//===----------------------------------------------------------------------===//

#include "SymMBADetail.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <cassert>
#include <map>
#include <optional>

namespace neverd::symbolic::detail {

namespace {

/// A rewrite, and how many inputs the reading that produced it was over.  The
/// count travels with the candidate because the two readings disagree about
/// it: the linear one counts a product of unknowns as one opaque input, the
/// polynomial one looks inside and counts its factors.  Reporting the larger of
/// the two would describe a reading that lost.
struct Candidate {
  SymRef Expr;
  unsigned NumAtoms = 0;
  /// True only after an exact verifier, separate from candidate synthesis, has
  /// established that \c Expr equals the region it may replace.
  bool Proven = false;
};

/// One reading of an expression: what it looks like over inputs the solver can
/// drive, and what those inputs are.
struct Region {
  Abstraction Abstract;
  llvm::SmallVector<uint32_t, 16> AtomIds;
  llvm::SmallVector<SymRef, 16> Atoms;
};

bool isMinimalVariableSum(const SymContext &Ctx, SymRef E,
                          const SolverLimits &Limits) {
  if (Ctx.op(E) != SymOp::Add || Ctx.width(E) == 1)
    return false;
  llvm::ArrayRef<SymRef> Terms = Ctx.operands(E);
  if (Terms.size() > Limits.MaxAtoms)
    return false;
  // Canonical sums contain each variable at most once. Every input affects
  // the result, so an expression needs all these leaves and at least one
  // operation: this sum already meets that reading-cost lower bound.
  return std::all_of(Terms.begin(), Terms.end(),
                     [&](SymRef Term) { return Ctx.op(Term) == SymOp::Var; });
}

std::optional<Region> readRegion(SymContext &Ctx, SymRef E,
                                 const MBAOptions &Opts, bool AllowProducts,
                                 WorkBudget &Budget, SolveReport &Rep) {
  std::optional<Abstraction> Abstract =
      abstractToMBA(Ctx, E, AllowProducts, &Budget, Opts.MaxTableBytes);
  if (!Abstract)
    return std::nullopt;

  Region Out;
  Out.Abstract = std::move(*Abstract);
  Ctx.collectVars(Out.Abstract.Body, Out.AtomIds);
  const unsigned MaxAtoms = resolveLimits(Opts).MaxAtoms;
  const bool TooWide =
      Out.AtomIds.size() > MaxAtoms || !cornerCount(Out.AtomIds.size());
  if (TooWide)
    Rep.TooWide = true;
  if (TooWide || (Out.AtomIds.empty() && !Ctx.isConst(Out.Abstract.Body)))
    return std::nullopt;
  for (uint32_t Id : Out.AtomIds)
    Out.Atoms.push_back(Ctx.varRef(Id));
  return Out;
}

/// Best form of \p E as a single region.
///
/// Two readings of the same expression are tried.  The linear one treats a
/// product of unknowns as opaque and measures everything around it; the
/// polynomial one keeps the product and expands it.  Neither subsumes the
/// other — the linear reading handles inputs the expansion cannot see inside
/// of, and the expansion handles products the measurement cannot read — so
/// both run and the shorter answer wins.
SymRef solveRegion(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                   WorkBudget &Budget, SolveReport &Rep) {
  const SolverLimits Limits = resolveLimits(Opts);
  if (isMinimalVariableSum(Ctx, E, Limits)) {
    Rep.Outcome = MBAOutcome::AlreadyShortest;
    return E;
  }
  llvm::SmallVector<Candidate, 6> Candidates;
  bool Measured = false;

  if (std::optional<Region> Linear =
          readRegion(Ctx, E, Opts, /*AllowProducts=*/false, Budget, Rep)) {
    Measured = true;
    auto NumAtoms = static_cast<unsigned>(Linear->AtomIds.size());
    std::optional<size_t> Corners = cornerCount(Linear->AtomIds.size());
    if (!Corners || !Budget.consume(*Corners)) {
      Rep.BudgetExhausted = true;
    } else {
      llvm::SmallVector<SymRef, 4> Forms;
      // Exact abstraction can eliminate every input. The literal is already
      // the candidate; bitwise synthesis requires at least one atom. Keep the
      // one-corner budget charge and the same proof and selection gates below.
      if (Linear->AtomIds.empty())
        Forms.push_back(Linear->Abstract.Body);
      else
        linearCandidates(Ctx,
                         measure(Ctx, Linear->Abstract.Body, Linear->AtomIds),
                         Linear->Atoms, termBudget(Ctx, E, Opts), Limits, Forms);
      for (SymRef Form : Forms) {
        // Prove the identity over independent inputs before restoring their
        // sources. Restoration may combine coefficients and erase the shared
        // input spelling, but instantiating a proved identity remains exact.
        if (!proveLinearIdentity(Ctx, Linear->Abstract.Body, Form,
                                 Limits.MaxAtoms, Budget, Opts.MaxTableBytes))
          continue;
        SymRef Rewritten = Linear->Abstract.Hidden.empty()
                               ? Form
                               : Ctx.substitute(Form, Linear->Abstract.Hidden);
        Candidates.push_back({Rewritten, NumAtoms, true});
      }
    }
  }

  if (std::optional<Region> Poly =
          readRegion(Ctx, E, Opts, /*AllowProducts=*/true, Budget, Rep)) {
    Measured = true;
    if (std::optional<SymRef> Form =
            solvePolynomial(Ctx, Poly->Abstract.Body, Poly->AtomIds,
                            Poly->Atoms, Opts, Budget)) {
      if (provePolynomialIdentity(Ctx, Poly->Abstract.Body, *Form,
                                  Limits.MaxAtoms, Budget,
                                  Opts.MaxTableBytes)) {
        SymRef Rewritten = Poly->Abstract.Hidden.empty()
                               ? *Form
                               : Ctx.substitute(*Form, Poly->Abstract.Hidden);
        Candidates.push_back(
            {Rewritten, static_cast<unsigned>(Poly->AtomIds.size()), true});
      }
    }
  }
  Rep.BudgetExhausted |= Budget.exhausted();

  // Reaching a measurement at all is the difference between "there is nothing
  // here of the kind I read" and "I read it and it is already as short as it
  // gets".  Being refused for width is a third answer again, and the only one a
  // larger budget would change.
  if (Rep.Outcome == MBAOutcome::NotApplicable) {
    if (Rep.BudgetExhausted)
      Rep.Outcome = MBAOutcome::BudgetExhausted;
    else if (Measured)
      Rep.Outcome = MBAOutcome::AlreadyShortest;
    else if (Rep.TooWide)
      Rep.Outcome = MBAOutcome::TooManyInputs;
  }

  const Candidate *Best = nullptr;
  size_t BestCost = 0;
  for (const Candidate &C : Candidates) {
    if (!C.Proven)
      continue;
    size_t Cost = readingCost(Ctx, C.Expr);
    if (!Best || Cost < BestCost) {
      Best = &C;
      BestCost = Cost;
    }
  }
  if (!Best || Best->Expr == E)
    return E;

  assert(Best->Proven && "an unproved MBA candidate reached selection");
  bool Verified = agreeOnSamples(Ctx, E, Best->Expr, Opts.VerifySamples);
  assert(Verified &&
         "an MBA rewrite disagreed with the expression it replaces");
  if (!Verified)
    return E;

  if (!Opts.AllowGrowth && BestCost > readingCost(Ctx, E))
    return E;

  Rep.NumAtoms = Best->NumAtoms;
  Rep.Outcome = MBAOutcome::Rewritten;
  Rep.Evidence = MBAEvidence::Derivation;
  return Best->Expr;
}

//===----------------------------------------------------------------------===//
// Independent summand groups
//===----------------------------------------------------------------------===//

/// Solve independent summand groups before attempting a whole measurement.
///
/// The 2^t cost of a measurement is real arithmetic, not a tunable: it is how
/// many corners the weights are read off.  But it bounds a single measurement,
/// not an expression.  Two summands of a linear MBA that share no input are
/// independent, so a sum of them is several separate linear MBAs written next
/// to each other, and each can be measured over its own few inputs.  The price
/// is then set by the largest group instead of by the total, which puts an
/// expression over any number of inputs back in reach so long as they were
/// tangled a few at a time -- and tangling them all at once is something the
/// obfuscator cannot afford either, for the same 2^t reason.
///
/// Exactness comes from the independence: no input crosses a group boundary, so
/// summing the solved groups reproduces the original term for term.  The sample
/// check at the end guards against a slip in that reasoning rather than being
/// the reason to believe it.
SymRef solveIndependentTerms(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                             WorkBudget &Budget, SolveReport &Rep) {
  if (Ctx.op(E) != SymOp::Add ||
      isMinimalVariableSum(Ctx, E, resolveLimits(Opts)))
    return E;

  std::optional<Abstraction> Abstract = abstractToMBA(
      Ctx, E, /*AllowProducts=*/false, &Budget, Opts.MaxTableBytes);
  if (!Abstract)
    return E;

  llvm::SmallVector<uint32_t, 32> AllAtoms;
  Ctx.collectVars(Abstract->Body, AllAtoms);
  if (AllAtoms.size() < 2)
    return E;

  // The cut is between summands, so there has to be a sum to cut.
  if (Ctx.op(Abstract->Body) != SymOp::Add)
    return E;
  llvm::ArrayRef<SymRef> Terms = Ctx.operands(Abstract->Body);

  // Two inputs share a group when one term mentions both; closing the relation
  // up means a chain of terms puts their inputs in one group too.
  llvm::DenseMap<uint32_t, uint32_t> Parent;
  for (uint32_t Id : AllAtoms)
    Parent[Id] = Id;
  auto find = [&Parent](uint32_t X) {
    while (Parent[X] != X) {
      Parent[X] = Parent[Parent[X]];
      X = Parent[X];
    }
    return X;
  };

  llvm::SmallVector<llvm::SmallVector<uint32_t, 4>, 16> PerTerm(Terms.size());
  for (size_t I = 0; I < Terms.size(); ++I) {
    Ctx.collectVars(Terms[I], PerTerm[I]);
    for (size_t J = 1; J < PerTerm[I].size(); ++J) {
      uint32_t A = find(PerTerm[I][0]);
      uint32_t B = find(PerTerm[I][J]);
      if (A != B)
        Parent[A] = B;
    }
  }

  // Keyed by group representative so the rebuilt sum comes out the same way on
  // every run; a term naming no input is a constant and joins no group.
  std::map<uint32_t, llvm::SmallVector<SymRef, 8>> Groups;
  llvm::SmallVector<SymRef, 4> Constants;
  for (size_t I = 0; I < Terms.size(); ++I) {
    if (PerTerm[I].empty()) {
      Constants.push_back(Terms[I]);
      continue;
    }
    Groups[find(PerTerm[I][0])].push_back(Terms[I]);
  }
  // A single group means the inputs really are all tangled together, so the
  // width belongs to the expression rather than to how it was written.
  if (Groups.size() < 2)
    return E;

  llvm::SmallVector<SymRef, 16> Parts;
  llvm::SmallVector<size_t, 4> OffsetGroups;
  const bool HasOffset = Constants.size() == 1 &&
                         Ctx.isConst(Constants.front()) &&
                         !Ctx.isConstZero(Constants.front());
  unsigned Widest = 0;
  bool AnySolved = false;
  for (const auto &Group : Groups) {
    llvm::ArrayRef<SymRef> GroupTerms = Group.second;
    // A shared offset can expose a complement in one nontrivial region.
    // Keep a fixed number of alternatives, not every partition of that offset.
    if (HasOffset && GroupTerms.size() > 1 && OffsetGroups.size() < 4)
      OffsetGroups.push_back(Parts.size());
    SymRef Part =
        GroupTerms.size() == 1 ? GroupTerms[0] : Ctx.mkAdd(GroupTerms);
    SolveReport PartRep;
    SymRef Solved = solveRegion(Ctx, Part, Opts, Budget, PartRep);
    Rep.BudgetExhausted |= PartRep.BudgetExhausted;
    if (Solved != Part) {
      AnySolved = true;
      Widest = std::max(Widest, PartRep.NumAtoms);
    }
    Parts.push_back(Solved);
  }
  if (!AnySolved && OffsetGroups.empty())
    return E;

  Parts.append(Constants.begin(), Constants.end());
  auto restoreParts = [&]() {
    SymRef R = Ctx.mkAdd(Parts);
    return Abstract->Hidden.empty() ? R : Ctx.substitute(R, Abstract->Hidden);
  };
  SymRef Rebuilt = AnySolved ? restoreParts() : E;
  if (!OffsetGroups.empty() && !Budget.exhausted()) {
    const size_t InputCost = readingCost(Ctx, E);
    size_t BestCost = readingCost(Ctx, Rebuilt);
    const size_t RestoreWork = Ctx.dagSize(E);
    // Bound the temporary sum edges separately from the solver's tables.
    const bool Fits = Parts.size() <= Opts.MaxTableBytes / (2 * sizeof(SymRef));
    if (!Fits)
      Rep.BudgetExhausted = true;
    for (size_t I : OffsetGroups) {
      if (!Fits || Budget.exhausted())
        break;
      if (!Budget.consume(Parts.size()) || !Budget.consume(RestoreWork)) {
        Rep.BudgetExhausted = true;
        break;
      }
      SymRef Previous = Parts[I];
      SymRef WithOffset = Ctx.mkAdd(Previous, Constants.front());
      SolveReport OffsetRep;
      SymRef Solved = solveRegion(Ctx, WithOffset, Opts, Budget, OffsetRep);
      Rep.BudgetExhausted |= OffsetRep.BudgetExhausted;
      if (Solved == WithOffset)
        continue;

      // The offset belongs to this group exactly once. Every other group
      // keeps its independently proved value, including on later attempts.
      Parts[I] = Solved;
      Parts.pop_back();
      SymRef Candidate = restoreParts();
      Parts.push_back(Constants.front());
      Parts[I] = Previous;
      size_t Cost = readingCost(Ctx, Candidate);
      if (Cost < InputCost && Cost < BestCost) {
        Rebuilt = Candidate;
        BestCost = Cost;
        Widest = std::max(Widest, OffsetRep.NumAtoms);
      }
    }
  }
  if (Rebuilt == E)
    return E;

  bool Verified = agreeOnSamples(Ctx, E, Rebuilt, Opts.VerifySamples);
  assert(Verified && "a split MBA rewrite disagreed with what it replaces");
  if (!Verified)
    return E;

  if (!Opts.AllowGrowth && readingCost(Ctx, Rebuilt) > readingCost(Ctx, E))
    return E;

  Rep.NumAtoms = Widest;
  Rep.Outcome = MBAOutcome::Rewritten;
  Rep.Evidence = MBAEvidence::Derivation;
  return Rebuilt;
}

} // namespace

SymRef solveOneRegion(SymContext &Ctx, SymRef E, const MBAOptions &Opts,
                      WorkBudget &Budget, SolveReport &Rep) {
  SolveReport ArithmeticRep;
  SymRef Arithmetic = solveArithmetic(Ctx, E, Opts, Budget, ArithmeticRep);
  if (Budget.exhausted() || Ctx.isConst(Arithmetic) ||
      (Arithmetic != E && Ctx.op(Arithmetic) == SymOp::Var)) {
    Rep = ArithmeticRep;
    return Arithmetic;
  }

  // Factoring can hide a bitwise product identity inside a new sum. Measure
  // the original spelling too, then rank both exact readings.
  SymRef Split = solveIndependentTerms(Ctx, E, Opts, Budget, Rep);

  // Local rewrites can expose a cheaper shared form after placeholders are
  // restored. Keep that whole-region opportunity, while sums of distinct
  // variables stop at their proven minimum instead of enumerating 2^n corners.
  SolveReport Refined;
  SymRef Solved = solveRegion(Ctx, Split, Opts, Budget, Refined);
  if (Split == E)
    Rep = Refined;
  Rep.BudgetExhausted |= Refined.BudgetExhausted;
  Rep.TooWide |= Refined.TooWide;
  Rep.NumAtoms = std::max(Rep.NumAtoms, Refined.NumAtoms);
  if (Arithmetic != E) {
    if (readingCost(Ctx, Arithmetic) <= readingCost(Ctx, Solved)) {
      Solved = Arithmetic;
      Rep.NumAtoms = ArithmeticRep.NumAtoms;
    }
    Rep.Outcome = MBAOutcome::Rewritten;
    Rep.Evidence = MBAEvidence::Derivation;
  }
  Rep.BudgetExhausted |= ArithmeticRep.BudgetExhausted;
  if (Solved == E && Rep.BudgetExhausted)
    Rep.Outcome = MBAOutcome::BudgetExhausted;
  if (Solved != E && !Budget.exhausted()) {
    // Restoring hidden affine inputs can expose products of constants and
    // sums. Cancel their modular coefficients before publishing the spelling.
    SolveReport Restored;
    SymRef Normalized = solveArithmetic(Ctx, Solved, Opts, Budget, Restored);
    Rep.BudgetExhausted |= Restored.BudgetExhausted;
    if (Normalized != Solved) {
      Solved = Normalized;
      Rep.NumAtoms = std::max(Rep.NumAtoms, Restored.NumAtoms);
    }
  }
  if (Solved != E && !Budget.exhausted() &&
      (Ctx.op(Solved) == SymOp::Add || Ctx.op(Solved) == SymOp::Mul) &&
      readingCost(Ctx, Solved) < readingCost(Ctx, E) &&
      !isMinimalVariableSum(Ctx, Solved, resolveLimits(Opts))) {
    // A sum of scaled opaque inputs has no bitwise relation left to read.
    // Canonical multiplication puts its one combined coefficient first.
    auto unscaled = [&](SymRef R) {
      return Ctx.op(R) == SymOp::Mul && Ctx.numOperands(R) == 2 &&
                     Ctx.isConst(Ctx.operand(R, 0))
                 ? Ctx.operand(R, 1)
                 : R;
    };
    auto isBitwise = [&](SymRef R) {
      SymOp Op = Ctx.op(unscaled(R));
      return Op == SymOp::And || Op == SymOp::Or || Op == SymOp::Xor ||
             Op == SymOp::Not;
    };
    SymRef Body = unscaled(Solved);
    bool HasRelation = isBitwise(Body);
    if (!HasRelation && Ctx.op(Body) == SymOp::Add) {
      for (SymRef Term : Ctx.operands(Body)) {
        if (!Budget.consume())
          break;
        if (isBitwise(Term)) {
          HasRelation = true;
          break;
        }
      }
    }
    Rep.BudgetExhausted |= Budget.exhausted();
    if (!HasRelation || Budget.exhausted())
      return Solved;
    // Restoring inputs can expose a new linear relation after the original
    // reading. Give that result one more exact reading with the same budget;
    // it is not in the deep walk's original postorder. Requiring a strict
    // decrease keeps equal-cost spellings stable, even in growth mode.
    SolveReport Restored;
    SymRef Refined = solveRegion(Ctx, Solved, Opts, Budget, Restored);
    Rep.BudgetExhausted |= Restored.BudgetExhausted;
    if (readingCost(Ctx, Refined) < readingCost(Ctx, Solved)) {
      Solved = Refined;
      Rep.NumAtoms = std::max(Rep.NumAtoms, Restored.NumAtoms);
      Rep.Outcome = MBAOutcome::Rewritten;
      Rep.Evidence = MBAEvidence::Derivation;
    }
  }
  return Solved;
}

} // namespace neverd::symbolic::detail
