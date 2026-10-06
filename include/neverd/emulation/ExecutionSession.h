//===- ExecutionSession.h - Bounded CPU continuations ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_EXECUTIONSESSION_H
#define NEVERD_EMULATION_EXECUTIONSESSION_H

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/ExecutionBudget.h"

#include <vector>

namespace neverd::emulation {
enum class SessionExitKind {
#define NEVERD_SESSION_EXIT(Name, Text) Name,
#include "neverd/emulation/ExecutionSession.def"
#undef NEVERD_SESSION_EXIT
};
const char *sessionExitKindName(SessionExitKind Kind);

/// A half-open guest range [Address, Address + Size). The first instruction
/// attempted inside it suspends the session before admission, budget charge or
/// any instruction effect.
struct ExecutionWatch {
  uint64_t Address, Size;
};

struct SessionExit {
  SessionExitKind Kind;
  /// Absent if the runtime stopped before entering the CPU. A simultaneous
  /// CPU failure always outranks the runtime's requested admission stop.
  std::optional<ExecutionExit> CPU;
};

/// Owns one CPU and its hooks, independently of OS policy and mapped images.
/// Multiple sessions can share one workload budget and physical memory, and
/// execute deterministic instruction quanta cooperatively. This is not SMP.
/// Service/fault continuations require explicit consumption before resumption;
/// neither consumption nor a quantum yield resets resources or guest state.
class ExecutionSession final {
public:
  /// Observer sees admitted instruction attempts only; it must not mutate
  /// or execute the CPU, replace hooks, or replenish the workload budget.
  static llvm::Expected<std::unique_ptr<ExecutionSession>>
  create(std::unique_ptr<ExecutionBackend> CPU,
         std::shared_ptr<ExecutionBudget> Budget,
         std::function<bool(const BackendFault &)> Recovery = {},
         std::function<void(uint64_t, uint32_t)> InstructionObserver = {});

  ExecutionSession(const ExecutionSession &) = delete;
  ExecutionSession &operator=(const ExecutionSession &) = delete;

  /// Stopped model access only. The owner must not replace hooks, consume
  /// pending exits directly, or execute this CPU outside the session.
  ExecutionBackend &cpu() { return *CPU; }
  ExecutionBudget &budget() { return *Budget; }
  llvm::Expected<SessionExit> run(uint64_t PC, uint64_t InstructionQuantum);
  /// Replace every execution watch while the CPU is stopped. Ranges must be
  /// nonempty and must not wrap; overlapping ranges are merged. A watched
  /// instruction ends the run with SessionExitKind::ExecutionWatch and leaves
  /// the CPU at that PC. Resuming at the reported PC executes that one
  /// instruction without matching again, so an owner may keep the range.
  llvm::Error watchExecution(std::vector<ExecutionWatch> Watches);
  llvm::Expected<ServiceRequest> takeServiceRequest();
  llvm::Expected<BackendFault> takeRecoverableFault();
  /// The sole thread-safe operation; follows the selected CPU's stop contract.
  void stop() { CPU->stop(); }

private:
  enum class State { Ready, Service, Recoverable, Terminal };
  ExecutionSession(std::unique_ptr<ExecutionBackend> CPU,
                   std::shared_ptr<ExecutionBudget> Budget)
      : Budget(std::move(Budget)), CPU(std::move(CPU)) {}
  bool watched(uint64_t PC) const;
  State Current = State::Ready;
  uint64_t Quantum = 0, Admitted = 0;
  std::optional<SessionExitKind> AdmissionStop;
  /// Sorted, disjoint and nonadjacent.
  std::vector<ExecutionWatch> Watches;
  std::optional<uint64_t> WatchedPC, WatchResume;
  std::shared_ptr<ExecutionBudget> Budget;
  // Destroy the CPU/hook closures before the state they observe.
  std::unique_ptr<ExecutionBackend> CPU;
};
} // namespace neverd::emulation
#endif
