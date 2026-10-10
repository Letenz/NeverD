//===- NativeUndefinedIndependence.h - Original native execution -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_INTERPRETER_NATIVEUNDEFINEDINDEPENDENCE_H
#define NEVERD_ANALYSIS_INTERPRETER_NATIVEUNDEFINEDINDEPENDENCE_H

#include "neverd/analysis/InterpreterSpecialization.h"
#include "neverd/analysis/LowIRRefinement.h"
#include "neverd/analysis/LowIRUndefinedIndependence.h"

#include "llvm/ADT/STLFunctionalExtras.h"

namespace neverd::analysis::detail {

struct NativeUndefinedIndependenceResult {
  LowIRIndependenceResult Proof;
  std::vector<SpecializationInstruction> Instructions;
  std::vector<SpecializationReadWitness> Reads;
};

/// Explore original instructions with persistent paired machine states. The
/// caller supplies immutable instruction/read evidence and the physical stack
/// contract; no recovered graph, selected return address or projected state is
/// accepted. Success requires all scheduled executions to reach an outer
/// return.
NativeUndefinedIndependenceResult
checkNativeUndefinedIndependence(SpecializationProvider &Provider,
                                 SpecializationCursor Entry,
                                 const LowIRIndependenceContract &Contract,
                                 const LowIRIndependenceLimits &Limits);

struct NativeLowIRRefinementResult {
  LowIRRefinementResult Proof;
  std::vector<SpecializationInstruction> Instructions;
  std::vector<SpecializationReadWitness> Reads;
};

NativeLowIRRefinementResult checkNativeLowIRRefinement(
    SpecializationProvider &Provider, SpecializationCursor Entry,
    const LowFunc &Candidate, const LowIRIndependenceContract &Contract,
    LowIRRefinementWitness Witness, const LowIRRefinementLimits &Limits,
    const LowIRLoopRefinementPlan *LoopPlan = nullptr);

/// Untrusted one-to-one residual mappings; original addresses may repeat.
struct NativeLoopCutpointOrigin {
  va_t CandidateAddress, OriginalAddress;
  /// Search priority only; this flag grants no semantic authority.
  bool UniqueOriginal = false;
};

LowIRLoopInferenceResult inferNativeLowIRLoopRefinementPlan(
    SpecializationProvider &Provider, const LowFunc &Candidate,
    const LowIRIndependenceContract &Contract,
    const LowIRLoopInferenceLimits &Limits,
    llvm::ArrayRef<NativeLoopCutpointOrigin> EligibleOrigins);

struct NativeLoopStateProposalFailure {
  LowIRLoopInferenceStatus Status;
  std::string Diagnostic;
};

/// Append untrusted own-prefix selector assignments to generalized cuts whose
/// guard ranges are disjoint from their existing state assignments. The caller
/// must validate the complete plan before and after this transformation.
/// Existing input provenance is retained; candidate inference must rebind its
/// own prefix inputs before calling. Charge uses the caller's cumulative work
/// budget and may throw on exhaustion. After any failure, partial modifications
/// must not be consumed or published. This constructs no proof or assumptions.
std::optional<NativeLoopStateProposalFailure>
proposeNativeLoopSelectorStates(LowIRLoopRefinementPlan &Plan,
                                const LowIRIndependenceLimits &Limits,
                                llvm::function_ref<void(uint64_t)> Charge);

} // namespace neverd::analysis::detail

#endif
