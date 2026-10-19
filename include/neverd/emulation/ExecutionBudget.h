//===- ExecutionBudget.h - Shared finite workload resources ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_EXECUTIONBUDGET_H
#define NEVERD_EMULATION_EXECUTIONBUDGET_H

#include "llvm/Support/Error.h"

#include <chrono>
#include <cstdint>
#include <memory>

namespace neverd::emulation {
struct ExecutionLimits {
  uint64_t Instructions, Events, TimeoutMicroseconds;
};
enum class ExecutionTimePolicy {
#define NEVERD_EXECUTION_TIME_POLICY(Name, Text) Name,
#include "neverd/emulation/ExecutionBudget.def"
#undef NEVERD_EXECUTION_TIME_POLICY
};
const char *executionTimePolicyName(ExecutionTimePolicy Policy);

/// One caller-confined budget shared by all CPUs, threads and continuations of
/// a workload. It cannot be copied, moved or reset to replenish resources.
/// The runtime charges admitted instruction attempts, not retired hardware
/// instructions. Each OS model decides which modeled events consume credit.
/// Neither host callbacks nor native cancellation gain a hard time bound.
class ExecutionBudget final {
public:
  using Clock = std::chrono::steady_clock;
  static llvm::Expected<std::unique_ptr<ExecutionBudget>>
  create(ExecutionLimits Limits, Clock::time_point Now = Clock::now());
  static llvm::Expected<std::unique_ptr<ExecutionBudget>>
  createWithTimePolicy(ExecutionLimits Limits, ExecutionTimePolicy Policy,
                       Clock::time_point Now = Clock::now());

  /// One owned active-work interval. Nested intervals charge their union once;
  /// idle time between intervals is excluded under ActiveWork. The budget must
  /// outlive its scopes. close() supports explicit clock samples in tests;
  /// destruction closes a remaining interval with the current monotonic clock.
  class WorkScope final {
  public:
    ~WorkScope();
    WorkScope(const WorkScope &) = delete;
    WorkScope &operator=(const WorkScope &) = delete;
    llvm::Error close(Clock::time_point Now = Clock::now());

  private:
    friend class ExecutionBudget;
    explicit WorkScope(ExecutionBudget *Budget) : Budget(Budget) {}
    ExecutionBudget *Budget;
  };
  llvm::Expected<std::unique_ptr<WorkScope>>
  enterWork(Clock::time_point Now = Clock::now());
  ExecutionTimePolicy timePolicy() const { return Policy; }

  ExecutionBudget(const ExecutionBudget &) = delete;
  ExecutionBudget &operator=(const ExecutionBudget &) = delete;

  const ExecutionLimits &limits() const { return Limits; }
  uint64_t instructions() const { return Instructions; }
  uint64_t events() const { return Events; }
  bool hasInstructions(uint64_t Count = 1) const;
  bool hasEvents(uint64_t Count = 1) const;
  /// Failed reservations have no effect, including at UINT64_MAX.
  bool consumeInstructions(uint64_t Count = 1);
  bool consumeEvents(uint64_t Count = 1);

  /// Rounded down for CPU admission; zero also covers a final fractional
  /// microsecond. An earlier clock sample never extends the original budget.
  uint64_t remainingMicroseconds(Clock::time_point Now = Clock::now()) const;

private:
  ExecutionBudget(ExecutionLimits Limits, Clock::time_point Start,
                  Clock::time_point Deadline, ExecutionTimePolicy Policy)
      : Limits(Limits), Start(Start), Deadline(Deadline), Policy(Policy),
        LastWorkEnd(Start) {}
  void leaveWork(Clock::time_point Now) noexcept;
  const ExecutionLimits Limits;
  const Clock::time_point Start, Deadline;
  const ExecutionTimePolicy Policy;
  Clock::duration ActiveElapsed{};
  Clock::time_point ActiveStart{}, ActiveDeadline{}, LastWorkEnd;
  mutable Clock::time_point ActiveSample{};
  uint64_t WorkDepth = 0;
  uint64_t Instructions = 0, Events = 0;
};
} // namespace neverd::emulation
#endif
