//===- Limits.h - Component limits and defaults -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Resource ceilings, work budgets and defaults owned by this component.
/// Binary layouts and instruction/ABI constants remain with their models.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_CONCOLIC_LIMITS_H
#define NEVERD_CONCOLIC_LIMITS_H

#include <cstddef>
#include <cstdint>

namespace neverd::limits::concolic {

// include/neverd/concolic/LowIRConcolic.h
inline constexpr uint64_t
    kLowIRConcolicDefaultLowIRConcolicSolverOptionsMaxConflicts =
        uint64_t{1} << 18;
inline constexpr uint64_t
    kLowIRConcolicDefaultLowIRConcolicSolverOptionsMaxPropagations =
        uint64_t{1} << 24;
inline constexpr uint64_t
    kLowIRConcolicDefaultLowIRConcolicSolverOptionsMaxWatchVisits =
        uint64_t{1} << 26;

} // namespace neverd::limits::concolic

#endif // NEVERD_CONCOLIC_LIMITS_H
