//===- ExecutionSession.cpp - Typed execution and continuation ownership -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/emulation/ExecutionSession.h"

#include "../core/ExecutionDeadline.h"
#include "../core/ExecutionDiagnostics.h"
#include "RuntimeValues.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>

namespace neverd::emulation {
llvm::Expected<std::unique_ptr<ExecutionSession>> ExecutionSession::create(
    std::unique_ptr<ExecutionBackend> CPU,
    std::shared_ptr<ExecutionBudget> Budget,
    std::function<bool(const BackendFault &)> Recovery,
    std::function<void(uint64_t, uint32_t)> InstructionObserver,
    std::function<bool(const ResourcePause &)> ResourcePolicy,
    SessionModelHooks ModelHooks) {
  if (!CPU || !Budget)
    return diagnostic::error(runtime::SessionResources);
  if (ResourcePolicy && !CPU->supportsResourcePause())
    return diagnostic::error(diagnostic::ResourcePauseUnsupported);
  auto Session = std::unique_ptr<ExecutionSession>(
      new ExecutionSession(std::move(CPU), std::move(Budget)));
  Session->InstructionObserver = std::move(InstructionObserver);
  Session->HasModelAdmission = bool(ModelHooks.BeforeInstruction);
  BackendHooks Hooks;
  Hooks.Instruction = [S = Session.get(),
                       Before = std::move(ModelHooks.BeforeInstruction)](
                          uint64_t PC, uint32_t Size) {
    // A resumed watch stop executes exactly its own instruction.
    const bool Resumed = S->WatchResume == PC;
    S->WatchResume.reset();
    if (S->ReturnPC == PC) {
      S->AdmissionStop = SessionExitKind::InvocationReturn;
    } else if (!Resumed && S->watched(PC)) {
      // A positive quantum ends before observing another thread's watch.
      // Model admission below can still finish an uncharged boundary at the
      // exact instruction limit; it does not transfer watch ownership.
      if (Size && S->Quantum && !S->Budget->hasInstructions())
        S->AdmissionStop = SessionExitKind::InstructionLimit;
      else if (Size && S->Quantum && S->Admitted == S->Quantum)
        S->AdmissionStop = SessionExitKind::Quantum;
      else {
        S->WatchedPC = PC;
        S->AdmissionStop = SessionExitKind::ExecutionWatch;
      }
    } else {
      const auto Decision =
          Before ? Before(PC, Size) : InstructionAdmission::Execute;
      if (Decision == InstructionAdmission::Stop)
        S->AdmissionStop = SessionExitKind::ModelBoundary;
      else if (!Size)
        // A direct watch handoff is not an admitted instruction. On resume,
        // the CPU retires one instruction before rearming its page overlay.
        return;
      else if (!S->Budget->hasInstructions())
        S->AdmissionStop = SessionExitKind::InstructionLimit;
      else if (S->Admitted == S->Quantum)
        S->AdmissionStop = SessionExitKind::Quantum;
      else if (Decision == InstructionAdmission::Defer) {
        S->DeferredInstruction = Instruction{PC, Size};
        S->AdmissionStop = SessionExitKind::DeferredInstruction;
      } else if (!S->Budget->consumeInstructions())
        S->AdmissionStop = SessionExitKind::InstructionLimit;
      else {
        ++S->Admitted;
        if (S->InstructionObserver)
          S->InstructionObserver(PC, Size);
        if (Decision == InstructionAdmission::Emulate)
          S->AdmissionStop = SessionExitKind::ModelBoundary;
      }
    }
    if (S->AdmissionStop)
      S->CPU->stop();
  };
  Hooks.RecoverableFault = std::move(Recovery);
  Hooks.ResourcePause = std::move(ResourcePolicy);
  Hooks.Read = std::move(ModelHooks.Read);
  Hooks.Write = std::move(ModelHooks.Write);
  Hooks.Fault = std::move(ModelHooks.Fault);
  Hooks.Interrupt = std::move(ModelHooks.Interrupt);
  Hooks.InvalidInstruction = std::move(ModelHooks.InvalidInstruction);
  Hooks.MemoryWritten = [S = Session.get()] {
    S->AdmissionStop = SessionExitKind::MemoryWriteWatch;
    S->CPU->stop();
  };
  if (auto E = Session->CPU->installHooks(std::move(Hooks)))
    return std::move(E);
  return Session;
}

llvm::Expected<SessionExit> ExecutionSession::run(uint64_t PC,
                                                  uint64_t InstructionQuantum,
                                                  uint64_t MaxMicroseconds) {
  if (Current != State::Ready && Current != State::Continuation)
    return diagnostic::error(runtime::SessionState);
  if (!InstructionQuantum && !allowsUnchargedBoundaries())
    return diagnostic::error(runtime::SessionQuantum);
  if (ResourceResumePC && *ResourceResumePC != PC)
    return diagnostic::error(diagnostic::ResourceResumePC);
  // A stopped model operation may have faulted the CPU since its last run.
  // Use the same zero-span state preflight as retained backing access before
  // considering an exhausted budget; resource limits must not hide that fault.
  if (Current != State::Continuation)
    if (auto E = CPU->validateBacking(0, 0))
      return std::move(E);
  auto Work = Budget->enterWork();
  if (!Work)
    return Work.takeError();
  const uint64_t Remaining =
      std::min(Budget->remainingMicroseconds(), MaxMicroseconds);
  if (!Remaining)
    return SessionExit{SessionExitKind::Timeout, std::nullopt};
  if (!allowsUnchargedBoundaries() && !Budget->hasInstructions())
    return SessionExit{SessionExitKind::InstructionLimit, std::nullopt};
  auto Deadline = makeExecutionDeadline(Remaining);
  if (!Deadline)
    return Deadline.takeError();
  InstructionDeadline = *Deadline;
  DeferredInstruction.reset();
  Quantum = InstructionQuantum;
  Admitted = 0;
  AdmissionStop.reset();
  // Only a continuation at the reported PC steps over its own watch.
  if (WatchedPC != PC)
    WatchResume.reset();
  else
    WatchResume = WatchedPC;
  const auto SteppingWatch = WatchResume;
  WatchedPC.reset();
  Current = State::Running;
  auto Exit = CPU->runUntilExit(PC, Remaining);
  if (!Exit) {
    Current = State::Terminal;
    return Exit.takeError();
  }
  ResourceResumePC.reset();
  auto Kind = SessionExitKind::CPU;
  switch (Exit->Kind) {
  case ExecutionExitKind::Stopped:
    Kind = AdmissionStop.value_or(SessionExitKind::CPU);
    Current = Kind == SessionExitKind::DeferredInstruction ? State::Deferred
                                                           : State::Ready;
    break;
  case ExecutionExitKind::Deadline:
    Current = State::Ready;
    Kind = SessionExitKind::Timeout;
    break;
  case ExecutionExitKind::ServiceRequest:
    Current = State::Service;
    break;
  case ExecutionExitKind::RecoverableFault:
    Current = State::Recoverable;
    break;
  case ExecutionExitKind::ResourcePause:
    // Preserve an already selected watch step-over through mapping repair.
    // A newly reached fetch target must still stop at its own watch.
    if (Exit->Resource && SteppingWatch == Exit->Resource->ResumePC)
      WatchedPC = SteppingWatch;
    Current = State::Resource;
    break;
  default:
    Current = State::Terminal;
    break;
  }
  if (Current != State::Deferred) {
    DeferredInstruction.reset();
    InstructionDeadline.reset();
  }
  return SessionExit{Kind, std::move(*Exit)};
}

llvm::Expected<std::optional<SessionExitKind>>
ExecutionSession::admitDeferredInstruction() {
  if (Current != State::Deferred || !DeferredInstruction ||
      !InstructionDeadline)
    return diagnostic::error(runtime::SessionDeferred);
  if (auto E = CPU->validateBacking(0, 0))
    return std::move(E);
  const auto PCRegister = CPU->architecture() == GuestArchitecture::X64
                              ? CPURegister::X64PC
                              : CPURegister::AArch64PC;
  auto PC = CPU->readRegister(PCRegister);
  if (!PC)
    return PC.takeError();
  if ((*PC)[0] != DeferredInstruction->PC)
    return diagnostic::error(runtime::SessionDeferredPC);
  auto Work = Budget->enterWork();
  if (!Work)
    return Work.takeError();
  if (!Budget->remainingMicroseconds() ||
      ExecutionBudget::Clock::now() >= *InstructionDeadline)
    return std::optional(SessionExitKind::Timeout);
  if (!Budget->hasInstructions())
    return std::optional(SessionExitKind::InstructionLimit);
  if (Admitted == Quantum)
    return std::optional(SessionExitKind::Quantum);
  if (!Budget->consumeInstructions())
    return std::optional(SessionExitKind::InstructionLimit);
  const auto Pending = *DeferredInstruction;
  DeferredInstruction.reset();
  InstructionDeadline.reset();
  AdmissionStop.reset();
  ++Admitted;
  // Match the in-CPU observer's reentry guard at this stopped admission point.
  Current = State::Running;
  try {
    if (InstructionObserver)
      InstructionObserver(Pending.PC, Pending.Size);
  } catch (...) {
    Current = State::Terminal;
    return diagnostic::error(runtime::SessionObserver);
  }
  Current = State::Ready;
  return std::optional<SessionExitKind>();
}
llvm::Error ExecutionSession::acknowledgeResourcePause() {
  if (Current != State::Resource)
    return diagnostic::error(diagnostic::ResourcePauseMissing);
  const auto Pending = CPU->pendingResourcePause();
  if (!Pending)
    return diagnostic::error(diagnostic::ResourcePauseMissing);
  if (auto E = CPU->acknowledgeResourcePause())
    return E;
  ResourceResumePC = Pending->ResumePC;
  Current = State::Continuation;
  return llvm::Error::success();
}

bool ExecutionSession::watched(uint64_t PC) const {
  auto I =
      llvm::upper_bound(Watches, PC, [](uint64_t PC, const ExecutionWatch &W) {
        return PC < W.Address;
      });
  return I != Watches.begin() &&
         PC - std::prev(I)->Address < std::prev(I)->Size;
}

namespace {
template <typename Watch>
llvm::Expected<std::vector<Watch>> normalizeWatches(std::vector<Watch> New) {
  for (const auto &W : New)
    if (!W.Size || W.Size - 1 > UINT64_MAX - W.Address)
      return diagnostic::error(runtime::SessionWatch);
  const auto Before = [](const Watch &A, const Watch &B) {
    return A.Address < B.Address;
  };
  if (!llvm::is_sorted(New, Before))
    llvm::sort(New, Before);
  size_t Count = 0;
  for (const Watch W : New) {
    // The last byte is representable even when the exclusive end is not.
    if (Count && W.Address - New[Count - 1].Address <= New[Count - 1].Size) {
      auto &LastRange = New[Count - 1];
      const uint64_t Last = std::max(LastRange.Address + (LastRange.Size - 1),
                                     W.Address + (W.Size - 1));
      LastRange.Size = Last - LastRange.Address + 1;
      // A range covering the whole address space cannot state its size.
      if (!LastRange.Size)
        return diagnostic::error(runtime::SessionWatch);
    } else
      New[Count++] = W;
  }
  New.resize(Count);
  return New;
}
} // namespace

namespace {
llvm::Expected<std::vector<ExecutionWatch>>
projectedWatches(const std::vector<ExecutionWatch> &Watches,
                 std::optional<uint64_t> ReturnPC) {
  auto Result = Watches;
  if (ReturnPC)
    Result.push_back({*ReturnPC, 1});
  return normalizeWatches(std::move(Result));
}
} // namespace

llvm::Expected<std::unique_ptr<SessionContext>>
ExecutionSession::saveContext() {
  if (Current != State::Ready)
    return diagnostic::error(runtime::SessionState);
  auto Saved = std::unique_ptr<SessionContext>(new SessionContext());
  Saved->Owner = ContextOwner;
  Saved->Watches = Watches;
  Saved->WriteWatches = WriteWatches;
  Saved->WatchedPC = WatchedPC;
  Saved->WatchResume = WatchResume;
  Saved->ReturnPC = ReturnPC;
  auto Registers = CPU->saveContext();
  if (!Registers)
    return Registers.takeError();
  Saved->CPU = std::move(*Registers);
  return Saved;
}

llvm::Error ExecutionSession::restoreContext(const SessionContext &Saved) {
  if (Current != State::Ready)
    return diagnostic::error(runtime::SessionState);
  if (Saved.Owner.lock() != ContextOwner)
    return diagnostic::error(runtime::SessionContext);
  auto Projected = projectedWatches(Saved.Watches, Saved.ReturnPC);
  if (!Projected)
    return Projected.takeError();
  auto RestoredWatches = Saved.Watches;
  auto RestoredWrites = Saved.WriteWatches;
  if (auto E = CPU->restoreContext(*Saved.CPU)) {
    Current = State::Terminal;
    return E;
  }
  if (auto E = CPU->setMemoryWriteWatches(RestoredWrites)) {
    Current = State::Terminal;
    return E;
  }
  CPU->setExecutionWatches(*Projected);
  Watches = std::move(RestoredWatches);
  WriteWatches = std::move(RestoredWrites);
  WatchedPC = Saved.WatchedPC;
  WatchResume = Saved.WatchResume;
  ReturnPC = Saved.ReturnPC;
  return llvm::Error::success();
}

llvm::Error ExecutionSession::watchReturn(uint64_t PC) {
  if (Current != State::Ready)
    return diagnostic::error(runtime::SessionState);
  if (auto E = CPU->validateBacking(0, 0))
    return E;
  if (ReturnPC)
    return diagnostic::error(runtime::SessionReturn);
  auto Projected = projectedWatches(Watches, PC);
  if (!Projected)
    return Projected.takeError();
  ReturnPC = PC;
  AdmissionStop.reset();
  WatchedPC.reset();
  WatchResume.reset();
  CPU->setExecutionWatches(*Projected);
  return llvm::Error::success();
}

llvm::Error ExecutionSession::watchExecution(std::vector<ExecutionWatch> New) {
  if (Current != State::Ready)
    return diagnostic::error(runtime::SessionState);
  auto Merged = normalizeWatches(std::move(New));
  if (!Merged)
    return Merged.takeError();
  auto Projected = projectedWatches(*Merged, ReturnPC);
  if (!Projected)
    return Projected.takeError();
  Watches = std::move(*Merged);
  // A direct contract enforces the watches in the CPU's page tables, so the
  // backend needs the merged set; the checked path reads Watches directly.
  CPU->setExecutionWatches(*Projected);
  return llvm::Error::success();
}

llvm::Error
ExecutionSession::watchMemoryWrites(std::vector<MemoryWriteWatch> New) {
  if (Current != State::Ready)
    return diagnostic::error(runtime::SessionState);
  auto Merged = normalizeWatches(std::move(New));
  if (!Merged)
    return Merged.takeError();
  if (auto E = CPU->setMemoryWriteWatches(*Merged))
    return E;
  WriteWatches = std::move(*Merged);
  return llvm::Error::success();
}

llvm::Expected<ServiceRequest> ExecutionSession::takeServiceRequest() {
  if (Current != State::Service)
    return diagnostic::error(runtime::SessionService);
  auto Request = CPU->takeServiceRequest();
  if (!Request) {
    Current = State::Terminal;
    return diagnostic::error(runtime::SessionService);
  }
  Current = State::Ready;
  return *Request;
}
llvm::Expected<BackendFault> ExecutionSession::takeRecoverableFault() {
  if (Current != State::Recoverable)
    return diagnostic::error(runtime::SessionFault);
  auto Fault = CPU->takeRecoverableFault();
  if (!Fault) {
    Current = State::Terminal;
    return diagnostic::error(runtime::SessionFault);
  }
  Current = State::Ready;
  return *Fault;
}
const char *sessionExitKindName(SessionExitKind Kind) {
  switch (Kind) {
#define NEVERD_SESSION_EXIT(Name, Text)                                        \
  case SessionExitKind::Name:                                                  \
    return Text;
#include "neverd/emulation/ExecutionSession.def"
#undef NEVERD_SESSION_EXIT
  }
  llvm_unreachable(runtime::SessionExit);
}
} // namespace neverd::emulation
