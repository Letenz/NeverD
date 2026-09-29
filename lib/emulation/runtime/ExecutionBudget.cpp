//===- ExecutionBudget.cpp - Finite workload resource accounting ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/emulation/ExecutionBudget.h"

#include "../core/ExecutionDeadline.h"
#include "RuntimeValues.h"

#include <algorithm>

namespace neverd::emulation {
llvm::Expected<std::unique_ptr<ExecutionBudget>>
ExecutionBudget::create(ExecutionLimits Limits, Clock::time_point Now) {
  if (!Limits.Instructions || !Limits.Events)
    return diagnostic::error(runtime::Limits);
  auto Deadline = makeExecutionDeadline(Limits.TimeoutMicroseconds, Now);
  if (!Deadline)
    return Deadline.takeError();
  return std::unique_ptr<ExecutionBudget>(
      new ExecutionBudget(Limits, Now, *Deadline));
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
  if (Now >= Deadline)
    return 0;
  return std::chrono::duration_cast<std::chrono::microseconds>(
             Deadline - std::max(Now, Start))
      .count();
}
} // namespace neverd::emulation
