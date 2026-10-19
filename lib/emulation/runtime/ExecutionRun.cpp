//===- ExecutionRun.cpp - Persistent execution control -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Persistent execution control.
///

#include "neverd/emulation/ExecutionRun.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <set>

namespace neverd::emulation {
namespace {
llvm::Error failure(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
bool name(llvm::StringRef S) {
  return !S.empty() && S.size() <= execution_limit::NameBytes &&
         S.find('\0') == llvm::StringRef::npos;
}
bool contains(const ModuleRange &R, uint64_t A, uint64_t N) {
  return N && A >= R.Base && A - R.Base < R.Size && N <= R.Size - (A - R.Base);
}
} // namespace
const char *executionRunStateName(ExecutionRunState State) {
  switch (State) {
#define NEVERD_RUN_STATE(Name, Text)                                           \
  case ExecutionRunState::Name:                                                \
    return Text;
#include "neverd/emulation/ExecutionRun.def"
#undef NEVERD_RUN_STATE
  }
  llvm_unreachable("invalid execution run state");
}
const char *executionRunReasonName(ExecutionRunReason Reason) {
  switch (Reason) {
#define NEVERD_RUN_REASON(Name, Text)                                          \
  case ExecutionRunReason::Name:                                               \
    return Text;
#include "neverd/emulation/ExecutionRun.def"
#undef NEVERD_RUN_REASON
  }
  llvm_unreachable("invalid execution run reason");
}
ExecutionRun::ExecutionRun(std::unique_ptr<ExecutionEnvironment> Environment,
                           ExecutionRunOptions Options)
    : Environment(std::move(Environment)), Options(std::move(Options)) {}
ExecutionRun::~ExecutionRun() = default;
llvm::Expected<std::unique_ptr<ExecutionRun>>
ExecutionRun::create(std::unique_ptr<ExecutionBackend> CPU,
                     std::shared_ptr<modules::ModuleSet> Sources,
                     std::unique_ptr<ExecutionEnvironment> Environment,
                     ExecutionRunOptions Options,
                     std::shared_ptr<ExecutionBudget> SharedBudget) {
  if (!CPU || !Sources || !Environment || !CPU->addressSpace() ||
      !Options.InstructionQuantum || !Options.ActionsPerRun ||
      Options.ActionsPerRun > execution_limit::Actions ||
      !Options.MemoryTransferLimit ||
      Options.MemoryTransferLimit > execution_limit::TransferBytes ||
      Options.PauseStages.size() > execution_limit::Stages ||
      Options.Bindings.size() > Options.InstanceLimit)
    return failure("invalid persistent execution resources or bounds");
  if (Options.EnableResourcePauses && !CPU->supportsResourcePause())
    return failure("selected CPU does not support resource pauses");
  if (SharedBudget) {
    const auto &Limits = SharedBudget->limits();
    if (Limits.Instructions != Options.Limits.Instructions ||
        Limits.Events != Options.Limits.Events ||
        Limits.TimeoutMicroseconds != Options.Limits.TimeoutMicroseconds ||
        SharedBudget->timePolicy() != Options.TimePolicy)
      return failure(
          "supplied workload budget does not match execution options");
  } else {
    auto Budget = ExecutionBudget::createWithTimePolicy(Options.Limits,
                                                        Options.TimePolicy);
    if (!Budget)
      return Budget.takeError();
    SharedBudget = std::move(*Budget);
  }
  auto Run = std::unique_ptr<ExecutionRun>(
      new ExecutionRun(std::move(Environment), std::move(Options)));
  Run->Budget = std::move(SharedBudget);
  try {
    Run->EnvironmentName = Run->Environment->name();
    Run->Stages = Run->Environment->stageNames();
    if (!name(Run->EnvironmentName) ||
        Run->Stages.size() > execution_limit::Stages)
      return failure("invalid environment name or stage inventory");
    std::set<std::string> StageSet;
    for (const auto &S : Run->Stages)
      if (!name(S) || !StageSet.insert(S).second)
        return failure("invalid or duplicate environment stage");
    std::set<std::string> Requested;
    for (const auto &S : Run->Options.PauseStages)
      if (!StageSet.count(S) || !Requested.insert(S).second)
        return failure("unknown or duplicate requested stage");
    auto Modules =
        ModuleBindings::create(Sources, Run->Options.InstanceLimit,
                               Run->Environment->modulePublicationPreparer(),
                               Run->Environment->moduleMappingFinalizer(),
                               Run->Environment->moduleDependencyMatcher());
    if (!Modules)
      return Modules.takeError();
    Run->Modules = std::move(*Modules);
    for (auto &B : Run->Options.Bindings) {
      if (!B.Space)
        B.Space = CPU->addressSpace();
      if (B.Space->physicalMemory() != CPU->addressSpace()->physicalMemory())
        return failure(
            "module address space belongs to another physical owner");
      if (auto E = Run->Modules->declare(B))
        return std::move(E);
    }
    auto Session = ExecutionSession::create(
        std::move(CPU), Run->Budget,
        [R = Run.get()](const BackendFault &F) {
          return R->Environment->admitGuestException(R->Session->cpu(), F);
        },
        [R = Run.get()](uint64_t PC, uint32_t Size) {
          if (R->Environment->observeInstruction(R->Session->cpu(), PC, Size))
            R->Session->stop();
        },
        Run->Options.EnableResourcePauses
            ? std::function<bool(const ResourcePause &)>(
                  [R = Run.get()](const ResourcePause &P) {
                    return R->admitResource(P);
                  })
            : std::function<bool(const ResourcePause &)>(),
        Run->Environment->modelHooks());
    if (!Session)
      return Session.takeError();
    Run->Session = std::move(*Session);
    auto Work = Run->Budget->enterWork();
    if (!Work)
      return Work.takeError();
    if (auto E = Run->Environment->initialize(*Run->Session, *Run->Modules))
      return std::move(E);
    if (!Run->Session->readyToRun() || Run->Session->cpu().fault())
      return failure(
          "environment initialization consumed an execution outcome");
  } catch (...) {
    return failure("environment initialization raised a host exception");
  }
  return Run;
}
ModuleMappingBuilder ExecutionRun::builder() {
  return [this](const ModuleBinding &B, const modules::ModuleImage &I) {
    return Environment->buildModule(B, I);
  };
}
bool ExecutionRun::admitResource(const ResourcePause &P) const {
  if (!Environment->admitResourcePause(P) || !P.Fault.Address || !P.Fault.Size)
    return false;
  if (Invocation) {
    const auto &F = Invocation->frame();
    const auto A = *P.Fault.Address, N = *P.Fault.Size;
    if ((A <= F.StackBase ? F.StackBase - A < N
                          : A - F.StackBase < F.StackSize) ||
        contains({A, N, false}, F.ReturnPC, 1))
      return false;
  }
  for (const auto &R : Ranges)
    if (contains(R, *P.Fault.Address, *P.Fault.Size))
      return !R.Mapped;
  return Options.PauseUnknownAddresses;
}
ExecutionRunEvent ExecutionRun::publish(ExecutionRunState State,
                                        ExecutionRunReason Reason,
                                        std::string Diagnostic) {
  Last = {State, Reason, Stage, std::move(Diagnostic), {}, {}, {}, {}};
  Current.store(State);
  return Last;
}
ExecutionRunEvent ExecutionRun::fail(ExecutionRunReason Reason,
                                     std::string Text) {
  if (Modules)
    if (auto Pending = Modules->pendingRequest())
      llvm::consumeError(Modules->cancel(Pending->Token));
  return publish(ExecutionRunState::Failed, Reason, std::move(Text));
}
std::optional<ExecutionRunEvent>
ExecutionRun::budgetFailure(bool RequireInstructions) {
  if (!Budget->remainingMicroseconds())
    return fail(ExecutionRunReason::Timeout,
                "workload active/deadline time exhausted");
  if (RequireInstructions && !Budget->hasInstructions())
    return fail(ExecutionRunReason::InstructionLimit,
                "workload instruction limit exhausted");
  if (Invocation) {
    const auto P = Invocation->progress(*Budget);
    if (!P.RemainingMicroseconds)
      return fail(ExecutionRunReason::Timeout,
                  "guest invocation time limit exhausted");
    if (!P.RemainingEvents)
      return fail(ExecutionRunReason::EventLimit,
                  "guest invocation event limit exhausted");
    if (RequireInstructions && !P.RemainingInstructions)
      return fail(ExecutionRunReason::InstructionLimit,
                  "guest invocation instruction limit exhausted");
  }
  return std::nullopt;
}
llvm::Expected<ExecutionRunEvent> ExecutionRun::awaitResource(
    ModuleRequest Request, std::optional<ResourcePause> Resource,
    std::optional<SessionExit> CPU, std::string Diagnostic) {
  if (budgetFailure(bool(Resource))) {
    Last.CPU = std::move(CPU);
    return Last;
  }
  if (!Budget->consumeEvents()) {
    fail(ExecutionRunReason::EventLimit, "resource event limit exhausted");
    Last.CPU = std::move(CPU);
    return Last;
  }
  publish(ExecutionRunState::AwaitingResource, ExecutionRunReason::Resource,
          std::move(Diagnostic));
  Last.Request = std::move(Request);
  Last.Resource = std::move(Resource);
  Last.CPU = std::move(CPU);
  return Last;
}
llvm::Expected<std::optional<ExecutionRunEvent>>
ExecutionRun::prepareEager(const EnvironmentAction &Action) {
  const auto Space = Session->cpu().addressSpace();
  const auto Eager =
      Modules->unmappedBindings(Space, modules::LoadPolicy::Eager);
  for (const auto &B : Eager) {
    auto Source = Modules->sources().prepare(B.ModuleID);
    if (!Source) {
      const auto Text = llvm::toString(Source.takeError());
      auto Request = Modules->requestModule(B.ID);
      if (!Request)
        return Request.takeError();
      Deferred = Action;
      auto Event = awaitResource(*Request, {}, {}, Text);
      if (!Event)
        return Event.takeError();
      return std::optional(std::move(*Event));
    }
  }
  auto Prepare = [&]() -> llvm::Error {
    if (!Eager.empty())
      if (auto E = Modules->prepareEager(Space, builder()))
        return E;
    return Environment->prepareExecution(*Session, *Modules);
  };
  if (auto E = Prepare()) {
    if (auto Failed = budgetFailure()) {
      llvm::consumeError(std::move(E));
      return Failed;
    }
    std::string Dependency, Detail;
    std::optional<std::string> ID;
    bool Missing = false;
    auto Other =
        llvm::handleErrors(std::move(E), [&](const ModuleDependencyError &D) {
          Missing = true;
          Dependency = D.Dependency;
          ID = D.InstanceID;
          Detail = D.Detail;
        });
    if (Missing && !Other) {
      auto Request =
          Modules->requestDependency(Space, Dependency, Action.PC, ID);
      if (!Request)
        return Request.takeError();
      Deferred = Action;
      auto Event = awaitResource(*Request, {}, {}, Detail);
      if (!Event)
        return Event.takeError();
      return std::optional(std::move(*Event));
    }
    if (Missing)
      Other = llvm::joinErrors(
          std::move(Other),
          llvm::make_error<ModuleDependencyError>(Dependency, ID, Detail));
    return std::optional(fail(ExecutionRunReason::EnvironmentFailure,
                              llvm::toString(std::move(Other))));
  }
  return std::optional<ExecutionRunEvent>();
}
llvm::Expected<std::optional<ExecutionRunEvent>>
ExecutionRun::executeCPU(const EnvironmentAction &Action,
                         bool ResourceContinuation) {
  if (auto Failed = budgetFailure(!Session->allowsUnchargedBoundaries()))
    return Failed;
  if (!ResourceContinuation) {
    auto Prepared = prepareEager(Action);
    if (!Prepared || *Prepared)
      return Prepared;
    if (auto Failed = budgetFailure(!Session->allowsUnchargedBoundaries()))
      return Failed;
  }
  if (auto E = Environment->prepareCPUEntry(*Session)) {
    if (auto Failed = budgetFailure(!Session->allowsUnchargedBoundaries())) {
      llvm::consumeError(std::move(E));
      return Failed;
    }
    return std::optional(fail(ExecutionRunReason::EnvironmentFailure,
                              llvm::toString(std::move(E))));
  }
  if (auto Failed = budgetFailure(!Session->allowsUnchargedBoundaries()))
    return Failed;
  Ranges = Modules->ranges(Session->cpu().addressSpace());
  if (PauseRequested.exchange(false)) {
    if (!ResourceContinuation)
      Deferred = Action;
    return std::optional(
        publish(ExecutionRunState::Paused, ExecutionRunReason::UserPause));
  }
  uint64_t Quantum = Options.InstructionQuantum, Timeout = UINT64_MAX;
  if (Invocation) {
    const auto P = Invocation->progress(*Budget);
    Quantum = std::min(Quantum, P.RemainingInstructions);
    Timeout = P.RemainingMicroseconds;
  }
  auto Exit = Session->run(Action.PC, Quantum, Timeout);
  if (!Exit)
    return Exit.takeError();
  // A derived zero call quantum still observes a model/reserved return, but
  // attempting another machine instruction exhausts that call's local bound.
  if (Invocation && !Quantum && Exit->Kind == SessionExitKind::Quantum)
    Exit->Kind = SessionExitKind::InstructionLimit;
  Continuation.reset();
  const auto FailExit = [&](ExecutionRunReason Reason, std::string Diagnostic) {
    fail(Reason, std::move(Diagnostic));
    Last.CPU = *Exit;
    return std::optional(Last);
  };
  if (auto E = Environment->afterCPUExit(*Session, *Exit))
    return FailExit(ExecutionRunReason::EnvironmentFailure,
                    llvm::toString(std::move(E)));
  if (Exit->CPU && Exit->CPU->Kind == ExecutionExitKind::ResourcePause) {
    if (!Exit->CPU->Resource || !Session->cpu().pendingResourcePause())
      return failure("CPU resource exit has no retained continuation");
    const auto Pause = *Exit->CPU->Resource;
    const auto &F = Pause.Fault;
    const auto Access = F.Access == BackendAccessKind::Read ? ModuleAccess::Read
                        : F.Access == BackendAccessKind::Write
                            ? ModuleAccess::Write
                            : ModuleAccess::Fetch;
    auto Request =
        Modules->requestAddress(Session->cpu().addressSpace(), F.PC, *F.Address,
                                *F.Size, Access, Pause.RequiredPermissions);
    if (!Request)
      return Request.takeError();
    auto Event = awaitResource(*Request, Pause, *Exit);
    if (!Event)
      return Event.takeError();
    if (Current.load() == ExecutionRunState::Failed)
      return std::optional(Last);
    if (Request->InstanceID) {
      const auto Instance = Modules->find(*Request->InstanceID);
      if (auto E = supplyImpl(Request->Token, {Instance->Binding})) {
        if (Current.load() != ExecutionRunState::Failed)
          Last.Diagnostic = llvm::toString(std::move(E));
        else
          llvm::consumeError(std::move(E));
        return std::optional(Last);
      }
      return std::optional<ExecutionRunEvent>();
    }
    return std::optional(Last);
  }
  if (Exit->Kind == SessionExitKind::Timeout)
    return FailExit(ExecutionRunReason::Timeout, "CPU time limit exhausted");
  if (Exit->Kind == SessionExitKind::InstructionLimit)
    return FailExit(ExecutionRunReason::InstructionLimit,
                    "CPU instruction limit exhausted");
  if (!Session->readyToRun() || Session->cpu().fault()) {
    const auto Text =
        Exit->CPU ? Exit->CPU->Diagnostic : "unhandled CPU continuation";
    return FailExit(ExecutionRunReason::GuestFailure, Text);
  }
  if (Exit->Kind == SessionExitKind::InvocationReturn) {
    if (!Invocation)
      return FailExit(ExecutionRunReason::EnvironmentFailure,
                      "environment returned without an active invocation");
    auto Finished = finishInvocation(*Exit);
    if (!Finished)
      return FailExit(ExecutionRunReason::GuestFailure,
                      llvm::toString(Finished.takeError()));
    return std::optional(std::move(*Finished));
  }
  return std::optional<ExecutionRunEvent>();
}
llvm::Expected<ExecutionRunEvent> ExecutionRun::run() {
  if (Busy || Current.load() != ExecutionRunState::Ready)
    return failure("execution run is not ready");
  Busy = true;
  auto Release = llvm::scope_exit([&] { Busy = false; });
  auto Work = Budget->enterWork();
  if (!Work)
    return fail(ExecutionRunReason::EnvironmentFailure,
                llvm::toString(Work.takeError()));
  Current.store(ExecutionRunState::Running);
  try {
    for (uint64_t N = 0; N < Options.ActionsPerRun; ++N) {
      if (auto Failed = budgetFailure())
        return *Failed;
      if (PauseRequested.exchange(false))
        return publish(ExecutionRunState::Paused,
                       ExecutionRunReason::UserPause);
      if (Continuation) {
        EnvironmentAction Action{
            EnvironmentActionKind::RunCPU, Continuation->ResumePC, Stage, {}};
        auto Event = executeCPU(Action, true);
        if (!Event)
          return fail(ExecutionRunReason::EnvironmentFailure,
                      llvm::toString(Event.takeError()));
        if (*Event)
          return **Event;
        continue;
      }
      auto Action =
          Deferred ? llvm::Expected<EnvironmentAction>(std::move(*Deferred))
                   : Environment->next(*Session, *Modules);
      Deferred.reset();
      if (!Action)
        return fail(ExecutionRunReason::EnvironmentFailure,
                    llvm::toString(Action.takeError()));
      if (auto Failed = budgetFailure())
        return *Failed;
      if (!name(Action->Stage))
        return fail(ExecutionRunReason::EnvironmentFailure,
                    "environment action has an invalid stage");
      if (Action->FailureReason &&
          Action->Kind != EnvironmentActionKind::Failed)
        return fail(ExecutionRunReason::EnvironmentFailure,
                    "nonfailed environment action selected a failure reason");
      Stage = Action->Stage;
      if (Invocation && (Action->Kind == EnvironmentActionKind::Completed ||
                         Action->Kind == EnvironmentActionKind::StageBoundary))
        return fail(
            ExecutionRunReason::EnvironmentFailure,
            "environment completed a phase before the invocation returned");
      if (Modules->pendingRequest() &&
          Action->Kind != EnvironmentActionKind::AwaitingResource)
        return fail(ExecutionRunReason::EnvironmentFailure,
                    "environment did not publish its resource request");
      switch (Action->Kind) {
      case EnvironmentActionKind::RunCPU: {
        auto Event = executeCPU(*Action, false);
        if (!Event)
          return fail(ExecutionRunReason::EnvironmentFailure,
                      llvm::toString(Event.takeError()));
        if (*Event)
          return **Event;
        break;
      }
      case EnvironmentActionKind::StageBoundary:
        if (std::find(Stages.begin(), Stages.end(), Stage) == Stages.end())
          return fail(ExecutionRunReason::EnvironmentFailure,
                      "environment emitted an undeclared stage");
        if (std::find(Options.PauseStages.begin(), Options.PauseStages.end(),
                      Stage) != Options.PauseStages.end())
          return publish(ExecutionRunState::Paused, ExecutionRunReason::Stage);
        break;
      case EnvironmentActionKind::AwaitingResource:
        if (auto Request = Modules->pendingRequest())
          return awaitResource(*Request, {}, {}, Action->Diagnostic);
        return fail(ExecutionRunReason::EnvironmentFailure,
                    "environment has no resource request");
      case EnvironmentActionKind::Completed:
        return publish(ExecutionRunState::Completed,
                       ExecutionRunReason::Completed);
      case EnvironmentActionKind::Failed:
        switch (Action->FailureReason.value_or(
            ExecutionRunReason::EnvironmentFailure)) {
        case ExecutionRunReason::EnvironmentFailure:
        case ExecutionRunReason::GuestFailure:
        case ExecutionRunReason::InstructionLimit:
        case ExecutionRunReason::EventLimit:
        case ExecutionRunReason::Timeout:
          return fail(Action->FailureReason.value_or(
                          ExecutionRunReason::EnvironmentFailure),
                      Action->Diagnostic);
        default:
          return fail(ExecutionRunReason::EnvironmentFailure,
                      "invalid environment failure reason");
        }
      default:
        return fail(ExecutionRunReason::EnvironmentFailure,
                    "invalid environment action");
      }
    }
    return publish(ExecutionRunState::Paused, ExecutionRunReason::Quantum);
  } catch (...) {
    return fail(ExecutionRunReason::EnvironmentFailure,
                "environment raised a host exception");
  }
}
llvm::Error ExecutionRun::resume() {
  if (Busy || Current.load() != ExecutionRunState::Paused)
    return failure("execution run is not paused");
  PauseRequested.store(false);
  publish(ExecutionRunState::Ready, ExecutionRunReason::Resumed);
  return llvm::Error::success();
}
llvm::Error ExecutionRun::requestPause() {
  const auto State = Current.load();
  if (State == ExecutionRunState::Completed ||
      State == ExecutionRunState::Failed)
    return failure("terminal execution cannot be paused");
  if (State != ExecutionRunState::Paused) {
    PauseRequested.store(true);
    Session->stop();
  }
  return llvm::Error::success();
}
llvm::Error ExecutionRun::supplyImpl(ModuleRequestToken Token,
                                     llvm::ArrayRef<ModuleBinding> Bindings) {
  const auto Request = Modules->pendingRequest();
  if (Current.load() != ExecutionRunState::AwaitingResource || !Request ||
      Request->Token != Token)
    return failure("execution has no matching resource token");
  if (auto Failed = budgetFailure(bool(Last.Resource)))
    return failure(Failed->Diagnostic);
  if (!Budget->hasEvents()) {
    fail(ExecutionRunReason::EventLimit,
         "resource supply event limit exhausted");
    return failure(Last.Diagnostic);
  }
  std::vector<ModuleBinding> Inputs(Bindings.begin(), Bindings.end());
  for (auto &B : Inputs)
    if (!B.Space)
      B.Space = Request->Space;
  // A linker dependency paused the original CPU action before any mapping.
  // Retry its complete eager batch together with the explicit supplied inputs.
  if (Request->Kind == ModuleRequestKind::MissingDependency && Deferred &&
      !Last.Resource)
    for (const auto &B :
         Modules->unmappedBindings(Request->Space, modules::LoadPolicy::Eager))
      if (std::none_of(Inputs.begin(), Inputs.end(),
                       [&](const auto &I) { return I.ID == B.ID; }))
        Inputs.push_back(B);
  if (auto E = Modules->supply(Token, Inputs, builder()))
    return E;
  if (!Budget->consumeEvents()) {
    fail(ExecutionRunReason::EventLimit,
         "resource supply changed the reserved event budget");
    return failure(Last.Diagnostic);
  }
  if (auto Failed = budgetFailure(bool(Last.Resource)))
    return failure(Failed->Diagnostic);
  if (Last.Resource) {
    auto Pause = Last.Resource;
    if (auto E = Session->acknowledgeResourcePause()) {
      const auto Text = llvm::toString(std::move(E));
      fail(ExecutionRunReason::EnvironmentFailure, Text);
      return failure(Text);
    }
    Continuation = Pause;
  }
  publish(ExecutionRunState::Ready, ExecutionRunReason::ResourceSupplied);
  return llvm::Error::success();
}
llvm::Error ExecutionRun::supply(ModuleRequestToken Token,
                                 llvm::ArrayRef<ModuleBinding> Bindings) {
  if (Busy || Current.load() != ExecutionRunState::AwaitingResource)
    return failure("execution run is not awaiting a resource");
  Busy = true;
  auto Release = llvm::scope_exit([&] { Busy = false; });
  auto Work = Budget->enterWork();
  if (!Work)
    return Work.takeError();
  try {
    return supplyImpl(Token, Bindings);
  } catch (...) {
    fail(ExecutionRunReason::EnvironmentFailure,
         "resource supply raised a host exception");
    return failure(Last.Diagnostic);
  }
}
llvm::Expected<ModuleResolutionKind>
ExecutionRun::resolve(const ModuleResolver &Resolver) {
  if (Busy || Current.load() != ExecutionRunState::AwaitingResource ||
      !Resolver)
    return failure("resolver requires an available pending resource");
  const auto Request = Modules->pendingRequest();
  if (!Request)
    return failure("execution has no retained resource request");
  auto Work = Budget->enterWork();
  if (!Work)
    return Work.takeError();
  if (auto Failed = budgetFailure(bool(Last.Resource)))
    return failure(Failed->Diagnostic);
  auto Result = [&]() -> llvm::Expected<ModuleResolution> {
    Busy = true;
    auto Release = llvm::scope_exit([&] { Busy = false; });
    try {
      return Resolver(*Request);
    } catch (...) {
      return failure("resolver raised a host exception");
    }
  }();
  if (auto Failed = budgetFailure(bool(Last.Resource))) {
    if (!Result)
      llvm::consumeError(Result.takeError());
    return failure(Failed->Diagnostic);
  }
  if (!Result)
    return Result.takeError();
  switch (Result->Kind) {
  case ModuleResolutionKind::Found:
    if (auto E = supply(Request->Token, Result->Bindings))
      return std::move(E);
    return Result->Kind;
  case ModuleResolutionKind::NeedInput:
  case ModuleResolutionKind::NotFound:
    if (!Result->Bindings.empty())
      return failure("unresolved module resolution contains bindings");
    return Result->Kind;
  default:
    return failure("invalid module resolution kind");
  }
}
llvm::Error ExecutionRun::declareModule(ModuleBinding Binding) {
  const auto State = Current.load();
  if (Busy ||
      (State != ExecutionRunState::Ready &&
       State != ExecutionRunState::Paused) ||
      Continuation)
    return failure(
        "module declaration requires an available stopped execution");
  if (!Binding.Space)
    Binding.Space = Session->cpu().addressSpace();
  if (Binding.Space->physicalMemory() !=
      Session->cpu().addressSpace()->physicalMemory())
    return failure("module address space belongs to another physical owner");
  return Modules->declare(std::move(Binding));
}
llvm::Expected<ExecutionRunSnapshot> ExecutionRun::inspect() const {
  if (Busy || Current.load() == ExecutionRunState::Running)
    return failure("execution inspection requires a stopped boundary");
  return ExecutionRunSnapshot{
      Last,
      EnvironmentName,
      Budget->timePolicy(),
      Budget->instructions(),
      Budget->events(),
      Budget->remainingMicroseconds(),
      Modules->revision(),
      Continuation,
      Invocation ? std::optional(Invocation->progress(*Budget)) : std::nullopt};
}
llvm::Expected<std::vector<uint8_t>>
ExecutionRun::readMemory(uint64_t A, uint64_t N) const {
  if (Busy || Current.load() == ExecutionRunState::Running ||
      N > Options.MemoryTransferLimit || (N && N - 1 > UINT64_MAX - A))
    return failure("guest memory read requires a stopped, bounded RAM range");
  std::vector<uint8_t> Bytes(N);
  if (auto E = Session->cpu().addressSpace()->read(A, Bytes))
    return std::move(E);
  return Bytes;
}
llvm::Error ExecutionRun::writeMemory(uint64_t A,
                                      llvm::ArrayRef<uint8_t> Bytes) {
  const auto State = Current.load();
  if (Busy ||
      (State != ExecutionRunState::Ready &&
       State != ExecutionRunState::Paused &&
       State != ExecutionRunState::AwaitingResource) ||
      Bytes.size() > Options.MemoryTransferLimit ||
      (!Bytes.empty() && Bytes.size() - 1 > UINT64_MAX - A))
    return failure(
        "guest memory write requires an available bounded RAM range");
  return Session->cpu().addressSpace()->write(A, Bytes);
}
} // namespace neverd::emulation
