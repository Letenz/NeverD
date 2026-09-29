//===- ExecutionDeadline.h - Validated finite CPU execution budgets ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_EXECUTIONDEADLINE_H
#define NEVERD_EMULATION_EXECUTIONDEADLINE_H

#include "ExecutionDiagnostics.h"

#include <chrono>
#include <cstdint>
#include <ratio>

namespace neverd::emulation {
inline llvm::Expected<std::chrono::steady_clock::time_point>
makeExecutionDeadline(uint64_t TimeoutMicroseconds,
                      std::chrono::steady_clock::time_point Now =
                          std::chrono::steady_clock::now()) {
  using Clock = std::chrono::steady_clock;
  using Microseconds = std::chrono::microseconds;
  using TicksPerMicrosecond =
      std::ratio_divide<Microseconds::period, Clock::period>;
  // Supported host clocks represent whole microseconds exactly. Validate the
  // multiplication before converting the unsigned public API value.
  static_assert(TicksPerMicrosecond::den == 1);
  if (!TimeoutMicroseconds ||
      TimeoutMicroseconds > uint64_t(Microseconds::max().count()) ||
      TimeoutMicroseconds >
          uint64_t(Clock::duration::max().count()) / TicksPerMicrosecond::num)
    return diagnostic::error(diagnostic::ExecutionTimeout);
  const auto Duration = std::chrono::duration_cast<Clock::duration>(
      Microseconds(TimeoutMicroseconds));
  // Subtract the validated nonnegative duration from the upper limit. The
  // converse subtraction (max - Now) could overflow for a negative epoch.
  if (Now > Clock::time_point::max() - Duration)
    return diagnostic::error(diagnostic::ExecutionTimeout);
  return Now + Duration;
}
} // namespace neverd::emulation
#endif
