//===- ExecutionSession.cpp - Typed execution and continuation ownership -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/emulation/ExecutionSession.h"

#include "../core/ExecutionDiagnostics.h"
#include "RuntimeValues.h"

#include "llvm/Support/ErrorHandling.h"

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
        if (!S->Budget->hasInstructions())
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
