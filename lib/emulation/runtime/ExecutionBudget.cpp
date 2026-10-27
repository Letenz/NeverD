//===- ExecutionBudget.cpp - Finite workload resource accounting ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/emulation/ExecutionBudget.h"

#include "../core/ExecutionDeadline.h"
#include "RuntimeValues.h"

#include <algorithm>
#include <cassert>

namespace neverd::emulation {
llvm::Expected<std::unique_ptr<ExecutionBudget>>
ExecutionBudget::create(ExecutionLimits Limits, Clock::time_point Now) {
  return createWithTimePolicy(Limits, ExecutionTimePolicy::AbsoluteDeadline,
                              Now);
}
llvm::Expected<std::unique_ptr<ExecutionBudget>>
ExecutionBudget::createWithTimePolicy(ExecutionLimits Limits,
                                      ExecutionTimePolicy Policy,
                                      Clock::time_point Now) {
  if (!Limits.Instructions || !Limits.Events)
    return diagnostic::error(runtime::Limits);
  if (Policy != ExecutionTimePolicy::AbsoluteDeadline &&
      Policy != ExecutionTimePolicy::ActiveWork)
    return diagnostic::error(runtime::Limits);
  auto Deadline = makeExecutionDeadline(Limits.TimeoutMicroseconds, Now);
  if (!Deadline)
    return Deadline.takeError();
  return std::unique_ptr<ExecutionBudget>(
      new ExecutionBudget(Limits, Now, *Deadline, Policy));
}
const char *executionTimePolicyName(ExecutionTimePolicy Policy) {
  switch (Policy) {
#define NEVERD_EXECUTION_TIME_POLICY(Name, Text)                               \
  case ExecutionTimePolicy::Name:                                              \
    return Text;
#include "neverd/emulation/ExecutionBudget.def"
#undef NEVERD_EXECUTION_TIME_POLICY
  }
  llvm_unreachable("invalid execution time policy");
}
ExecutionBudget::WorkScope::~WorkScope() {
  if (Budget)
    Budget->leaveWork(Clock::now());
}
llvm::Error ExecutionBudget::WorkScope::close(Clock::time_point Now) {
  if (!Budget)
    return diagnostic::error(runtime::WorkScope);
  Budget->leaveWork(Now);
  Budget = nullptr;
  return llvm::Error::success();
}
llvm::Expected<std::unique_ptr<ExecutionBudget::WorkScope>>
ExecutionBudget::enterWork(Clock::time_point Now) {
  if (WorkDepth == UINT64_MAX)
    return diagnostic::error(runtime::WorkScope);
  Clock::time_point Begin{}, End{};
  if (Policy == ExecutionTimePolicy::ActiveWork && !WorkDepth) {
    Begin = std::max(Now, LastWorkEnd);
    const auto Limit = std::chrono::duration_cast<Clock::duration>(
        std::chrono::microseconds(Limits.TimeoutMicroseconds));
    const auto Remaining = Limit - ActiveElapsed;
    if (Begin > Clock::time_point::max() - Remaining)
      return diagnostic::error(runtime::WorkScope);
    End = Begin + Remaining;
  }
  auto Scope = std::unique_ptr<WorkScope>(new WorkScope(this));
  if (Policy == ExecutionTimePolicy::ActiveWork) {
    if (!WorkDepth) {
      ActiveStart = ActiveSample = Begin;
      ActiveDeadline = End;
    } else {
      ActiveSample = std::max(ActiveSample, Now);
    }
  }
  ++WorkDepth;
  return Scope;
}
void ExecutionBudget::leaveWork(Clock::time_point Now) noexcept {
  assert(WorkDepth && "unowned workload interval");
  if (Policy == ExecutionTimePolicy::ActiveWork) {
    ActiveSample = std::max({ActiveSample, Now, ActiveStart});
    if (WorkDepth == 1) {
      ActiveElapsed += std::min(ActiveSample, ActiveDeadline) - ActiveStart;
      LastWorkEnd = ActiveSample;
    }
  }
  --WorkDepth;
}
bool ExecutionBudget::hasInstructions(uint64_t Count) const {
  return Count <= Limits.Instructions - Instructions;
}
bool ExecutionBudget::hasEvents(uint64_t Count) const {
  return Count <= Limits.Events - Events;
}
bool ExecutionBudget::consumeInstructions(uint64_t Count) {
  if (!hasInstructions(Count))
    return false;
  Instructions += Count;
  return true;
}
bool ExecutionBudget::consumeEvents(uint64_t Count) {
  if (!hasEvents(Count))
    return false;
  Events += Count;
  return true;
}
uint64_t ExecutionBudget::remainingMicroseconds(Clock::time_point Now) const {
  if (Policy == ExecutionTimePolicy::ActiveWork) {
    auto Remaining =
        std::chrono::duration_cast<Clock::duration>(
            std::chrono::microseconds(Limits.TimeoutMicroseconds)) -
        ActiveElapsed;
    if (WorkDepth) {
      ActiveSample = std::max({ActiveSample, Now, ActiveStart});
      if (ActiveSample >= ActiveDeadline)
        return 0;
      Remaining -= ActiveSample - ActiveStart;
    }
    return std::chrono::duration_cast<std::chrono::microseconds>(Remaining)
        .count();
  }
  if (Now >= Deadline)
    return 0;
  return std::chrono::duration_cast<std::chrono::microseconds>(
             Deadline - std::max(Now, Start))
      .count();
}
} // namespace neverd::emulation
