//===- LowIRRefinement.h - Selected-value program refinement ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_LOWIRREFINEMENT_H
#define NEVERD_ANALYSIS_LOWIRREFINEMENT_H

#include "neverd/analysis/LowIRUndefinedIndependence.h"

namespace neverd::analysis {

/// An explicit, constructive choice at each original undefined producer.
/// Neither policy describes the undefined-bit choice of a physical processor.
enum class LowIRRefinementWitness : uint8_t {
  /// Select the bits already computed by ordinary deterministic lifting.
  /// An unbound instruction temporary cannot supply this witness.
  LiftedBits,
  /// Select zero only for the active undefined bits; retain every other bit.
  ZeroBits,
};

struct LowIRRefinementLimits {
  /// Shared execution/query/observation budgets across both programs and the
  /// final relation. Input graph metadata limits apply to each graph.
  LowIRIndependenceLimits Execution;
  uint64_t MaxTerminalPairs = 65536;
};

enum class LowIRRefinementStatus : uint8_t {
  Proved,
  /// The supplied witness and, if present, loop plan do not establish the
  /// requested relation. A rejected plan need not imply differing outputs.
  /// This does not disprove refinement under a different witness.
  Different,
  Unsupported,
  Invalid,
  BudgetExceeded,
  InfeasibleEntry,
  ContractViolation,
};

enum class LowIRRefinementScope : uint8_t {
  CompleteFiniteLowIRPaths,
  CompleteFiniteNativeToLowIRPaths,
  InductiveLowIRLoops,
  InductiveNativeToLowIRLoops,
};

enum class LowIRLoopSpace : uint8_t { Register, Frame, SystemFlags };
enum class LowIRLoopSide : uint8_t { Entry, Original, Candidate };

struct LowIRLoopLocation {
  LowIRLoopSpace Space = LowIRLoopSpace::Register;
  /// Register byte offset; for Frame, the two's-complement entry-root offset.
  /// SystemFlags requires Offset == 0 and Bytes == 8 and a native profile.
  uint64_t Offset = 0;
  uint16_t Bytes = 0;
};

struct LowIRLoopInput {
  LowIRLoopSide Side = LowIRLoopSide::Entry;
  LowIRLoopLocation Location;
  NdVar Temporary;
};

struct LowIRLoopAssignment {
  LowIRLoopLocation Location;
  NdVar Value;
};

/// A candidate state template, never an assumed invariant. Start with the real
/// shared entry state (or the checked prefix selected below), then apply the
/// assignments separately to each program.
/// Entry inputs read that snapshot. Other inputs are arbitrary induction
/// parameters whose projections back from the constructed state are checked.
/// Expressions are side-effect-free scalar LowIR over constants and bound
/// temporaries, in a namespace separate from either program. Predicate must be
/// a canonical byte Boolean. Rank is a nonempty unsigned lexicographic tuple.
struct LowIRLoopCutpoint {
  /// LowIR block entries, or an original native instruction entry. Each side's
  /// addresses must be unique. A segment stops before executing its next cut.
  va_t OriginalAddress = 0;
  va_t CandidateAddress = 0;
  /// Use the first feasible paired entry-prefix arrival as the template base
  /// instead of the function entry state. Its path predicate is also retained
  /// and proved on every arrival. The cut must be reached by entry-prefix
  /// exploration; no abstract state is invented for an unreached cut.
  bool UseEntryPrefix = false;
  std::vector<LowIRLoopInput> Inputs;
  std::vector<LowOp> Expressions;
  std::vector<LowIRLoopAssignment> OriginalState, CandidateState;
  NdVar Predicate = NdVar::scalar(1, 1);
  std::vector<NdVar> Rank;
};

struct LowIRLoopRefinementPlan {
  /// Covers every cycle needed by this proof. A missed cycle exhausts finite
  /// segment exploration and cannot be treated as an inductive edge.
  std::vector<LowIRLoopCutpoint> Cutpoints;
};

struct LowIRRefinementProducer {
  /// Unique instruction visit and sidecar index, including revisited loops.
  uint64_t InstructionVisit = 0;
  int BlockId = -1;
  va_t InstructionAddress = 0;
  uint64_t EffectIndex = 0;
};

struct LowIRRefinementCertificate {
  LowIRRefinementScope Scope = LowIRRefinementScope::CompleteFiniteLowIRPaths;
  std::string InputDigest;
  std::string OriginalDigest;
  std::string CandidateDigest;
  LowIRRefinementWitness Witness = LowIRRefinementWitness::LiftedBits;
  LowIRIndependenceContract Contract;
  LowIRRefinementLimits Limits;
  std::vector<LowIRUndefinedInstruction> OriginalInstructions;
  std::vector<LowIRRefinementProducer> Producers;
  std::vector<LowIRNativeFlagTransition> NativeFlagTransitions;
  std::vector<LowIRNativeProfileProjection> NativeProfileProjections;
  /// Present only for inductive scopes. Binds the exact templates, predicates,
  /// projections and rankings that were checked, including their limits.
  std::optional<LowIRLoopRefinementPlan> LoopPlan;
};

struct LowIRRefinementResult {
  LowIRRefinementStatus Status = LowIRRefinementStatus::Invalid;
  std::optional<LowIRRefinementCertificate> Certificate;
  std::string Diagnostic;
  int BlockId = -1;
  va_t InstructionAddress = 0;
  int OpSeq = -1;
  uint64_t Operations = 0;
  uint64_t Instructions = 0;
  /// Complete terminal paths in finite scopes; terminal segments in inductive
  /// scopes. The latter count is not the number of full program executions.
  uint32_t OriginalPaths = 0;
  uint32_t CandidatePaths = 0;
  uint32_t BlockVisits = 0;
  uint32_t Producers = 0;
  uint32_t SolverQueries = 0;
  uint64_t Observations = 0;
  /// Endpoint pairs, including cutpoint arrivals in inductive scopes.
  uint64_t TerminalPairs = 0;
  uint64_t OriginalCutpoints = 0;
  uint64_t CandidateCutpoints = 0;
  uint64_t LoopInitiations = 0;
  uint64_t LoopTransitions = 0;
  uint64_t RankingChecks = 0;

