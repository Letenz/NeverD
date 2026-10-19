//===- SymSynthAffine.cpp - Oracle-guided modular affine fitting ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Fit modular affine candidates from oracle samples for full-width proof.
///
//===----------------------------------------------------------------------===//

#include "SymSynthDetail.h"

#include "llvm/ADT/SmallVector.h"

#include <cstddef>
#include <optional>
#include <utility>

namespace neverd::symbolic::synth {

SearchOutcome fitAffine(SymContext &Ctx, const SynthProblem &P,
                        const SynthOptions &Opts, SearchEffort &Effort,
                        Checker &Check,
                        const std::optional<SynthVerifyFn> &Verify,
                        uint64_t &ProofQueries) {
  SearchOutcome Result;
  const size_t Inputs = P.Leaves.size();
  if (Inputs == 0 || P.Target.size() < Inputs + 1 || Opts.MaxCost < 2)
    return Result;

  // buildGrid begins with zero and each unit vector. These values nominate a
  // fit for any expression, including nonlinear ones; they prove nothing.
  const size_t Words = (size_t(P.Width) + 63) / 64;
  // Spend at most an eighth of the remaining search on an unsuccessful fit.
  // Check division before multiplication so caller-controlled widths, grids
  // and input counts cannot overflow the preflight or consume the fallback.
  const size_t Allowance = Effort.remaining() / 8;
  if (Inputs > Allowance / Words ||
      P.Grid.size() > (Allowance / Words - Inputs) / (Inputs + 1))
    return Result;
  const size_t Work = (Inputs + P.Grid.size() * (Inputs + 1)) * Words;
  if (!Effort.spend(Work))
    return Result;

  const llvm::APInt Offset = P.Target.front();
  llvm::SmallVector<llvm::APInt, 8> Coefficients;
  for (size_t I = 0; I < Inputs; ++I) {
    llvm::APInt Coefficient = P.Target[I + 1] - Offset;
    // A signed one-bit input has values 0 and -1, not 0 and +1.
    if (P.Grid[I + 1][I].isAllOnes())
      Coefficient.negate();
    Coefficients.push_back(std::move(Coefficient));
  }

  // Reject carry- and nonlinear-dependent fits before interning or proving a
  // candidate. Arithmetic stays modulo the complete word width, even for
  // noninvertible coefficients and words wider than a host integer.
  for (size_t Row = 0; Row < P.Grid.size(); ++Row) {
    llvm::APInt Value = Offset;
    for (size_t I = 0; I < Inputs; ++I)
      Value += Coefficients[I] * P.Grid[Row][I];
    if (Value != P.Target[Row])
      return Result;
  }

  // Group equal coefficients without division, including even coefficients.
  // This constructs c*(x+y), instead of asking the grammar to discover each
  // repeated multiplication and subsequently factor it.
  llvm::SmallVector<bool, 8> Used(Inputs, false);
  llvm::SmallVector<SymRef, 8> Terms;
  if (!Offset.isZero())
    Terms.push_back(Ctx.mkConst(Offset));
  for (size_t I = 0; I < Inputs; ++I) {
    if (Used[I] || Coefficients[I].isZero())
      continue;
    llvm::SmallVector<SymRef, 8> Group;
    for (size_t J = I; J < Inputs; ++J)
      if (!Used[J] && Coefficients[J] == Coefficients[I]) {
        Used[J] = true;
        Group.push_back(P.Leaves[J]);
      }
    Terms.push_back(Ctx.mkMul(Ctx.mkConst(Coefficients[I]), Ctx.mkAdd(Group)));
  }
  const SymRef Candidate =
      Terms.empty() ? Ctx.mkZero(P.Width) : Ctx.mkAdd(Terms);
  Result = checkFittedCandidate(Ctx, Candidate, P, Opts, Effort, Check, Verify,
                                ProofQueries);
  Result.FromAffine = Result.Candidate.isValid();
  return Result;
}

SearchOutcome fitTypedAffine(SymContext &Ctx, const SynthProblem &P,
                             const SynthOptions &Opts, SearchEffort &Effort,
                             const std::optional<SynthVerifyFn> &Verify,
                             uint64_t &ProofQueries) {
  if (!P.HasNarrowInputs || P.Hidden.empty() || Effort.empty())
    return {};
  // This alternative preserves correlations which independent opaque inputs
  // intentionally forget. Never replace the ordinary grammar problem: it can
  // still be preferable when a typed fit is unavailable or not affine.
  SynthProblem Typed;
  size_t Work = 0;
  const ProblemStatus Status = buildTypedProblem(Ctx, P.Original, Opts, Typed,
                                                 Effort.remaining() / 4, Work);
  if (!Effort.spend(Work) || Status == ProblemStatus::BudgetExhausted) {
    SearchOutcome Result;
    Result.SawBudgetExhausted = true;
    return Result;
  }
  if (Status == ProblemStatus::TooManyLeaves) {
    SearchOutcome Result;
    Result.NumLeaves = static_cast<unsigned>(Typed.Leaves.size());
    return Result;
  }
  if (Status != ProblemStatus::Ready)
    return {};
  Checker Check = makeChecker(Ctx, Typed, Opts);
  return fitAffine(Ctx, Typed, Opts, Effort, Check, Verify, ProofQueries);
}

} // namespace neverd::symbolic::synth
