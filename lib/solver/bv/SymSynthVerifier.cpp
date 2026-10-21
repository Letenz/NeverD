//===- SymSynthVerifier.cpp - Prove synthesis candidates -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/solver/SymSynthVerifier.h"

#include "neverd/solver/BitVectorSolver.h"
#include "neverd/solver/Limits.h"
#include "neverd/solver/SatTypes.h"
#include "neverd/solver/Z3Solver.h"
#include "neverd/symbolic/SymExpr.h"
#include "neverd/symbolic/SymSynth.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace neverd::solver {

namespace {

void addSaturating(uint64_t &Total, uint64_t Delta) {
  constexpr uint64_t Max = std::numeric_limits<uint64_t>::max();
  Total = Delta > Max - Total ? Max : Total + Delta;
}

bool isValidRef(const symbolic::SymContext &Ctx, symbolic::SymRef R) {
  return R.isValid() && R.index() < Ctx.numNodes();
}

// Removing an identical addend from both sides is an equivalence over the
// complete modular word, even if that addend contains nonlinear operations.
// Keep this bounded and shallow; no sampled relation or partial coefficient
// match is sufficient. A model of the remaining inequality also refutes the
// original pair, with any cancelled-only inputs assigned arbitrary values.
void cancelCommonAddends(symbolic::SymContext &Ctx, symbolic::SymRef &A,
                         symbolic::SymRef &B) {
  using namespace symbolic;
  auto count = [&](SymRef R) {
    return Ctx.op(R) == SymOp::Add ? Ctx.numOperands(R) : size_t(1);
  };
  if (count(A) > ::neverd::limits::solver::kSymSynthVerifierMaxTerms ||
      count(B) > ::neverd::limits::solver::kSymSynthVerifierMaxTerms)
    return;
  auto terms = [&](SymRef R) {
    llvm::SmallVector<SymRef, 8> Result;
    if (Ctx.op(R) == SymOp::Add)
      Result.append(Ctx.operands(R).begin(), Ctx.operands(R).end());
    else
      Result.push_back(R);
    return Result;
  };
  const auto Left = terms(A), Right = terms(B);
  llvm::DenseSet<uint32_t> Remaining;
  for (SymRef R : Right)
    Remaining.insert(R.index());
  llvm::SmallVector<SymRef, 8> NewLeft, NewRight;
  for (SymRef R : Left)
    if (!Remaining.erase(R.index()))
      NewLeft.push_back(R);
  if (NewLeft.size() == Left.size())
    return;
  for (SymRef R : Right)
    if (Remaining.contains(R.index()))
      NewRight.push_back(R);
  const uint32_t Width = Ctx.width(A);
  A = NewLeft.empty() ? Ctx.mkZero(Width) : Ctx.mkAdd(NewLeft);
  B = NewRight.empty() ? Ctx.mkZero(Width) : Ctx.mkAdd(NewRight);
}

} // namespace

bool proofBackendAvailable(ProofBackend Backend) {
  switch (Backend) {
  case ProofBackend::BuiltIn:
    return true;
  case ProofBackend::Z3:
    return Z3Solver::available();
  }
  return false;
}

SymSynthVerifier::SymSynthVerifier(SolverOptions Options, ProofBackend Backend,
                                   uint32_t TimeoutMs)
    : Options(std::move(Options)), Backend(Backend), TimeoutMs(TimeoutMs) {
  // A refutation without its assignment is not useful to a synthesis caller.
  // Keep model ownership as an invariant even if a generic solver option was
  // copied from a proof-only call site.
  this->Options.BuildModel = true;
}

SymSynthVerifier::~SymSynthVerifier() = default;

void SymSynthVerifier::addStats(const SatStats &Stats) {
  addSaturating(Report.Stats.Conflicts, Stats.Conflicts);
  addSaturating(Report.Stats.Propagations, Stats.Propagations);
  addSaturating(Report.Stats.WatchVisits, Stats.WatchVisits);
}

void SymSynthVerifier::clearRefutation() {
  Report.RejectedCandidate = symbolic::SymRef();
  Report.Counterexample.reset();
}

symbolic::SynthVerification
SymSynthVerifier::operator()(symbolic::SymContext &Ctx,
                             symbolic::SymRef Original,
                             symbolic::SymRef Candidate) {
  addSaturating(Report.Stats.Queries, 1);
  if (ReportContext != &Ctx) {
    // A retained counterexample owns values, but its variable ids and candidate
    // handle still belong to the context that produced them.
    clearRefutation();
    ReportContext = &Ctx;
  }

  EquivResult Result = EquivResult::Invalid;
  std::optional<BitVectorModel> Counterexample;

  if (isValidRef(Ctx, Original) && isValidRef(Ctx, Candidate) &&
      Ctx.width(Original) == Ctx.width(Candidate)) {
    if (Backend == ProofBackend::Z3) {
      if (!Z3 || Z3Context != &Ctx) {
        Z3SolverOptions Z3Options;
        Z3Options.TimeoutMs = TimeoutMs;
        Z3 = std::make_unique<Z3Solver>(Ctx, Z3Options);
        Z3Context = &Ctx;
      }
      switch (Z3->checkDistinct(Original, Candidate)) {
      case SatResult::Unsat:
        Result = EquivResult::Equal;
        break;
      case SatResult::Sat:
        Result = EquivResult::Different;
        Counterexample = Z3->model();
        break;
      case SatResult::Unknown:
        Result = EquivResult::Unknown;
        break;
      case SatResult::Invalid:
        Result = EquivResult::Invalid;
        break;
      }
    } else if (Backend != ProofBackend::BuiltIn) {
      Result = EquivResult::Invalid;
    } else if (Original == Candidate) {
      Result = EquivResult::Equal;
    } else {
      BitVectorSolver Solver(Ctx, Options);
      symbolic::SymRef Left = Original, Right = Candidate;
      cancelCommonAddends(Ctx, Left, Right);
      Solver.assertDistinct(Left, Right);
      switch (Solver.check()) {
      case SatResult::Unsat:
        Result = EquivResult::Equal;
        break;
      case SatResult::Sat:
        Result = EquivResult::Different;
        Counterexample = Solver.model();
        break;
      case SatResult::Unknown:
        Result = EquivResult::Unknown;
        break;
      case SatResult::Invalid:
        Result = EquivResult::Invalid;
        break;
      }
      addStats(Solver.stats());
    }
  }

  switch (Result) {
  case EquivResult::Equal:
    Report.Proof = ProofStatus::Equivalent;
    clearRefutation();
    return symbolic::SynthVerification::Equivalent;
  case EquivResult::Different:
    Report.Proof = ProofStatus::Different;
    if (!Report.Counterexample.has_value()) {
      Report.RejectedCandidate = Candidate;
      Report.Counterexample = std::move(Counterexample);
    }
    return symbolic::SynthVerification::Different;
  case EquivResult::Unknown:
    Report.Proof = ProofStatus::Unknown;
    clearRefutation();
    return symbolic::SynthVerification::Unknown;
  case EquivResult::Invalid:
    Report.Proof = ProofStatus::Invalid;
    clearRefutation();
    // SynthVerification predates typed query errors.  Unknown is its
    // fail-closed compatibility value; Report retains the precise reason.
    return symbolic::SynthVerification::Unknown;
  }

  Report.Proof = ProofStatus::Invalid;
  clearRefutation();
  return symbolic::SynthVerification::Unknown;
}

} // namespace neverd::solver
