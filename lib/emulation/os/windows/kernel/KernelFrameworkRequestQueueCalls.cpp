//===- KernelFrameworkRequestQueueCalls.cpp - KMDF request calls -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "KernelFramework.h"
#include "WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
using namespace framework;
llvm::Error requestError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "KMDF request: " + Message);
}
} // namespace

llvm::Expected<uint64_t> KernelFramework::callRequestStopAcknowledge(
    llvm::StringRef, Binding &B, llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = requestForCall(B, A[1], RequestOwner::Any);
  if (!Selected)
    return Selected.takeError();
  auto &R = **Selected;
  if (A[2] > 1)
    return requestError("stop acknowledgment requires a Boolean requeue flag");
  const auto Active = std::find_if(
      PnpTransitions.begin(), PnpTransitions.end(), [&](const auto &Entry) {
        return Entry.second.Current.Phase == PnpPhase::IoStop &&
               Entry.second.Current.Request == A[1] &&
               !Entry.second.WaitingForRequests;
      });
  if (Active == PnpTransitions.end() || R.StopAcknowledged ||
      R.InCallerContext || R.Queued)
    return requestError(
        "stop acknowledgment requires the active EvtIoStop request");
  auto Q = Queues.find(R.Queue);
  if (Q == Queues.end() || !Q->second.PowerManaged ||
      Q->first != Active->second.Current.Queue)
    return requestError("stop acknowledgment lost its power-managed queue");
  if (A[2]) {
    if (R.Cancellation != CancelState::Unmarked)
      return requestError("requeue requires an unmarked request");
    std::optional<RequestDispatch> Dispatch;
    if (Q->second.Dispatch != QueueDispatchManual) {
      if (!RequestsHost.View)
        return requestError("request inspection host is unavailable");
      auto View = RequestsHost.View(R.IRP);
      if (!View)
        return View.takeError();
      auto Planned = queueDispatch(Q->first, A[1], *View);
      if (!Planned)
        return Planned.takeError();
      Dispatch = std::move(*Planned);
    }
    R.Queued = true;
    if (Dispatch) {
      R.QueuedCallback = Dispatch->PC;
      R.QueuedArguments = std::move(Dispatch->Arguments);
      if (!Dispatch->PC)
        R.QueuedCompletionStatus = Dispatch->Status;
    }
    const bool WasEmpty = Q->second.Pending.empty();
    Q->second.Pending.push_back(A[1]);
    if (WasEmpty && Q->second.Dispatch == QueueDispatchManual &&
        Q->second.ReadyNotify)
      Q->second.ReadyPending = true;
  } else {
    if (!Q->second.IoResume)
      return requestError("retained stop acknowledgment requires EvtIoResume");
    R.PowerSuspended = true;
  }
  R.StopAcknowledged = true;
  return 0;
}

