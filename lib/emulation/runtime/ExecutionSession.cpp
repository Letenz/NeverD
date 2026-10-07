//===- ExecutionSession.cpp - Typed execution and continuation ownership -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/emulation/ExecutionSession.h"

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
    std::function<void(uint64_t, uint32_t)> InstructionObserver) {
  if (!CPU || !Budget)
    return diagnostic::error(runtime::SessionResources);
  auto Session = std::unique_ptr<ExecutionSession>(
      new ExecutionSession(std::move(CPU), std::move(Budget)));
  BackendHooks Hooks;
  Hooks.Instruction =
      [S = Session.get(),
       Observer = std::move(InstructionObserver)](uint64_t PC, uint32_t Size) {
        // A resumed watch stop executes exactly its own instruction.
        const bool Resumed = S->WatchResume == PC;
        S->WatchResume.reset();
        if (!Resumed && S->watched(PC)) {
          S->WatchedPC = PC;
          S->AdmissionStop = SessionExitKind::ExecutionWatch;
        } else if (!Size)
          // A direct watch handoff is not an admitted instruction. On resume,
          // the CPU retires one instruction before rearming its page overlay.
          return;
        else if (!S->Budget->hasInstructions())
          S->AdmissionStop = SessionExitKind::InstructionLimit;
        else if (S->Admitted == S->Quantum)
          S->AdmissionStop = SessionExitKind::Quantum;
        else if (!S->Budget->consumeInstructions())
          S->AdmissionStop = SessionExitKind::InstructionLimit;
        else {
          ++S->Admitted;
          if (Observer)
            Observer(PC, Size);
        }
        if (S->AdmissionStop)
          S->CPU->stop();
      };
  Hooks.RecoverableFault = std::move(Recovery);
  Hooks.MemoryWritten = [S = Session.get()] {
    S->AdmissionStop = SessionExitKind::MemoryWriteWatch;
    S->CPU->stop();
  };
  if (auto E = Session->CPU->installHooks(std::move(Hooks)))
    return std::move(E);
  return Session;
}

llvm::Expected<SessionExit> ExecutionSession::run(uint64_t PC,
                                                  uint64_t InstructionQuantum) {
  if (Current != State::Ready)
    return diagnostic::error(runtime::SessionState);
  if (!InstructionQuantum)
    return diagnostic::error(runtime::SessionQuantum);
  // A stopped model operation may have faulted the CPU since its last run.
  // Use the same zero-span state preflight as retained backing access before
  // considering an exhausted budget; resource limits must not hide that fault.
  if (auto E = CPU->validateBacking(0, 0))
    return std::move(E);
  const uint64_t Remaining = Budget->remainingMicroseconds();
  if (!Remaining)
    return SessionExit{SessionExitKind::Timeout, std::nullopt};
  if (!Budget->hasInstructions())
    return SessionExit{SessionExitKind::InstructionLimit, std::nullopt};
  Quantum = InstructionQuantum;
  Admitted = 0;
  AdmissionStop.reset();
  // Only a continuation at the reported PC steps over its own watch.
  if (WatchedPC != PC)
    WatchResume.reset();
  else
    WatchResume = WatchedPC;
  WatchedPC.reset();
  auto Exit = CPU->runUntilExit(PC, Remaining);
  if (!Exit) {
    Current = State::Terminal;
    return Exit.takeError();
  }
  auto Kind = SessionExitKind::CPU;
  switch (Exit->Kind) {
  case ExecutionExitKind::Stopped:
    Kind = AdmissionStop.value_or(SessionExitKind::CPU);
    break;
  case ExecutionExitKind::Deadline:
    Kind = SessionExitKind::Timeout;
    break;
  case ExecutionExitKind::ServiceRequest:
    Current = State::Service;
    break;
  case ExecutionExitKind::RecoverableFault:
    Current = State::Recoverable;
    break;
  default:
    Current = State::Terminal;
    break;
  }
  return SessionExit{Kind, std::move(*Exit)};
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
  llvm::sort(New, [](const Watch &A, const Watch &B) {
    return A.Address < B.Address;
  });
  std::vector<Watch> Merged;
  for (const auto &W : New) {
    // The last byte is representable even when the exclusive end is not.
    if (!Merged.empty() &&
        W.Address - Merged.back().Address <= Merged.back().Size) {
      const uint64_t Last =
          std::max(Merged.back().Address + (Merged.back().Size - 1),
                   W.Address + (W.Size - 1));
      Merged.back().Size = Last - Merged.back().Address + 1;
      // A range covering the whole address space cannot state its size.
      if (!Merged.back().Size)
        return diagnostic::error(runtime::SessionWatch);
    } else
      Merged.push_back(W);
  }
  return Merged;
}
} // namespace

llvm::Error ExecutionSession::watchExecution(std::vector<ExecutionWatch> New) {
  if (Current != State::Ready)
    return diagnostic::error(runtime::SessionState);
  auto Merged = normalizeWatches(std::move(New));
  if (!Merged)
    return Merged.takeError();
  Watches = std::move(*Merged);
  // A direct contract enforces the watches in the CPU's page tables, so the
  // backend needs the merged set; the checked path reads Watches directly.
  CPU->setExecutionWatches(Watches);
  return llvm::Error::success();
}

llvm::Error
ExecutionSession::watchMemoryWrites(std::vector<MemoryWriteWatch> New) {
  if (Current != State::Ready)
    return diagnostic::error(runtime::SessionState);
  auto Merged = normalizeWatches(std::move(New));
  if (!Merged)
    return Merged.takeError();
  return CPU->setMemoryWriteWatches(*Merged);
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
