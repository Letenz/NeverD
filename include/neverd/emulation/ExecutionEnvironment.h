//===- ExecutionEnvironment.h - Stopped scene actions ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Stopped scene actions.
///

#ifndef NEVERD_EMULATION_EXECUTIONENVIRONMENT_H
#define NEVERD_EMULATION_EXECUTIONENVIRONMENT_H
#include "neverd/emulation/ExecutionSession.h"
#include "neverd/emulation/GuestInvocation.h"
#include "neverd/emulation/ModuleBinding.h"

namespace neverd::emulation {
enum class ExecutionRunReason {
#define NEVERD_RUN_REASON(Name, Text) Name,
#include "neverd/emulation/ExecutionRun.def"
#undef NEVERD_RUN_REASON
};
enum class EnvironmentActionKind {
#define NEVERD_ENVIRONMENT_ACTION(Name, Text) Name,
#include "neverd/emulation/ExecutionRun.def"
#undef NEVERD_ENVIRONMENT_ACTION
};
struct EnvironmentAction {
  EnvironmentActionKind Kind = EnvironmentActionKind::Failed;
  uint64_t PC = 0;
  std::string Stage, Diagnostic;
  /// Only Failed actions may select a failure reason. Completion, pause and
  /// resource reasons are rejected; omission retains EnvironmentFailure.
  std::optional<ExecutionRunReason> FailureReason;
};
/// Own the scene's existing OS objects, loader continuations, scheduler and
/// phase state. ExecutionRun alone calls Session.run. Stopped methods may use
/// the CPU, configure watches and handle owned service/OS exception transfers;
/// they must not execute CPUs, replace hooks or acknowledge resource pauses.
/// Resource and stage yields preserve this same object for the next action.
class ExecutionEnvironment {
public:
  virtual ~ExecutionEnvironment() = default;
  virtual std::string name() const = 0;
  virtual std::vector<std::string> stageNames() const { return {}; }
  /// Called once before initialize to attach scene policy to the single
  /// session. Closures must retain this environment's lifetime, not factory
  /// locals. Session owns hook installation and instruction accounting.
  virtual SessionModelHooks modelHooks() { return {}; }
  virtual llvm::Error initialize(ExecutionSession &, ModuleBindings &) {
    return llvm::Error::success();
  }
  virtual llvm::Expected<EnvironmentAction> next(ExecutionSession &,
                                                 ModuleBindings &) = 0;
  /// Called after each CPU result, including resource stops and limits. Handle
  /// services/exceptions exactly once; leave a resource continuation untouched.
  /// Do not capture/restore a CPU context while its resource stop is pending.
  virtual llvm::Error afterCPUExit(ExecutionSession &, const SessionExit &) = 0;
  virtual llvm::Expected<ModuleMappingPlan>
  buildModule(const ModuleBinding &, const modules::ModuleImage &) = 0;
  virtual ModulePublicationPreparer modulePublicationPreparer() { return {}; }
  virtual ModuleMappingFinalizer moduleMappingFinalizer() { return {}; }
  virtual ModuleDependencyMatcher moduleDependencyMatcher() { return {}; }
  /// Stopped preparation after eager mapping and before an ordinary CPU
  /// action. May publish adapter metadata/dependencies, never execute a CPU or
  /// alter its retained action. ModuleDependencyError yields a resource request
  /// through the run; repaired CPU continuations skip this hook.
  virtual llvm::Error prepareExecution(ExecutionSession &, ModuleBindings &) {
    return llvm::Error::success();
  }
  /// Refresh OS projections of committed memory immediately before every CPU
  /// entry, including a repaired resource continuation. Never run a CPU,
  /// change its registers/context, acknowledge a resource stop or replay the
  /// retained action. Failure is terminal; it does not request a resource or
  /// roll back already published modules. Snapshots do not invoke this hook.
  virtual llvm::Error prepareCPUEntry(ExecutionSession &) {
    return llvm::Error::success();
  }
  /// Pure stopped admission at a selected stage. Check ABI/target capabilities,
  /// thread/IRQL/loader state and provide a private mapped stack/return
  /// boundary. No guest/environment mutation; default environments do not
  /// permit calls.
  virtual llvm::Expected<GuestInvocationPlan>
  planInvocation(ExecutionSession &, ModuleBindings &,
                 const GuestInvocationRequest &, uint64_t Entry,
                 llvm::StringRef Stage) {
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "environment does not support guest invocation");
  }
  /// Called after ABI installation. Save the canonical caller/model frame and
  /// enter a nested frame for next()/afterCPUExit(); never run a CPU here.
  virtual llvm::Error beginInvocation(ExecutionSession &,
                                      const GuestInvocationFrame &) {
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "environment cannot begin guest invocation");
  }
  /// Called at a validated return, before CPU/caller-control restoration.
  /// Validate thread/model state and restore canonical caller/phase ownership,
  /// leaving CPU/session restoration to the run.
  /// afterCPUExit must leave InvocationReturn's CPU registers untouched.
  virtual llvm::Error finishInvocation(ExecutionSession &,
                                       const GuestInvocationResult &) {
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "environment cannot finish guest invocation");
  }
  /// Pure in-CPU decisions. Return true from observeInstruction to yield to the
  /// stopped environment; these methods cannot do IO or mutate/execute a CPU.
  virtual bool observeInstruction(ExecutionBackend &, uint64_t, uint32_t) {
    return false;
  }
  virtual bool admitGuestException(ExecutionBackend &, const BackendFault &) {
    return false;
  }
  /// A known OS-object bounds/state error must not become a missing file.
  virtual bool admitResourcePause(const ResourcePause &) const { return false; }
};
} // namespace neverd::emulation
#endif