llvm::Expected<uint64_t>
KernelFramework::callRequestForward(llvm::StringRef Name, Binding &B,
                                    llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = requestForCall(B, A[1], RequestOwner::Any);
  if (!Selected)
    return Selected.takeError();
  auto &R = **Selected;
  auto &O = Objects.at(A[1]);
  const bool Requeue = Name == api::WdfRequestRequeue;
  const uint64_t Target = Requeue ? R.Queue : A[2];
  auto DestinationObject = Objects.find(Target);
  auto Destination = Queues.find(Target);
  if (DestinationObject == Objects.end() ||
      DestinationObject->second.Kind != ObjectKind::Queue ||
      DestinationObject->second.Binding != B.Globals ||
      Destination == Queues.end() || DestinationObject->second.Deleting)
    return requestError("invalid, foreign or deleting destination queue");
  if (!Destination->second.Accepting)
    return QueueBusy;
  if (R.InCallerContext || R.Queued || R.CanceledOnQueue || !R.Queue ||
      (Requeue ? Destination->second.Dispatch != QueueDispatchManual
               : R.Queue == Target) ||
      R.Device != Destination->second.Device ||
      R.Cancellation != CancelState::Unmarked)
    return ControlInvalidDeviceRequest;
  std::optional<RequestDispatch> ForwardDispatch;
  if (!Requeue && Destination->second.Dispatch != QueueDispatchManual) {
    if (!RequestsHost.View)
      return requestError("request inspection host is unavailable");
    auto View = RequestsHost.View(R.IRP);
    if (!View)
      return View.takeError();
    auto Planned = queueDispatch(Target, A[1], *View);
    if (!Planned)
      return Planned.takeError();
    if (!Planned->PC &&
        (!RequestsHost.ValidateCompletion || !RequestsHost.SetInformation ||
         !RequestsHost.Information || !RequestsHost.Complete))
      return requestError("automatic queue completion host is unavailable");
    ForwardDispatch = std::move(*Planned);
  }
  if (!Requeue) {
    if (PendingCall)
      return requestError("forwarding cannot replace a pending callback");
    if (auto E = preflightCancellationToken(0))
      return E;
  }
  auto Source = Objects.find(R.Queue);
  if (Source == Objects.end() || Source->second.Kind != ObjectKind::Queue ||
      Source->second.Deleting || O.Parent != R.Queue)
    return requestError("request lost its source queue ownership");
  auto Child = std::find(Source->second.Children.begin(),
                         Source->second.Children.end(), A[1]);
  if (Child == Source->second.Children.end())
    return requestError("source queue lost its request child");
  const uint64_t SourceHandle = R.Queue;
  if (!RequestsHost.IsCanceled)
    return requestError("cancellation host is unavailable");
  auto AlreadyCanceled = RequestsHost.IsCanceled(R.IRP);
  if (!AlreadyCanceled)
    return AlreadyCanceled.takeError();
  const auto &Device = Devices.at(Destination->second.Device);
  if (Destination->second.PowerManaged && Device.PDO)
    if (auto E = powerPolicyActive(Device.PDO))
      return E;
  if (!Requeue) {
    DestinationObject->second.Children.push_back(A[1]);
    Source->second.Children.erase(Child);
    O.Parent = Target;
    R.Queue = Target;
  }
  R.Queued = true;
  if (ForwardDispatch) {
    R.QueuedCallback = ForwardDispatch->PC;
    R.QueuedArguments = std::move(ForwardDispatch->Arguments);
    if (!ForwardDispatch->PC)
      R.QueuedCompletionStatus = ForwardDispatch->Status;
  }
  const bool WasEmpty = Destination->second.Pending.empty();
  if (Requeue)
    Destination->second.Pending.push_front(A[1]);
  else
    Destination->second.Pending.push_back(A[1]);
  if (WasEmpty && Destination->second.Dispatch == QueueDispatchManual &&
      Destination->second.ReadyNotify)
    Destination->second.ReadyPending = true;
  if (*AlreadyCanceled) {
    // A request canceled before forwarding is subject to framework queue
    // cancellation as soon as the new queue takes ownership.
    auto Call = requestCancellation(R.IRP);
    if (!Call)
      return Call.takeError();
    if (*Call) {
      if (!Requeue)
        Continuations.at((**Call).Token)
            .Steps.push_back({StepKind::PresentQueue, SourceHandle});
      PendingCall = std::move(**Call);
    } else if (!Requeue) {
      const uint64_t Token = NextContinuation++;
      Continuations.emplace(
          Token, Continuation{{{StepKind::PresentQueue, SourceHandle}}});
      auto Next = advance(Token);
      if (!Next)
        return Next.takeError();
    }
  } else if (!Requeue) {
    const uint64_t Token = NextContinuation++;
    Continuations.emplace(
        Token, Continuation{{{StepKind::PresentQueue, SourceHandle}}});
    auto Delivered = presentQueued(Target, Token);
    if (!Delivered)
      return Delivered.takeError();
    if (!*Delivered) {
      auto Next = advance(Token);
      if (!Next)
        return Next.takeError();
    }
  }
  if (auto E = flushReadyNotifications())
    return E;
  return 0;
}

} // namespace neverd::emulation
