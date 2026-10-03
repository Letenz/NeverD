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

/// DemandedBits numbers bits in the scalar read of this byte range using the
/// state's byte order. Only those bits influence the discovered expression;
/// the byte range itself remains a candidate, not a finite-value fact.
struct ControlRegisterDemand {
  uint64_t Offset = 0;
  uint16_t Bytes = 0;
  uint64_t DemandedBits = 0;
};

struct ControlFrameDemand {
  int64_t Offset = 0;
  uint16_t Bytes = 0;
  uint64_t DemandedBits = 0;
};

struct ControlDiscovery {
  ControlDiscoveryStatus Status = ControlDiscoveryStatus::Complete;
  /// Conservative bit dependencies on the exact 64-bit FrameRoot expression.
  /// Present only after a complete walk; no location, value, or reachability
  /// fact follows from this mask. A fresh root's omitted bits cannot influence
  /// the expression when all other symbolic inputs are held fixed.
  std::optional<uint64_t> FrameRootBits;
  /// Candidate locations only: neither finiteness nor an incoming value is
  /// established. UnsupportedOrigin may retain useful candidates discovered
  /// through a load's address; BudgetExceeded always clears the candidates.
  std::vector<ControlRegisterDemand> RegisterRanges;
  std::vector<ControlFrameDemand> FrameSlots;
  /// Charged unique expression/slice work items, inspected origin records,
  /// and operands/64-bit words inspected by bounded structural mask scans.
  uint64_t Visited = 0;
};

/// Exact canonical entry-root identity, not a numeric alias or range proof.
/// The supported frame model has one 64-bit root and a modular displacement.
std::optional<uint64_t> frameRelativeOffset(const symbolic::SymContext &Ctx,
                                            symbolic::SymRef Value,
                                            symbolic::SymRef Root);

/// Gather persistent input locations needed by Value without changing State
/// or its context. Structural and bitwise operations, constant shifts and
/// modular low-prefix arithmetic retain bounded bit demands. Sums up to 64
/// bits can retain upper slices when disjoint possible-one masks prove no
/// carry; wider or uncertain sums keep their low prefix. Other operations
/// conservatively visit complete operands. Only structured
/// entry-register inputs and exact root-relative memory inputs are nominated.
/// Memory input birth is distinct from later loads of equal/forwarded values.
/// Post-clobber inputs and external memory are never nominated as fields,
/// though an external load's address can reveal register/frame dependencies.
ControlDiscovery gatherControlDependencies(const symbolic::SymState &State,
                                           symbolic::SymRef Value,
                                           symbolic::SymRef FrameRoot,
                                           uint64_t MaxVisited);

struct VariableBitDemand {
  /// Conservative dependencies on one exact Var of at most 64 bits. Missing
  /// means unsupported or exhausted, never absence of dependence.
  std::optional<uint64_t> Bits;
  uint64_t Visited = 0;
};

/// Read-only expression dependence, using the same bit transfers as control
/// location discovery. Other symbolic variables are independent leaves; no
/// machine location, memory origin, value or reachability is established.
/// Variable validation and the complete walk share MaxVisited.
VariableBitDemand gatherVariableBitDemand(const symbolic::SymContext &Ctx,
                                          symbolic::SymRef Value,
                                          symbolic::SymRef Variable,
                                          uint64_t MaxVisited);

/// On a reachable path, prove that the fresh 64-bit frame root has more than
/// Limit values. Exact declared nonwrapping bounds may restrict its high bits;
/// every other predicate conjunct must depend on at most its low 32 bits.
/// This read-only, cumulatively bounded refusal of optional enumeration proves
/// neither reachability nor a finite value. Unknown or exhausted work is false.
bool frameRootDomainExceedsLimit(
    const symbolic::SymState &State, symbolic::SymRef Predicate,
    symbolic::SymRef FrameRoot,
    const std::optional<SpecializationEntryFrameBounds> &Bounds, uint32_t Limit,
    uint64_t MaxVisited);

} // namespace neverd::analysis::detail

#endif // NEVERD_ANALYSIS_INTERPRETER_CONTROLDISCOVERY_H
