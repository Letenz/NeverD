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

// ExecutionWatch is defined with the backend interface in CPU.h: the direct
// contract enforces watches below the session, in the CPU's page tables.

struct SessionExit {
  SessionExitKind Kind;
  /// Absent if the runtime stopped before entering the CPU. A simultaneous
  /// CPU failure always outranks the runtime's requested admission stop.
  std::optional<ExecutionExit> CPU;
};

/// A scene's pre-admission decision. Stop observes an uncharged boundary;
/// Emulate charges an instruction attempt and stops before its machine effects.
/// Defer retains an uncharged instruction for stopped model validation,
/// followed by admitDeferredInstruction(). None of these decisions guesses CPU
/// semantics.
enum class InstructionAdmission { Execute, Stop, Emulate, Defer };
struct SessionModelHooks {
  /// May read CPU state and retain scene decisions. No IO, CPU mutation/run,
  /// hook replacement or budget consumption; stop() is permitted. Session
  /// watches and reserved returns have priority over this callback.
  std::function<InstructionAdmission(uint64_t, uint32_t)> BeforeInstruction;
  /// Existing OS access/diagnostic policy, with the same constraints as backend
  /// observers. These do not replace session-owned continuation/watch hooks.
  std::function<void(uint64_t, uint32_t)> Read;
  std::function<void(uint64_t, uint32_t, uint64_t)> Write;
  std::function<void(uint64_t, uint32_t, const char *)> Fault;
  std::function<void(uint32_t)> Interrupt;
  std::function<void()> InvalidInstruction;
};

/// CPU and session watch control from one stopped session. Memory, hooks and
/// cumulative budgets are shared, never rolled back. A pending resource,
/// service, exception or terminal outcome cannot be captured or overwritten.
class SessionContext final {
public:
  ~SessionContext() = default;
  SessionContext(const SessionContext &) = delete;
  SessionContext &operator=(const SessionContext &) = delete;

private:
  friend class ExecutionSession;
  SessionContext() = default;
  std::weak_ptr<const void> Owner;
  std::unique_ptr<BackendContext> CPU;
  std::vector<ExecutionWatch> Watches;
  std::vector<MemoryWriteWatch> WriteWatches;
  std::optional<uint64_t> WatchedPC, WatchResume, ReturnPC;
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
         std::function<void(uint64_t, uint32_t)> InstructionObserver = {},
         std::function<bool(const ResourcePause &)> ResourcePolicy = {},
         SessionModelHooks ModelHooks = {});

  ExecutionSession(const ExecutionSession &) = delete;
  ExecutionSession &operator=(const ExecutionSession &) = delete;

  /// Stopped model access only. The owner must not replace hooks, consume
  /// pending exits directly, or execute this CPU outside the session.
  ExecutionBackend &cpu() { return *CPU; }
  ExecutionBudget &budget() { return *Budget; }
  /// A model boundary may still complete after the final instruction credit.
  /// Session admission remains the authority for charging or rejecting code.
  bool hasModelAdmissionPolicy() const { return HasModelAdmission; }
  bool allowsUnchargedBoundaries() const {
    return HasModelAdmission || ReturnPC.has_value();
  }
  bool readyToRun() const {
    return Current == State::Ready || Current == State::Continuation;
  }
  bool hasResourceContinuation() const {
    return Current == State::Continuation;
  }
  bool hasDeferredInstruction() const { return Current == State::Deferred; }
  /// Accept exactly one retained attempt after stopped model validation. No
  /// machine instruction executes. The CPU must still identify the same PC;
  /// context/watch/run operations remain blocked until acceptance. An empty
  /// result means admitted; a quota result retains the pending instruction.
  /// The original run's local deadline and shared budget are both enforced.
  llvm::Expected<std::optional<SessionExitKind>> admitDeferredInstruction();
  bool hasInvocationReturn() const {
    return Current == State::Ready && ReturnPC &&
           AdmissionStop == SessionExitKind::InvocationReturn;
  }
  llvm::Expected<SessionExit> run(uint64_t PC, uint64_t InstructionQuantum,
                                  uint64_t MaxMicroseconds = UINT64_MAX);
  llvm::Expected<std::unique_ptr<SessionContext>> saveContext();
  /// Restores CPU/watch state only. A transport failure after effects makes
  /// the session terminal; a foreign context is rejected before effects.
  llvm::Error restoreContext(const SessionContext &);
  /// One reserved call-return boundary, independent of environment watches.
  /// It yields InvocationReturn before admission and cannot be stepped over.
  /// Restore the saved SessionContext to retire it and reinstate caller
  /// control.
  llvm::Error watchReturn(uint64_t PC);
  /// Replace every execution watch while the CPU is stopped. Ranges must be
  /// nonempty and must not wrap; overlapping ranges are merged. A watched
  /// instruction ends the run with SessionExitKind::ExecutionWatch and leaves
  /// the CPU at that PC. Resuming at the reported PC executes that one
  /// instruction without matching again, so an owner may keep the range.
  /// In checked execution, an exhausted instruction budget or quantum takes
  /// precedence over observing the next instruction.
  llvm::Error watchExecution(std::vector<ExecutionWatch> Watches);
  /// Replace RAM write watches while stopped. A committed write ends the run
  /// with MemoryWriteWatch before the next instruction, without charging it.
  /// CPU faults outrank this notification, including partial REP faults.
  llvm::Error watchMemoryWrites(std::vector<MemoryWriteWatch> Watches);
  llvm::Expected<ServiceRequest> takeServiceRequest();
  llvm::Expected<BackendFault> takeRecoverableFault();
  llvm::Error acknowledgeResourcePause();
  /// The sole thread-safe operation; follows the selected CPU's stop contract.
  void stop() { CPU->stop(); }

private:
  enum class State {
    Ready,
    Running,
    Service,
    Recoverable,
    Resource,
    Continuation,
    Deferred,
    Terminal
  };
  ExecutionSession(std::unique_ptr<ExecutionBackend> CPU,
                   std::shared_ptr<ExecutionBudget> Budget)
      : Budget(std::move(Budget)), CPU(std::move(CPU)) {}
  bool watched(uint64_t PC) const;
  State Current = State::Ready;
  uint64_t Quantum = 0, Admitted = 0;
  std::optional<SessionExitKind> AdmissionStop;
  /// Sorted, disjoint and nonadjacent.
  std::vector<ExecutionWatch> Watches;
  std::vector<MemoryWriteWatch> WriteWatches;
  std::optional<uint64_t> WatchedPC, WatchResume;
  std::optional<uint64_t> ReturnPC;
  std::optional<uint64_t> ResourceResumePC;
  struct Instruction {
    uint64_t PC;
    uint32_t Size;
  };
  std::optional<Instruction> DeferredInstruction;
  std::optional<ExecutionBudget::Clock::time_point> InstructionDeadline;
  std::function<void(uint64_t, uint32_t)> InstructionObserver;
  bool HasModelAdmission = false;
  std::shared_ptr<ExecutionBudget> Budget;
  std::shared_ptr<const void> ContextOwner = std::make_shared<uint8_t>(0);
  // Destroy the CPU/hook closures before the state they observe.
  std::unique_ptr<ExecutionBackend> CPU;
};
} // namespace neverd::emulation
#endif
