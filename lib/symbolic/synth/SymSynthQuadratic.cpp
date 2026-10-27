//===- SymSynthQuadratic.cpp - Proved modular quadratic fitting ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recover degree-two candidates from oracle probes, then require proof.
///
//===----------------------------------------------------------------------===//

#include "SymSynthDetail.h"

#include "llvm/ADT/DenseSet.h"

#include <algorithm>
#include <cstddef>
#include <optional>

namespace neverd::symbolic::synth {
namespace {

SymRef buildQuadratic(SymContext &Ctx, const SynthProblem &P,
                      SearchEffort &Fit) {
  const size_t N = P.Leaves.size();
  const size_t Words = (size_t(P.Width) + 63) / 64;
  // Preflight before multiplying or allocating caller-controlled dimensions.
  // Count linear terms, squares, cross products and the constant, even when
  // some fitted coefficients later turn out to be zero.
  if (N == 0 || N >= Fit.remaining() || N > Fit.remaining() / (N + 1))
    return {};
  const size_t Probes = N * (N + 1) / 2;
  const size_t Terms = Probes + N + 1;
  if (Terms > Fit.remaining() / Words ||
      P.Grid.size() > (Fit.remaining() / Words) / Terms ||
      !Fit.spend(P.Grid.size() * Terms * Words))
    return {};

  // Bound the optional DAG walk and extra oracle evaluations. Opaque inputs
  // stay independent full words; typed conversion fitting belongs to the
  // separate affine model and is deliberately not assumed here.
  llvm::DenseSet<uint32_t> Seen;
  llvm::SmallVector<SymRef, 32> Pending{P.Body};
  size_t EvaluationCost = 0;
  while (!Pending.empty()) {
    if (!Fit.spend())
      return {};
    SymRef R = Pending.pop_back_val();
    if (!Seen.insert(R.index()).second)
      continue;
    const auto Ops = Ctx.operands(R);
    const size_t NodeWords = (size_t(Ctx.width(R)) + 63) / 64;
    if (!Fit.spend(NodeWords) || !Fit.spend(Ops.size()))
      return {};
    EvaluationCost += NodeWords;
    Pending.append(Ops.begin(), Ops.end());
  }
  if (Probes > Fit.remaining() / EvaluationCost ||
      !Fit.spend(Probes * EvaluationCost))
    return {};

  SymEvalPlan Plan(Ctx, P.Body);
  SamplePoint Point(N, llvm::APInt(P.Width, 0));
  auto evaluate = [&]() {
    return Plan.evalWith([&](uint32_t Id) -> const llvm::APInt * {
      const auto It =
          std::lower_bound(P.LeafVars.begin(), P.LeafVars.end(), Id);
      return It != P.LeafVars.end() && *It == Id
                 ? &Point[size_t(It - P.LeafVars.begin())]
                 : nullptr;
    });
  };

  const llvm::APInt Offset = P.Target.front();
  llvm::SmallVector<llvm::APInt, 8> Linear, Square, Cross;
  for (size_t I = 0; I < N; ++I) {
    Point[I] = llvm::APInt(P.Width, 2, false, true);
    const llvm::APInt Delta =
        evaluate() - P.Target[I + 1] - P.Target[I + 1] + Offset;
    Point[I].clearAllBits();
    if (Delta[0])
      return {};
    // 2*a = Delta mod 2^w has two solutions, not a modular inverse of two.
    // Pick the low representative and absorb the other top bit into b.
    // The ambiguity contributes 2^(w-1)*x*(x-1), identically zero mod 2^w.
    // For w=1, x*x=x and the whole diagonal is represented by the linear term.
    Square.push_back(Delta.lshr(1));
    Linear.push_back(P.Target[I + 1] - Offset - Square.back());
    // The other representative can eliminate a linear term entirely. Prefer
    // it when that removes a monomial, without dividing by an even value or
    // enumerating all 2^N equivalent coefficient vectors.
    const llvm::APInt High = llvm::APInt::getSignMask(P.Width);
    if (Linear.back() == High && !Square.back().isZero()) {
      Square.back() ^= High;
      Linear.back().clearAllBits();
    }
  }
  for (size_t I = 0; I < N; ++I) {
    Point[I] = llvm::APInt(P.Width, 1);
    for (size_t J = I + 1; J < N; ++J) {
      Point[J] = llvm::APInt(P.Width, 1);
      Cross.push_back(evaluate() - P.Target[I + 1] - P.Target[J + 1] + Offset);
      Point[J].clearAllBits();
    }
    Point[I].clearAllBits();
  }

  // These probes nominate a polynomial for any oracle, including ones which
  // are not polynomials. Reject disagreement on the entire training grid
  // before constructing a candidate; independent checking and proof follow.
  for (size_t Row = 0; Row < P.Grid.size(); ++Row) {
    const SamplePoint &Values = P.Grid[Row];
    llvm::APInt Value = Offset;
    size_t K = 0;
    for (size_t I = 0; I < N; ++I) {
      Value += (Square[I] * Values[I] + Linear[I]) * Values[I];
      for (size_t J = I + 1; J < N; ++J)
        Value += Cross[K++] * Values[I] * Values[J];
    }
    if (Value != P.Target[Row])
      return {};
  }

  llvm::SmallVector<SymRef, 16> Sum;
  if (!Offset.isZero())
    Sum.push_back(Ctx.mkConst(Offset));
  auto addTerm = [&](const llvm::APInt &Coefficient, SymRef A, SymRef B) {
    if (!Coefficient.isZero())
      Sum.push_back(Ctx.mkMul(Ctx.mkConst(Coefficient), Ctx.mkMul(A, B)));
  };
  size_t K = 0;
  for (size_t I = 0; I < N; ++I) {
    addTerm(Linear[I], P.Leaves[I], Ctx.mkOne(P.Width));
    addTerm(Square[I], P.Leaves[I], P.Leaves[I]);
    for (size_t J = I + 1; J < N; ++J)
      addTerm(Cross[K++], P.Leaves[I], P.Leaves[J]);
  }
  SymRef Best = Sum.empty() ? Ctx.mkZero(P.Width) : Ctx.mkAdd(Sum);
  // The exact MBA pass may already have factored the original polynomial.
  // Preserve such shared products when choosing a spelling: asking a SAT
  // backend to rediscover distributivity can dwarf the actual simplification.
  // Try both variable orders, since input ids do not encode a preferred factor.
  if (Terms > Fit.remaining() / 4 || !Fit.spend(4 * Terms))
    return Best;
  auto sharedCost = [&](SymRef Candidate) {
    size_t Cost = 0;
    auto add = [&](SymRef Term) {
      if (Seen.contains(Term.index()))
        Cost += Ctx.readabilityCost(Term);
    };
    if (Ctx.op(Candidate) == SymOp::Add)
      for (SymRef Term : Ctx.operands(Candidate))
        add(Term);
    else
      add(Candidate);
    return Cost;
  };
  size_t Shared = sharedCost(Best);
  for (bool Reverse : {false, true}) {
    llvm::SmallVector<SymRef, 16> Factored;
    if (!Offset.isZero())
      Factored.push_back(Ctx.mkConst(Offset));
    for (size_t Position = 0; Position < N; ++Position) {
      const size_t I = Reverse ? N - Position - 1 : Position;
      llvm::SmallVector<SymRef, 8> Inner;
      if (!Linear[I].isZero())
        Inner.push_back(Ctx.mkConst(Linear[I]));
      auto scaled = [&](const llvm::APInt &Coefficient, SymRef V) {
        if (!Coefficient.isZero())
          Inner.push_back(Ctx.mkMul(Ctx.mkConst(Coefficient), V));
      };
      scaled(Square[I], P.Leaves[I]);
      for (size_t Next = Position + 1; Next < N; ++Next) {
        const size_t J = Reverse ? N - Next - 1 : Next;
        const size_t Lo = std::min(I, J), Hi = std::max(I, J);
        scaled(Cross[Lo * (2 * N - Lo - 1) / 2 + Hi - Lo - 1], P.Leaves[J]);
      }
      if (!Inner.empty())
        Factored.push_back(Ctx.mkMul(P.Leaves[I], Ctx.mkAdd(Inner)));
    }
    const SymRef Candidate =
        Factored.empty() ? Ctx.mkZero(P.Width) : Ctx.mkAdd(Factored);
    const size_t CandidateShared = sharedCost(Candidate);
    if (CandidateShared > Shared &&
        Ctx.readability(Candidate) <= Ctx.readability(Best)) {
      Best = Candidate;
      Shared = CandidateShared;
    }
  }
  return Best;
}

} // namespace

SearchOutcome fitQuadratic(SymContext &Ctx, const SynthProblem &P,
                           const SynthOptions &Opts, SearchEffort &Effort,
                           Checker &Check,
                           const std::optional<SynthVerifyFn> &Verify,
                           uint64_t &ProofQueries) {
  if (P.Leaves.empty() || P.Target.size() < P.Leaves.size() + 1 ||
      Opts.MaxCost < 3)
    return {};
  // An unsuccessful proposal may use only a quarter of the remaining work;
  // enumeration and sampling retain their opportunity. Charge partial walks
  // too, and keep independent verification within the shared request budget.
  SearchEffort Fit(Effort.remaining() / 4);
  const SymRef Candidate = buildQuadratic(Ctx, P, Fit);
  Effort.spend(Fit.used());
  if (!Candidate.isValid())
    return {};
  SearchOutcome Result = checkFittedCandidate(Ctx, Candidate, P, Opts, Effort,
                                              Check, Verify, ProofQueries);
  Result.FromQuadratic = Result.Candidate.isValid();
  return Result;
}

} // namespace neverd::symbolic::synth
