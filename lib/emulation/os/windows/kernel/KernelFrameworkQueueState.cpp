//===- KernelFrameworkQueueState.cpp - KMDF queue transitions ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "KernelFramework.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
using namespace framework;
llvm::Error invalidQueue(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "KMDF queue: " + Message);
}
} // namespace

llvm::Expected<uint64_t>
KernelFramework::callQueueStop(llvm::StringRef Name, Binding &B,
                               llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = queueForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &Q = **Selected;
  if (Q.DrainComplete)
    return invalidQueue("queue has a pending drain-completion callback");
  if (Name == api::WdfIoQueueStop && A[2] && Q.StopComplete)
    return invalidQueue("queue already has a stop-completion callback");
  Q.Accepting = true;
  Q.Dispatching = false;
  if (Name == api::WdfIoQueueStop && A[2]) {
    Q.StopComplete = A[2];
    Q.StopContext = A[3];
    auto Result = start({});
    if (!Result)
      return Result.takeError();
  }
  return 0;
}

llvm::Expected<uint64_t>
KernelFramework::callQueueStart(llvm::StringRef, Binding &B,
                                llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = queueForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &Q = **Selected;
  if (Q.DrainComplete)
    return invalidQueue("queue has a pending drain-completion callback");
  Q.Accepting = true;
  Q.Dispatching = true;
  if (Q.Dispatch == QueueDispatchManual) {
    Q.ReadyPending = Q.ReadyNotify && !queuePnpHeld(Q) && !Q.Pending.empty();
    if (auto E = flushReadyNotifications())
      return E;
    return 0;
  }
  if (Q.Pending.empty())
    return 0;
  std::vector<Step> Steps(Q.Pending.size(), {StepKind::PresentQueue, A[1]});
  auto Result = start(std::move(Steps));
  if (!Result)
    return Result.takeError();
  return *Result;
}

llvm::Expected<uint64_t>
KernelFramework::callQueueDrain(llvm::StringRef Name, Binding &B,
                                llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = queueForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &Q = **Selected;
  if (Q.DrainComplete || Q.StopComplete)
    return invalidQueue("queue already has a state-completion callback");
  Q.Accepting = false;
  if (Name == api::WdfIoQueueDrain && A[2]) {
    Q.DrainComplete = A[2];
    Q.DrainContext = A[3];
    auto Result = start({});
    if (!Result)
      return Result.takeError();
  }
  return 0;
}

llvm::Expected<uint64_t>
KernelFramework::callQueuePurge(llvm::StringRef Name, Binding &B,
                                llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = queueForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &Q = **Selected;
  const bool StopAndPurge = Name == api::WdfIoQueueStopAndPurge ||
                            Name == api::WdfIoQueueStopAndPurgeSynchronously;
  const bool Asynchronous =
      Name == api::WdfIoQueuePurge || Name == api::WdfIoQueueStopAndPurge;
  if (Q.DrainComplete || Q.StopComplete)
    return invalidQueue("queue already has a state-completion callback");
  if (PendingCall)
    return invalidQueue("purge cannot replace a pending guest callback");
  if (!RequestsHost.RecordCancel || !RequestsHost.ValidateCompletion ||
      !RequestsHost.SetInformation || !RequestsHost.Information ||
      !RequestsHost.Complete)
    return invalidQueue("purge cancellation host is unavailable");
  std::vector<Step> Steps;
  std::vector<uint64_t> Cancellable;
  for (uint64_t Handle : Q.Pending) {
    auto R = Requests.find(Handle);
    if (R == Requests.end() || !R->second.Queued || R->second.Queue != A[1] ||
        R->second.Completed || R->second.Completing)
      return invalidQueue("purge lost a queued request");
    if (!(Q.CanceledOnQueue && (R->second.DeliveredOnce || R->second.Enqueued)))
      if (auto E = RequestsHost.ValidateCompletion(R->second.IRP,
                                                   RequestCancelled, 0))
        return E;
  }
  for (const auto &[Handle, R] : Requests)
    if (R.Queue == A[1] && !R.Queued && !R.Completed && !R.Completing &&
        R.Cancellation == CancelState::Marked) {
      auto O = Objects.find(Handle);
      if (O == Objects.end() || !O->second.InternalReferences ||
          !R.CancelRoutine)
        return invalidQueue("purge lost a cancelable driver request");
      Cancellable.push_back(Handle);
    }
  for (uint64_t Handle : Q.Pending) {
    auto &R = Requests.at(Handle);
    if (auto E = RequestsHost.RecordCancel(R.IRP))
      return E;
    if (Q.CanceledOnQueue && (R.DeliveredOnce || R.Enqueued)) {
      R.Queued = false;
      R.CanceledOnQueue = true;
      R.QueuedCallback = 0;
      R.QueuedArguments.clear();
      R.QueuedCompletionStatus.reset();
      Steps.push_back({StepKind::CanceledOnQueue, Handle});
      continue;
    }
    if (auto E = RequestsHost.SetInformation(R.IRP, 0))
      return E;
    R.Completing = true;
    R.CompletionStatus = RequestCancelled;
    std::vector<Step> Delete;
    if (auto E = planDelete(Handle, Delete))
      return E;
    auto Destruction =
        std::find_if(Delete.begin(), Delete.end(), [&](const Step &S) {
          return S.Kind == StepKind::TryDestroy && S.Object == Handle;
        });
    Delete.insert(Destruction, {StepKind::CompleteRequest, Handle});
    Steps.insert(Steps.end(), Delete.begin(), Delete.end());
    R.Queued = false;
    R.QueuedCallback = 0;
    R.QueuedArguments.clear();
    R.QueuedCompletionStatus.reset();
  }
  Q.Pending.clear();
  Q.ReadyPending = false;
  for (uint64_t Handle : Cancellable)
    Steps.push_back({StepKind::PurgeCancelRequest, Handle});
  Q.Accepting = StopAndPurge;
  if (StopAndPurge)
    Q.Dispatching = false;
  if (Asynchronous && A[2]) {
    if (StopAndPurge) {
      Q.StopComplete = A[2];
      Q.StopContext = A[3];
    } else {
      Q.DrainComplete = A[2];
      Q.DrainContext = A[3];
    }
  }
  if (!Steps.empty() || (Asynchronous && A[2])) {
    auto Result = start(std::move(Steps));
    if (!Result)
      return Result.takeError();
  }
  return 0;
}

llvm::Expected<uint64_t>
KernelFramework::callQueueGetState(llvm::StringRef, Binding &B,
                                   llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = queueForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &Q = **Selected;
  if (A[2])
    if (auto E = writable(A[2], 4))
      return E;
  if (A[3])
    if (auto E = writable(A[3], 4))
      return E;
  const uint32_t Queued = Q.Pending.size();
  const uint32_t Delivered =
      std::count_if(Requests.begin(), Requests.end(), [&](const auto &Entry) {
        return Entry.second.Queue == A[1] && !Entry.second.Queued &&
               !Entry.second.Completed;
      });
  if (A[2])
    if (auto E = Memory.writeInteger(A[2], Queued, 4))
      return E;
  if (A[3])
    if (auto E = Memory.writeInteger(A[3], Delivered, 4))
      return E;
  uint32_t State = Q.Accepting ? QueueAcceptRequests : 0;
  if (Q.Dispatching)
    State |= QueueDispatchRequests;
  if (queuePnpHeld(Q))
    State |= QueuePnpHeld;
  if (!Queued)
    State |= QueueNoRequests;
  if (!Delivered)
    State |= QueueDriverNoRequests;
  return State;
}

} // namespace neverd::emulation
