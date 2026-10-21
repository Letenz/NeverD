//===- ProcessInputReplay.h - Defensive replay-plan validation -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_SAFETY_PROCESSINPUTREPLAY_H
#define NEVERD_LIB_SAFETY_PROCESSINPUTREPLAY_H

#include "neverd/safety/Limits.h"
#include "neverd/safety/SafetyTypes.h"

#include <cstdint>
#include <optional>
#include <string>

namespace neverd::safety {

/// Result of validating and taking ownership of a candidate replay plan.
/// Plan is populated exactly when Reason is empty.
struct ProcessInputReplayResult {
  std::optional<ReplayPlan> Plan;
  std::string Reason;
};

/// Validate a plan against the solver model and an explicit, complete set of
/// query-variable IDs stored in ReplayPlan::QueryVariables.  Model entries not
/// in that set are intentionally irrelevant, and SolverAssignment::Fresh is
/// intentionally not consulted: provenance requires a typed ReplayBinding.
std::optional<std::string>
validateProcessInputReplay(const ReplayPlan &Plan,
                           const std::vector<SolverAssignment> &Model,
                           uint64_t ByteBudget = ::neverd::limits::safety::
                               kProcessInputReplayProcessInputReplayByteBudget);

/// Validate Candidate and return it only when it is a bounded, exact replay.
ProcessInputReplayResult
buildProcessInputReplay(ReplayPlan Candidate,
                        const std::vector<SolverAssignment> &Model,
                        uint64_t ByteBudget = ::neverd::limits::safety::
                            kProcessInputReplayProcessInputReplayByteBudget);

} // namespace neverd::safety

// Preserve interface spellings; resource values are defined by Limits.h.
namespace neverd::safety {
inline constexpr uint64_t kProcessInputReplayByteBudget =
    ::neverd::limits::safety::kProcessInputReplayProcessInputReplayByteBudget;
}

#endif // NEVERD_LIB_SAFETY_PROCESSINPUTREPLAY_H
