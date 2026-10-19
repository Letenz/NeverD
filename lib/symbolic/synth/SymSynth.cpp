//===- SymSynth.cpp - Oracle-guided expression synthesis ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the entry points described in SymSynth.h: reduce, search, check,
/// put back what was hidden, and decide whether the answer was worth having.
///
/// The order of those last two matters.  The search works over a body whose
/// inputs are independent, and an accepted candidate is a statement about that
/// body — the strongest form of the statement, because independent inputs
/// range over everything the subterms behind them could have been.  Only after
/// the statement is settled are the subterms substituted back, which is an
/// exact rewriting rather than another thing to check.  Whether the result is
/// an improvement is then a question about the expression a reader would see,
/// so it is asked of the concrete form and not of the body: a candidate that
/// mentions one hidden subterm twice is short as a body and long as an answer.
///
/// Terminal enumeration runs first, followed by a bounded affine proposal and
/// the composite grammar search. The sampler runs only on their failure.
/// Affine fitting and sampling do not establish a grammar minimum; the result
/// identifies these paths separately.
///
//===----------------------------------------------------------------------===//

#include "neverd/symbolic/SymSynth.h"

#include "SymSynthDetail.h"

#include "llvm/Support/ErrorHandling.h"

#include <cstddef>
#include <optional>

