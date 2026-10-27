//===- ExecutionRun.h - Persistent workload owner --------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Persistent workload owner.
///

#ifndef NEVERD_EMULATION_EXECUTIONRUN_H
#define NEVERD_EMULATION_EXECUTIONRUN_H
#include "neverd/emulation/ExecutionEnvironment.h"
#include "neverd/emulation/ExecutionLimits.h"
#include "neverd/emulation/ModuleResolver.h"

#include <atomic>

namespace neverd::emulation {
enum class ExecutionRunState {
#define NEVERD_RUN_STATE(Name, Text) Name,
#include "neverd/emulation/ExecutionRun.def"
#undef NEVERD_RUN_STATE
};
const char *executionRunStateName(ExecutionRunState);
const char *executionRunReasonName(ExecutionRunReason);
struct ExecutionRunOptions {
  ExecutionLimits Limits{100000000, 1000000, 10000000};
  ExecutionTimePolicy TimePolicy = ExecutionTimePolicy::ActiveWork;
  uint64_t InstructionQuantum = 100000, ActionsPerRun = 4096;
  uint64_t InstanceLimit = 256, MemoryTransferLimit = 1024 * 1024;
  bool EnableResourcePauses = true, PauseUnknownAddresses = true;
  std::vector<std::string> PauseStages;
  std::vector<ModuleBinding> Bindings;
};
struct ExecutionRunEvent {
  ExecutionRunState State = ExecutionRunState::Ready;
  ExecutionRunReason Reason = ExecutionRunReason::Created;
  std::string Stage, Diagnostic;
  std::optional<SessionExit> CPU;
  std::optional<ModuleRequest> Request;
  std::optional<ResourcePause> Resource;
  std::optional<GuestInvocationResult> Invocation;
};
struct ExecutionRunSnapshot {
  ExecutionRunEvent Event;
  std::string Environment;
  ExecutionTimePolicy TimePolicy;
  uint64_t Instructions, Events, RemainingMicroseconds, ModuleRevision;
  std::optional<ResourcePause> Continuation;
  std::optional<GuestInvocationProgress> Invocation;
};
/// Retains one CPU/session, environment, module table and budget across user
/// yields. state()/requestPause() are thread-safe while the owner lives. All
/// other calls require serialization and reject callback reentry. Adding a
/// shared ModuleSet source does not modify or resume any execution implicitly.
class ExecutionRun final {
public:
  /// An optional existing budget must have exactly the requested limits/time
  /// policy. Its consumed credits, deadline and active time remain unchanged.
  static llvm::Expected<std::unique_ptr<ExecutionRun>>
  create(std::unique_ptr<ExecutionBackend> CPU,
         std::shared_ptr<modules::ModuleSet> Sources,
         std::unique_ptr<ExecutionEnvironment> Environment,
         ExecutionRunOptions Options = {},
         std::shared_ptr<ExecutionBudget> SharedBudget = {});
  ExecutionRun(const ExecutionRun &) = delete;
  ExecutionRun &operator=(const ExecutionRun &) = delete;
  ~ExecutionRun();
  llvm::Expected<ExecutionRunEvent> run();
  /// Explicit Paused -> Ready; does not execute a CPU or replenish budgets.
  llvm::Error resume();
  llvm::Error requestPause();
  llvm::Error supply(ModuleRequestToken, llvm::ArrayRef<ModuleBinding>);
  llvm::Expected<ModuleResolutionKind> resolve(const ModuleResolver &);
  llvm::Error declareModule(ModuleBinding);
  /// Starts one finite call at an allowed stage; does not execute instructions.
  /// run()/supply()/resume() drive it through the same environment/session.
  llvm::Error invoke(GuestInvocationRequest);
  llvm::Expected<ExecutionRunSnapshot> inspect() const;
  llvm::Expected<std::vector<uint8_t>> readMemory(uint64_t, uint64_t) const;
  llvm::Error writeMemory(uint64_t, llvm::ArrayRef<uint8_t>);
  ExecutionRunState state() const { return Current.load(); }
  const ModuleBindings &modules() const { return *Modules; }

private:
  ExecutionRun(std::unique_ptr<ExecutionEnvironment> Environment,
               ExecutionRunOptions Options);
  ModuleMappingBuilder builder();
  bool admitResource(const ResourcePause &) const;
  llvm::Expected<ExecutionRunEvent> awaitResource(ModuleRequest,
                                                  std::optional<ResourcePause>,
                                                  std::optional<SessionExit>,
                                                  std::string Diagnostic = {});
  ExecutionRunEvent publish(ExecutionRunState, ExecutionRunReason,
                            std::string Diagnostic = {});
  ExecutionRunEvent fail(ExecutionRunReason, std::string);
  llvm::Error supplyImpl(ModuleRequestToken, llvm::ArrayRef<ModuleBinding>);
  std::optional<ExecutionRunEvent>
  budgetFailure(bool RequireInstructions = false);
  llvm::Expected<std::optional<ExecutionRunEvent>>
  prepareEager(const EnvironmentAction &);
  llvm::Expected<std::optional<ExecutionRunEvent>>
  executeCPU(const EnvironmentAction &, bool ResourceContinuation);
  llvm::Expected<ExecutionRunEvent> finishInvocation(const SessionExit &);
  std::unique_ptr<ExecutionEnvironment> Environment;
  ExecutionRunOptions Options;
  std::string EnvironmentName, Stage;
  std::vector<std::string> Stages;
  std::unique_ptr<ModuleBindings> Modules;
  std::shared_ptr<ExecutionBudget> Budget;
  std::vector<ModuleRange> Ranges;
  std::optional<EnvironmentAction> Deferred;
  std::optional<ResourcePause> Continuation;
  std::unique_ptr<GuestInvocation> Invocation;
  std::string InvocationStage;
  ExecutionRunEvent Last;
  std::atomic<ExecutionRunState> Current{ExecutionRunState::Ready};
  std::atomic<bool> PauseRequested{false};
  bool Busy = false;
  // Destroy hooks before everything they observe.
  std::unique_ptr<ExecutionSession> Session;
};
} // namespace neverd::emulation
#endif
