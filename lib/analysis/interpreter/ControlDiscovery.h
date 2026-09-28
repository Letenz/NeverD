//===- ControlDiscovery.h - Bounded control dependency discovery --*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_INTERPRETER_CONTROLDISCOVERY_H
#define NEVERD_ANALYSIS_INTERPRETER_CONTROLDISCOVERY_H

#include "neverd/analysis/InterpreterSpecialization.h"
#include "neverd/symbolic/SymState.h"

namespace neverd::analysis::detail {

enum class ControlDiscoveryStatus {
  Complete,
  UnsupportedOrigin,
  BudgetExceeded,
};

struct ControlDiscovery {
  ControlDiscoveryStatus Status = ControlDiscoveryStatus::Complete;
  /// Candidate locations only: neither finiteness nor an incoming value is
  /// established. UnsupportedOrigin may retain useful candidates discovered
  /// through a load's address; BudgetExceeded always clears the candidates.
  std::vector<symbolic::SymRegisterRange> RegisterRanges;
  std::vector<SpecializationFrameSlot> FrameSlots;
  /// Charged unique expression/slice work items and inspected load origins.
  uint64_t Visited = 0;
};

/// Exact canonical entry-root identity, not a numeric alias or range proof.
/// The supported frame model has one 64-bit root and a modular displacement.
std::optional<uint64_t> frameRelativeOffset(const symbolic::SymContext &Ctx,
                                            symbolic::SymRef Value,
                                            symbolic::SymRef Root);

/// Gather persistent input locations needed by Value without changing State
/// or its context. Extract/Concat and widening retain demanded byte ranges;
/// other arithmetic conservatively visits complete operands. Only structured
/// entry-register inputs and exact root-relative load ranges are nominated.
/// Unknown/fresh values and external memory are never nominated as fields,
/// though an external load's address can reveal register/frame dependencies.
ControlDiscovery gatherControlDependencies(const symbolic::SymState &State,
                                           symbolic::SymRef Value,
                                           symbolic::SymRef FrameRoot,
                                           uint64_t MaxVisited);

} // namespace neverd::analysis::detail

#endif // NEVERD_ANALYSIS_INTERPRETER_CONTROLDISCOVERY_H
