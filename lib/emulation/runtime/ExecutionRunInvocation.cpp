//===- ExecutionRunInvocation.cpp - Calls through the retained owner ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Calls through the retained owner.
///

#include "neverd/emulation/ExecutionRun.h"

#include "llvm/ADT/ScopeExit.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
llvm::Error failure(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
} // namespace
llvm::Error ExecutionRun::invoke(GuestInvocationRequest Request) {
  if (Busy || Current.load() != ExecutionRunState::Paused || Invocation ||
      Continuation || Deferred || Modules->pendingRequest() ||
      (Last.Reason != ExecutionRunReason::Stage &&
       Last.Reason != ExecutionRunReason::InvocationComplete))
    return failure("guest invocation requires an available selected stage");
  Busy = true;
  auto Release = llvm::scope_exit([&] { Busy = false; });
  auto Work = Budget->enterWork();
  if (!Work)
    return Work.takeError();
  try {
    if (auto E =
            GuestInvocation::validate(Request, Session->cpu().architecture(),
                                      Options.MemoryTransferLimit))
      return E;
    if (auto Failed = budgetFailure(true))
      return failure(Failed->Diagnostic);
    const GuestInvocation::AccountingStart Start{
        Budget->instructions(), Budget->events(),
        Budget->remainingMicroseconds()};
    uint64_t Entry = Request.Entry;
    if (!Request.InstanceID.empty()) {
      auto Instance = Modules->find(Request.InstanceID);
      if (!Instance || Instance->Binding.Space != Session->cpu().addressSpace())
        return failure(
            "guest invocation instance is unknown in this address space");
      const auto Base = Instance->mapped() ? std::optional(Instance->Base)
                                           : Instance->Binding.Base;
      const auto Size = Instance->mapped() ? std::optional(Instance->Size)
                                           : Instance->Binding.Size;
      if (!Base || !Size || Request.Entry >= *Size ||
          Request.Entry > UINT64_MAX - *Base)
        return failure("guest invocation RVA is outside its instance");
      Entry += *Base;
    }
    if (!Entry)
      return failure("guest invocation requires an explicit nonzero entry");
    auto Plan =
        Environment->planInvocation(*Session, *Modules, Request, Entry, Stage);
    if (auto Failed = budgetFailure(true)) {
      if (!Plan)
        llvm::consumeError(Plan.takeError());
      return failure(Failed->Diagnostic);
    }
    const auto Elapsed =
        Start.RemainingMicroseconds -
        std::min(Start.RemainingMicroseconds, Budget->remainingMicroseconds());
    if (Elapsed >= Request.Limits.TimeoutMicroseconds) {
      if (!Plan)
        llvm::consumeError(Plan.takeError());
      fail(ExecutionRunReason::Timeout,
           "guest invocation planning time limit exhausted");
      return failure(Last.Diagnostic);
    }
    if (!Plan)
      return Plan.takeError();
    auto Prepared =
        GuestInvocation::prepare(std::move(Request), *Plan, Entry, *Session,
                                 Options.MemoryTransferLimit, Start);
    if (!Prepared)
      return Prepared.takeError();
    if (!Budget->hasEvents()) {
      fail(ExecutionRunReason::EventLimit,
           "guest invocation start event limit exhausted");
      return failure(Last.Diagnostic);
    }
    Invocation = std::move(*Prepared);
    InvocationStage = Stage;
    if (auto Failed = budgetFailure(true))
      return failure(Failed->Diagnostic);
    Budget->consumeEvents();
    if (auto E = Invocation->install(*Session)) {
      const auto Text = llvm::toString(std::move(E));
      fail(ExecutionRunReason::EnvironmentFailure, Text);
      return failure(Text);
    }
    if (auto E = Environment->beginInvocation(*Session, Invocation->frame())) {
      const auto Text = llvm::toString(std::move(E));
      fail(ExecutionRunReason::EnvironmentFailure, Text);
      return failure(Text);
    }
    if (auto Failed = budgetFailure(true))
      return failure(Failed->Diagnostic);
    publish(ExecutionRunState::Ready, ExecutionRunReason::InvocationStarted);
    return llvm::Error::success();
  } catch (...) {
    const std::string Text =
        "guest invocation preparation raised a host exception";
    fail(ExecutionRunReason::EnvironmentFailure, Text);
    return failure(Text);
  }
}

llvm::Expected<ExecutionRunEvent>
ExecutionRun::finishInvocation(const SessionExit &Exit) {
  if (auto Failed = budgetFailure()) {
    Last.CPU = Exit;
    return Last;
  }
  if (!Budget->hasEvents()) {
    fail(ExecutionRunReason::EventLimit,
         "guest invocation completion event limit exhausted");
    Last.CPU = Exit;
    return Last;
  }
  auto Result = Invocation->result(*Session);
  if (!Result)
    return Result.takeError();
  if (auto E = Environment->finishInvocation(*Session, *Result)) {
    fail(ExecutionRunReason::EnvironmentFailure, llvm::toString(std::move(E)));
    Last.CPU = Exit;
    return Last;
  }
  if (auto Failed = budgetFailure()) {
    Last.CPU = Exit;
    return Last;
  }
  if (!Budget->hasEvents()) {
    fail(ExecutionRunReason::EventLimit,
         "guest invocation completion event limit exhausted");
    Last.CPU = Exit;
    return Last;
  }
  if (auto E = Invocation->restoreCaller(*Session)) {
    fail(ExecutionRunReason::EnvironmentFailure, llvm::toString(std::move(E)));
    Last.CPU = Exit;
    return Last;
  }
  if (auto Failed = budgetFailure()) {
    Last.CPU = Exit;
    return Last;
  }
  if (!Budget->consumeEvents())
    return failure("guest invocation completion event limit exhausted");
  const auto P = Invocation->progress(*Budget);
  Result->Instructions = P.Instructions;
  Result->Events = P.Events;
  Result->Microseconds = P.Microseconds;
  Stage = InvocationStage;
  Invocation.reset();
  publish(ExecutionRunState::Paused, ExecutionRunReason::InvocationComplete);
  Last.CPU = Exit;
  Last.Invocation = std::move(*Result);
  return Last;
}
} // namespace neverd::emulation
