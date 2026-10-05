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
};

LowIRLoopInferenceResult inferNativeLowIRLoopRefinementPlan(
    SpecializationProvider &Provider, const LowFunc &Candidate,
    const LowIRIndependenceContract &Contract,
    const LowIRLoopInferenceLimits &Limits,
    llvm::ArrayRef<NativeLoopCutpointOrigin> EligibleOrigins);

} // namespace neverd::analysis::detail

#endif
