//===- LowIRLoopAlignment.cpp - Bounded search for paired loop cuts -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/analysis/LowIRRefinement.h"

#include <algorithm>
#include <map>
#include <numeric>
#include <set>

namespace neverd::analysis {
namespace {
using Status = LowIRLoopAlignmentStatus;
struct Stop {};

class AlignmentSearch {
  const LowFunc &Original, &Candidate;
  llvm::ArrayRef<LowIRUndefinedInstruction> OriginalInstructions;
  const LowIRIndependenceContract &Contract;
  LowIRRefinementWitness Witness;
  const LowIRLoopAlignmentLimits &Limits;
  LowIRLoopAlignmentResult Result;
  bool AttemptExhausted = false;
  using BlockMap = std::map<int, const LowBlock *>;

  [[noreturn]] void stop(Status S, llvm::StringRef Message) {
    Result.Status = S;
    Result.Diagnostic = Message.str();
    throw Stop{};
  }

  void charge(uint64_t Count = 1) {
    if (Count > Limits.MaxSearchWork - Result.SearchWork)
      stop(Status::BudgetExceeded, "loop alignment search work exhausted");
    Result.SearchWork += Count;
  }

  void requireQueries() {
    if (Result.SolverQueries == Limits.MaxSolverQueries)
      stop(Status::BudgetExceeded,
           "loop alignment total query budget exhausted");
  }

  uint32_t queryGrant(uint32_t StageLimit) {
    requireQueries();
    const uint64_t Remaining = Limits.MaxSolverQueries - Result.SolverQueries;
    return static_cast<uint32_t>(std::min<uint64_t>(StageLimit, Remaining));
  }

  // Validate identities before any search indexes or traverses successors.
  // Semantic graph validation remains with the existing executor.
  BlockMap index(const LowFunc &F) {
    charge(F.Blocks.size());
    BlockMap Blocks;
    std::set<va_t> Addresses;
    for (const auto &B : F.Blocks)
      if (!Blocks.emplace(B.Id, &B).second ||
          !Addresses.insert(B.StartAddr).second)
        stop(Status::Invalid, "duplicate LowIR block identity");
    if (!Addresses.count(F.Entry))
      stop(Status::Invalid, "missing LowIR entry block");
    for (const auto &B : F.Blocks) {
      charge(B.Succs.size());
      std::set<int> Unique;
      for (int S : B.Succs)
        if (!Blocks.count(S) || !Unique.insert(S).second)
          stop(Status::Invalid, "unknown or duplicate CFG successor");
    }
    return Blocks;
  }

  bool cyclic(const LowBlock &Root, const BlockMap &Blocks) {
    charge();
    std::vector<int> Pending{Root.Id};
    std::set<int> Seen{Root.Id};
    while (!Pending.empty()) {
      charge();
      const auto &B = *Blocks.at(Pending.back());
      Pending.pop_back();
      charge(B.Succs.size());
      for (int S : B.Succs) {
        if (S == Root.Id)
          return true;
        if (Seen.insert(S).second)
          Pending.push_back(S);
      }
    }
    return false;
  }

  LowIRLoopInferenceResult infer(const LowFunc &F,
                                 LowIRLoopInferenceLimits Stage,
                                 llvm::ArrayRef<va_t> Eligible = {}) {
    Stage.Execution.MaxSolverQueries =
        queryGrant(Stage.Execution.MaxSolverQueries);
    auto R = inferLowIRLoopRefinementPlan(F, Contract, Stage, Eligible);
    Result.SolverQueries += R.SolverQueries;
    AttemptExhausted |= R.Status == LowIRLoopInferenceStatus::BudgetExceeded;
    if (R.Status == LowIRLoopInferenceStatus::Invalid)
      stop(Status::Invalid, R.Diagnostic);
    return R;
  }

  // Count before allocating pairings or invoking the authoritative pairer.
  bool metadata(uint64_t Count, uint64_t &Remaining) {
    charge(Count);
    if (Count > Remaining) {
      AttemptExhausted = true;
      Result.LastCandidateDiagnostic =
          "loop alignment metadata budget exhausted";
      return false;
    }
    Remaining -= Count;
    return true;
  }

  bool planMetadata(const LowIRLoopRefinementPlan &Plan, uint64_t &Remaining) {
    if (!metadata(Plan.Cutpoints.size(), Remaining))
      return false;
    for (const auto &C : Plan.Cutpoints)
      for (size_t Count :
           {C.Inputs.size(), C.Expressions.size(), C.OriginalState.size(),
            C.CandidateState.size(), C.Rank.size()})
        if (!metadata(Count, Remaining))
          return false;
    return true;
  }