  bool proved() const {
    return Status == LowIRRefinementStatus::Proved && Certificate.has_value();
  }
};

/// Compare a deterministic candidate against an original with explicit
/// architecture-undefined effects. For every admitted ordinary entry input,
/// the selected original execution and candidate must terminate and agree on
/// RETURN operands, requested registers and the union of written frame bytes.
/// Both must meet the preservation contract. Control structure may differ.
///
/// This constructs an ISA-allowed refinement witness, not independence from
/// all undefined choices, CPU-specific equality, or a native/source proof.
/// Candidate is semantic LowIR: no architecture sidecars are inferred for it.
/// Instruction boundaries and temporary lifetimes must still be valid. Loops
/// require complete finite execution within the shared budgets. Every feasible
/// terminal pair and complete entry-domain coverage are checked. Incomplete
/// exploration, unknown aliases and unsupported operations refuse a
/// certificate. Hashes bind all inputs and limits; rerun the check to validate
/// changed inputs.
LowIRRefinementResult checkLowIRRefinement(
    const LowFunc &Original,
    llvm::ArrayRef<LowIRUndefinedInstruction> OriginalInstructions,
    const LowFunc &Candidate, const LowIRIndependenceContract &Contract,
    LowIRRefinementWitness Witness = LowIRRefinementWitness::LiftedBits,
    const LowIRRefinementLimits &Limits = {});

/// Prove entry initiation, every feasible segment successor, invariant
/// preservation, terminal observations and total termination. Both sides must
/// reach corresponding cuts or both return. Every cut-to-cut transition must
/// strictly decrease the same finite unsigned lexicographic rank (matching
/// component widths at all cuts). Finite segment exploration must cover its
/// entire domain; a successful sibling cannot hide an unfinished path.
///
/// The template checks all modified registers and the entire accessible frame
/// at cuts. Inductive terminal observations include the entire frame when
/// written-byte observation is requested, a conservative strengthening that
/// retains writes made in earlier iterations. Templates, predicates and ranks
/// are explicit proof hints; they do not restrict the admitted entry domain.
/// No loop certificate is produced by the finite API or the strict
/// independence API. This still does not certify a C backend or physical CPU.
LowIRRefinementResult checkLowIRLoopRefinement(
    const LowFunc &Original,
    llvm::ArrayRef<LowIRUndefinedInstruction> OriginalInstructions,
    const LowFunc &Candidate, const LowIRIndependenceContract &Contract,
    const LowIRLoopRefinementPlan &Plan,
    LowIRRefinementWitness Witness = LowIRRefinementWitness::LiftedBits,
    const LowIRRefinementLimits &Limits = {});

} // namespace neverd::analysis

#endif
