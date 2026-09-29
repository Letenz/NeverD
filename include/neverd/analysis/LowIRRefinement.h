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
  /// The supplied constructive witness does not establish this relation.
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
  uint32_t OriginalPaths = 0;
  uint32_t CandidatePaths = 0;
  uint32_t BlockVisits = 0;
  uint32_t Producers = 0;
  uint32_t SolverQueries = 0;
  uint64_t Observations = 0;
  uint64_t TerminalPairs = 0;

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

} // namespace neverd::analysis

#endif