  bool pairAndCheck(const LowIRLoopRefinementPlan &Left,
                    const LowIRLoopRefinementPlan &Right,
                    llvm::ArrayRef<size_t> Order) {
    if (Result.PairingAttempts >= Limits.MaxPairingAttempts)
      stop(Status::BudgetExceeded, "loop alignment pairing budget exhausted");
    ++Result.PairingAttempts;
    uint64_t Remaining = Limits.MaxMetadata;
    if (!planMetadata(Left, Remaining) || !planMetadata(Right, Remaining) ||
        !metadata(Order.size(), Remaining))
      return false;
    std::vector<LowIRLoopCutpointPair> Pairs;
    for (size_t I = 0; I != Order.size(); ++I) {
      const auto &L = Left.Cutpoints[I], &R = Right.Cutpoints[Order[I]];
      LowIRLoopCutpointPair Pair;
      Pair.OriginalAddress = L.OriginalAddress;
      Pair.CandidateAddress = R.OriginalAddress;
      for (const auto &LI : L.Inputs) {
        charge();
        if (LI.Side != LowIRLoopSide::Original ||
            LI.Location.Space != LowIRLoopSpace::Frame)
          continue;
        for (const auto &RI : R.Inputs) {
          charge();
          if (RI.Side == LowIRLoopSide::Original &&
              RI.Location.Space == LowIRLoopSpace::Frame &&
              LI.Location.Offset == RI.Location.Offset &&
              LI.Location.Bytes == RI.Location.Bytes) {
            if (!metadata(1, Remaining))
              return false;
            Pair.SharedInputs.push_back({LI.Location, RI.Location});
          }
        }
      }
      Pairs.push_back(std::move(Pair));
    }
    auto Plan =
        pairLowIRLoopRefinementPlans(Left, Right, Pairs, Limits.MaxMetadata);
    if (!Plan) {
      Result.LastCandidateDiagnostic = llvm::toString(Plan.takeError());
      return false;
    }
    auto ProofLimits = Limits.Proof;
    ProofLimits.Execution.MaxSolverQueries =
        queryGrant(ProofLimits.Execution.MaxSolverQueries);
    Result.Refinement =
        checkLowIRLoopRefinement(Original, OriginalInstructions, Candidate,
                                 Contract, *Plan, Witness, ProofLimits);
    Result.SolverQueries += Result.Refinement.SolverQueries;
    if (Result.Refinement.proved()) {
      Result.Status = Status::Proved;
      return true;
    }
    Result.LastCandidateDiagnostic = Result.Refinement.Diagnostic;
    if (Result.Refinement.Status == LowIRRefinementStatus::Invalid)
      stop(Status::Invalid, Result.Refinement.Diagnostic);
    AttemptExhausted |=
        Result.Refinement.Status == LowIRRefinementStatus::BudgetExceeded;
    requireQueries();
    return false;
  }

  bool tryCandidate(const LowIRLoopRefinementPlan &Left,
                    llvm::ArrayRef<va_t> Eligible = {}) {
    requireQueries();
    if (Result.CandidateAttempts >= Limits.MaxCandidateAttempts)
      stop(Status::BudgetExceeded, "loop alignment candidate budget exhausted");
    ++Result.CandidateAttempts;
    auto Right = infer(Candidate, Limits.CandidateInference, Eligible);
    if (!Right.inferred()) {
      Result.LastCandidateDiagnostic = Right.Diagnostic;
      requireQueries();
      return false;
    }
    if (Right.Plan->Cutpoints.size() > Limits.MaxCuts) {
      AttemptExhausted = true;
      Result.LastCandidateDiagnostic = "loop alignment cut count exceeded";
      return false;
    }
    if (Right.Plan->Cutpoints.size() != Left.Cutpoints.size()) {
      Result.LastCandidateDiagnostic = "loop alignment cut counts differ";
      return false;
    }
    charge(Left.Cutpoints.size());
    std::vector<size_t> Order(Left.Cutpoints.size());
    std::iota(Order.begin(), Order.end(), 0);
    do {
      if (pairAndCheck(Left, *Right.Plan, Order))
        return true;
      charge(Order.size());
    } while (std::next_permutation(Order.begin(), Order.end()));
    return false;
  }

public:
  AlignmentSearch(const LowFunc &A,
                  llvm::ArrayRef<LowIRUndefinedInstruction> Records,
                  const LowFunc &B, const LowIRIndependenceContract &C,
                  LowIRRefinementWitness W, const LowIRLoopAlignmentLimits &L)
      : Original(A), Candidate(B), OriginalInstructions(Records), Contract(C),
        Witness(W), Limits(L) {}

  LowIRLoopAlignmentResult run() {
    try {
      index(Original);
      const auto Blocks = index(Candidate);
      auto Left = infer(Original, Limits.OriginalInference);
      if (!Left.inferred())
        stop(AttemptExhausted ? Status::BudgetExceeded : Status::Unsupported,
             "original loop inference: " + Left.Diagnostic);
      if (Left.Plan->Cutpoints.size() > Limits.MaxCuts)
        stop(Status::BudgetExceeded, "loop alignment cut count exceeded");
      if (tryCandidate(*Left.Plan))
        return std::move(Result);
      requireQueries();
      for (const auto &B : Candidate.Blocks) {
        charge();
        if (cyclic(B, Blocks) && tryCandidate(*Left.Plan, {B.StartAddr}))
          return std::move(Result);
      }
      stop(AttemptExhausted ? Status::BudgetExceeded : Status::Unsupported,
           AttemptExhausted
               ? "loop alignment search incomplete within stage limits"
               : "no loop relation found in bounded alignment search");
    } catch (const Stop &) {
    }
    return std::move(Result);
  }
};
} // namespace

LowIRLoopAlignmentResult inferAndCheckLowIRLoopRefinement(
    const LowFunc &Original,
    llvm::ArrayRef<LowIRUndefinedInstruction> OriginalInstructions,
    const LowFunc &Candidate, const LowIRIndependenceContract &Contract,
    LowIRRefinementWitness Witness, const LowIRLoopAlignmentLimits &Limits) {
  return AlignmentSearch(Original, OriginalInstructions, Candidate, Contract,
                         Witness, Limits)
      .run();
}
} // namespace neverd::analysis