namespace neverd::symbolic {

using namespace synth;

const char *synthOutcomeName(SynthOutcome Outcome) {
  switch (Outcome) {
  case SynthOutcome::NotApplicable:
    return "not-applicable";
  case SynthOutcome::AlreadyShortest:
    return "already-shortest";
  case SynthOutcome::TooManyInputs:
    return "too-many-inputs";
  case SynthOutcome::BudgetExhausted:
    return "budget-exhausted";
  case SynthOutcome::Counterexample:
    return "counterexample";
  case SynthOutcome::ProofIncomplete:
    return "proof-incomplete";
  case SynthOutcome::Synthesized:
    return "synthesized";
  }
  llvm_unreachable("unhandled synthesis outcome");
}

const char *synthVerificationName(SynthVerification Verification) {
  switch (Verification) {
  case SynthVerification::Equivalent:
    return "equivalent";
  case SynthVerification::Different:
    return "different";
  case SynthVerification::Unknown:
    return "unknown";
  }
  llvm_unreachable("unhandled synthesis verification");
}

const char *synthEvidenceName(SynthEvidence Evidence) {
  switch (Evidence) {
  case SynthEvidence::None:
    return "none";
  case SynthEvidence::Samples:
    return "samples";
  case SynthEvidence::Verifier:
    return "verifier";
  }
  llvm_unreachable("unhandled synthesis evidence");
}

namespace {

SynthResult synthesizeImpl(SymContext &Ctx, SymRef E, const SynthOptions &Opts,
                           std::optional<SynthVerifyFn> Verify) {
  SynthResult Result;
  Result.Expr = E;
  if (!E.isValid())
    return Result;

  Result.SizeBefore = Ctx.readabilityCost(E);
  Result.SizeAfter = Result.SizeBefore;

  SynthProblem Problem;
  const ProblemStatus Status = buildProblem(Ctx, E, Opts, Problem);
  Result.NumLeaves = static_cast<unsigned>(Problem.Leaves.size());
  if (Status != ProblemStatus::Ready &&
      (!Problem.HasNarrowInputs || !Opts.UseAffineFitting ||
       Opts.MaxCost < 2)) {
    if (Status == ProblemStatus::TooManyLeaves)
      Result.Outcome = SynthOutcome::TooManyInputs;
    return Result;
  }
  SearchEffort Effort(Opts.MaxWork);
  SearchOutcome Found;
  bool FromSampler = false;
  if (Status == ProblemStatus::Ready) {
    const OpSemantics Sem(Ctx, Problem.Width);
    Checker Check = makeChecker(Ctx, Problem, Opts);
    Found = enumerateShortest(Ctx, Problem, Opts, Sem, Effort, Check, Verify,
                              Result.ProofQueries);
    if (!Found.Candidate.isValid() && !Found.StopForProof &&
        Opts.UseStochasticFallback) {
      SearchOutcome Sampled = searchStochastically(
          Ctx, Problem, Opts, Sem, Effort, Check, Verify, Result.ProofQueries);
      Sampled.SawRefuted |= Found.SawRefuted;
      Sampled.SawProofIncomplete |= Found.SawProofIncomplete;
      Sampled.StopForProof |= Found.StopForProof;
      Sampled.SawBudgetExhausted |= Found.SawBudgetExhausted;
      Found = Sampled;
      FromSampler = Found.Candidate.isValid();
    }
  } else {
    // A conversion at the root can hide the entire expression from the
    // grammar, while its complete typed semantics still admits an affine fit.
    Found =
        fitTypedAffine(Ctx, Problem, Opts, Effort, Verify, Result.ProofQueries);
  }

  Result.Work = Effort.used();
  if (Found.NumLeaves)
    Result.NumLeaves = Found.NumLeaves;
  if (!Found.Candidate.isValid()) {
    if (Found.StopForProof) {
      Result.Outcome = SynthOutcome::ProofIncomplete;
      Result.Verification = SynthVerification::Unknown;
      return Result;
    }
    // Running out of budget with candidates left unbuilt is the one refusal
    // that says nothing about the expression, so it outranks a refutation,
    // which in turn outranks having searched and found nothing.
    if (Effort.empty() || Found.SawBudgetExhausted)
      Result.Outcome = SynthOutcome::BudgetExhausted;
    else if (Found.SawProofIncomplete) {
      Result.Outcome = SynthOutcome::ProofIncomplete;
      Result.Verification = SynthVerification::Unknown;
    } else if (Found.SawRefuted) {
      Result.Outcome = SynthOutcome::Counterexample;
      Result.Verification = SynthVerification::Different;
    } else if (Status == ProblemStatus::TooManyLeaves ||
               (Status != ProblemStatus::Ready &&
                Result.NumLeaves > Opts.MaxLeaves))
      Result.Outcome = SynthOutcome::TooManyInputs;
    else if (Status == ProblemStatus::Ready)
      Result.Outcome = SynthOutcome::AlreadyShortest;
    return Result;
  }

  // Grammar candidates were proved over independent opaque inputs, so exact
  // substitution restores them. Typed candidates were checked against E itself
  // and contain original inputs, never these newly-created placeholders.
  const SymRef Concrete = Ctx.substitute(Found.Candidate, Problem.Hidden);
  const SymReadability Score = Ctx.readability(Concrete);
  Result.Verification = Found.Evidence == SynthEvidence::Verifier
                            ? SynthVerification::Equivalent
                            : SynthVerification::Unknown;
  if (Concrete == E || (!Opts.AllowGrowth && Score >= Ctx.readability(E))) {
    Result.Outcome = SynthOutcome::AlreadyShortest;
    return Result;
  }

  Result.Expr = Concrete;
  Result.Changed = true;
  Result.SizeAfter = Score.Nodes;
  Result.CandidateCost = Found.Cost;
  Result.Outcome = SynthOutcome::Synthesized;
  Result.Evidence = Found.Evidence;
  Result.FromSampler = FromSampler;
  Result.FromAffine = Found.FromAffine;
  Result.FromQuadratic = Found.FromQuadratic;
  return Result;
}

} // namespace

SynthResult synthesize(SymContext &Ctx, SymRef E, const SynthOptions &Opts,
                       SynthVerifyFn Verify) {
  if (!Verify) {
    SynthResult Result;
    Result.Expr = E;
    if (!E.isValid())
      return Result;
    Result.SizeBefore = Ctx.readabilityCost(E);
    Result.SizeAfter = Result.SizeBefore;
    Result.Outcome = SynthOutcome::ProofIncomplete;
    return Result;
  }
  return synthesizeImpl(Ctx, E, Opts, std::optional<SynthVerifyFn>(Verify));
}

SynthResult discoverSynthesisCandidate(SymContext &Ctx, SymRef E,
                                       const SynthOptions &Opts) {
  return synthesizeImpl(Ctx, E, Opts, std::nullopt);
}

SymRef synthesizeEquivalent(SymContext &, SymRef Input, const SynthOptions &) {
  return Input;
}

SymRef synthesizeEquivalent(SymContext &Ctx, SymRef Input,
                            const SynthOptions &Opts, SynthVerifyFn Verify) {
  return synthesize(Ctx, Input, Opts, Verify).Expr;
}

} // namespace neverd::symbolic
