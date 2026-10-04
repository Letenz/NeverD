//===- LowIRLoopInference.h - Internal loop proposal families -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_ANALYSIS_INTERPRETER_LOWIRLOOPINFERENCE_H
#define NEVERD_LIB_ANALYSIS_INTERPRETER_LOWIRLOOPINFERENCE_H

#include "neverd/analysis/LowIRRefinement.h"

namespace neverd::analysis::detail {

enum class LowIRLoopCutFamily { Default, BranchArms, FilteredBranchArms };

/// Additional families remain untrusted proposals. Only previously inferred
/// plans can exclude a duplicate family; their templates are never imported.
LowIRLoopInferenceResult inferLowIRLoopRefinementPlanFamily(
    const LowFunc &Candidate, const LowIRIndependenceContract &Contract,
    const LowIRLoopInferenceLimits &Limits, LowIRLoopCutFamily Family,
    llvm::ArrayRef<const LowIRLoopRefinementPlan *> PreviousPlans = {});

/// Propose cuts at cyclic branch-arm entries, completing cycle coverage with
/// the ordinary greedy selector. DefaultPlan, when available, only excludes
/// a duplicate cut family; it supplies no invariant or trusted semantics.
LowIRLoopInferenceResult inferBranchArmLowIRLoopRefinementPlan(
    const LowFunc &Candidate, const LowIRIndependenceContract &Contract,
    const LowIRLoopInferenceLimits &Limits,
    const LowIRLoopRefinementPlan *DefaultPlan = nullptr);

} // namespace neverd::analysis::detail

#endif
