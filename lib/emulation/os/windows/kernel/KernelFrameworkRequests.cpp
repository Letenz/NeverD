//===- KernelFrameworkRequests.cpp - KMDF queue dispatch and completion
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Framework request handles borrow one authoritative WDM IRP. Queue callbacks
/// return void; the framework owns pending dispatch status and completion owns
/// packet/buffer retirement independently from callback return.
///
//===----------------------------------------------------------------------===//

#include "KernelFramework.h"
#include "WindowsKernelLayout.h"

#include <algorithm>
#include <bit>

namespace neverd::emulation {
namespace {
using namespace framework;
llvm::Error requestError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "KMDF request: " + Message);
}
} // namespace

llvm::Error
KernelFramework::preflightCancellationToken(uint64_t EarlierCallbacks) const {
  if (!NextContinuation || EarlierCallbacks >= UINT64_MAX - NextContinuation)
    return requestError("cancellation continuation token capacity exhausted");
  const uint64_t Token = NextContinuation + EarlierCallbacks;
  if (Continuations.contains(Token) || CancelCallbacks.contains(Token))
    return requestError("cancellation continuation token is already owned");
  return llvm::Error::success();
}

llvm::Expected<bool>
KernelFramework::preflightRequestCancellation(uint64_t IRP,
                                              uint64_t EarlierCallbacks) const {
  const auto R =
      std::find_if(Requests.begin(), Requests.end(),
                   [&](const auto &Entry) { return Entry.second.IRP == IRP; });
  if (R != Requests.end() && R->second.Queued) {
    auto Q = Queues.find(R->second.Queue);
    if (R->second.Completed || R->second.Completing ||
        R->second.Cancellation != CancelState::Unmarked || Q == Queues.end() ||
        (Q->second.Dispatch != QueueDispatchManual &&
         Q->second.Dispatch != QueueDispatchParallel &&
         Q->second.Dispatch != QueueDispatchSequential) ||
        std::find(Q->second.Pending.begin(), Q->second.Pending.end(),
                  R->first) == Q->second.Pending.end())
      return requestError("queued request lost framework queue ownership");
    if (PendingCall)
      return requestError(
          "cancellation cannot replace a pending guest callback");
    const bool Notify = Q->second.CanceledOnQueue &&
                        (R->second.DeliveredOnce || R->second.Enqueued);
    if (!RequestsHost.IsCanceled ||
        (!Notify &&
         (!RequestsHost.ValidateCompletion || !RequestsHost.SetInformation ||
          !RequestsHost.Information || !RequestsHost.Complete)))
      return requestError("queued request cancellation host is unavailable");
    if (auto E = preflightCancellationToken(EarlierCallbacks))
      return E;
    // Even callback-free cancellation consumes a framework continuation.
    // Reserve one token and one scheduler slot conservatively at this boundary.
    return true;
  }
  if (R == Requests.end() || R->second.Completed || R->second.Completing ||
      R->second.Cancellation != CancelState::Marked)
    return false;
  if (PendingCall)
    return requestError("cancellation cannot replace a pending guest callback");
  if (!RequestsHost.IsCanceled)
    return requestError("cancellation host is unavailable");
  const auto O = Objects.find(R->first);
  if (O == Objects.end() || !O->second.InternalReferences ||
      !R->second.CancelRoutine)
    return requestError("cancelable request lost its callback reference");
  if (auto E = preflightCancellationToken(EarlierCallbacks))
    return E;
  return true;
}

llvm::Expected<std::optional<KernelFramework::GuestCall>>
KernelFramework::requestCancellation(uint64_t IRP) {
  auto GeneratesCallback = preflightRequestCancellation(IRP);
  if (!GeneratesCallback)
    return GeneratesCallback.takeError();
  if (!*GeneratesCallback)
    return std::optional<GuestCall>{};
  auto Canceled = RequestsHost.IsCanceled(IRP);
  if (!Canceled)
    return Canceled.takeError();
  if (!*Canceled)
    return requestError("cancellation requires the underlying IRP cancel flag");
  auto R =
      std::find_if(Requests.begin(), Requests.end(),
                   [&](const auto &Entry) { return Entry.second.IRP == IRP; });
  if (R->second.Queued) {
    auto Q = Queues.find(R->second.Queue);
    if (Q == Queues.end())
      return requestError("queued cancellation lost its framework queue");
    auto Pending =
        std::find(Q->second.Pending.begin(), Q->second.Pending.end(), R->first);
    if (Pending == Q->second.Pending.end())
      return requestError("queued cancellation lost its pending request");
    if (Q->second.CanceledOnQueue &&
        (R->second.DeliveredOnce || R->second.Enqueued)) {
      Q->second.Pending.erase(Pending);
      if (Q->second.Pending.empty())
        Q->second.ReadyPending = false;
      R->second.Queued = false;
      R->second.CanceledOnQueue = true;
      R->second.QueuedCallback = 0;
      R->second.QueuedArguments.clear();
      R->second.QueuedCompletionStatus.reset();
      auto Canceled = start({{StepKind::CanceledOnQueue, R->first}});
      if (!Canceled)
        return Canceled.takeError();
      return takeGuestCall();
    }
    if (auto E = RequestsHost.ValidateCompletion(IRP, RequestCancelled, 0))
      return E;
    if (auto E = RequestsHost.SetInformation(IRP, 0))
      return E;
    R->second.Completing = true;
    R->second.CompletionStatus = RequestCancelled;
    std::vector<Step> Steps;
    if (auto E = planDelete(R->first, Steps)) {
      R->second.Completing = false;
      return E;
    }
    auto Destruction =
        std::find_if(Steps.begin(), Steps.end(), [&](const Step &S) {
          return S.Kind == StepKind::TryDestroy && S.Object == R->first;
        });
    Steps.insert(Destruction, {StepKind::CompleteRequest, R->first});
    Q->second.Pending.erase(Pending);
    if (Q->second.Pending.empty())
      Q->second.ReadyPending = false;
    R->second.Queued = false;
    auto Completed = start(std::move(Steps));
    if (!Completed)
      return Completed.takeError();
    return takeGuestCall();
  }
  const uint64_t Token = NextContinuation++;
  // FxRequest::InsertTailIrpQueue holds FXREQUEST_QUEUE_TAG. Cancellation
  // transfers that hold to ProcessCancelledRequests, which releases it only
  // after InvokeCancel returns. Driver references are a separate authority.
  // https://github.com/microsoft/Windows-Driver-Frameworks/blob/b6191d9543441329154da32f7ab9bdd97228dd3c/src/framework/shared/irphandlers/io/fxioqueue.cpp#L4892-L4935
  Continuations.emplace(
      Token,
      Continuation{{{StepKind::Callback, R->first, R->second.CancelRoutine, 0,
                     0, callbackSynchronizationObject(R->first)},
                    {StepKind::CancelReturned, R->first}}});
  CancelCallbacks.emplace(Token, R->first);
  R->second.Cancellation = CancelState::Queued;
  R->second.CancelRoutine = 0;
  auto Result = advance(Token);
  if (!Result)
    return Result.takeError();
  return takeGuestCall();
}

llvm::Error KernelFramework::beginCancelCallback(uint64_t Token) {
  auto C = CancelCallbacks.find(Token);
  if (C == CancelCallbacks.end())
    return requestError("unknown cancellation callback token");
  auto R = Requests.find(C->second);
  if (R == Requests.end() || R->second.Completed || R->second.Completing ||
      R->second.Cancellation != CancelState::Queued)
    return requestError("cancellation callback was already delivered");
  // Queueing a callback does not authorize completion. The public framework
  // sets FXREQUEST_FLAG_CANCELLED immediately before invoking the driver.
  R->second.Cancellation = CancelState::Delivered;
  return llvm::Error::success();
}

llvm::Error KernelFramework::retainQueueCallback(uint64_t Token,
                                                 uint64_t Queue) {
  if (!Queues.contains(Queue) || QueueCallbacks.contains(Token))
    return requestError("callback lost its queue or already holds a reference");
  if (auto E = retainSynchronizationObject(Queue))
    return E;
  QueueCallbacks.emplace(Token, Queue);
  return llvm::Error::success();
}

llvm::Error KernelFramework::finishRequestDispatch(uint64_t IRP) {
  auto Callback = RequestDispatchQueues.find(IRP);
  if (Callback == RequestDispatchQueues.end())
    return llvm::Error::success();
  if (auto E = releaseSynchronizationObject(Callback->second))
    return E;
  RequestDispatchQueues.erase(Callback);
  return flushSynchronizationDestructions();
}

bool KernelFramework::isPowerManagedCallback(uint64_t IRP,
                                             uint64_t Token) const {
  const auto &Callbacks = Token ? QueueCallbacks : RequestDispatchQueues;
  auto Callback = Callbacks.find(Token ? Token : IRP);
  if (Callback == Callbacks.end())
    return false;
  auto Queue = Queues.find(Callback->second);
  return Queue != Queues.end() && Queue->second.PowerManaged;
}

llvm::Expected<KernelFramework::RequestDispatch>
KernelFramework::queueDispatch(uint64_t QueueHandle, uint64_t RequestHandle,
                               const RequestView &View) const {
  auto Q = Queues.find(QueueHandle);
  if (Q == Queues.end())
    return requestError("request queue has no framework identity");
  const auto &Queue = Q->second;
  uint64_t Callback = 0;
  uint64_t Length = 0;
  bool Specific = true;
  if (View.Major == RequestMajorRead) {
    Callback = Queue.Read;
    Length = View.OutputLength;
  } else if (View.Major == RequestMajorWrite) {
    Callback = Queue.Write;
    Length = View.InputLength;
  } else if (View.Major == RequestMajorDeviceControl) {
    Callback = Queue.DeviceControl;
  } else {
    return requestError("unsupported framework request major function");
  }
  if (!Callback) {
    Specific = false;
    Callback = Queue.Default;
  }
  if (!Callback)
    return RequestDispatch{0, {}, ControlInvalidDeviceRequest};
  if (!Queue.AllowZeroLength && !Length &&
      (View.Major == RequestMajorRead || View.Major == RequestMajorWrite))
    return RequestDispatch{0, {}, 0};
  RequestDispatch Dispatch;
  Dispatch.PC = Callback;
  Dispatch.Status = windows::StatusPending;
  Dispatch.SynchronizationObject = callbackSynchronizationObject(QueueHandle);
  Dispatch.Arguments = {QueueHandle, RequestHandle};
  if (Specific) {
    if (View.Major == RequestMajorDeviceControl) {
      Dispatch.Arguments.push_back(View.OutputLength);
      Dispatch.Arguments.push_back(View.InputLength);
      Dispatch.Arguments.push_back(View.ControlCode);
    } else {
      Dispatch.Arguments.push_back(Length);
    }
  }
  return Dispatch;
}

llvm::Expected<bool> KernelFramework::presentQueued(uint64_t QueueHandle,
                                                    uint64_t Token) {
  auto Q = Queues.find(QueueHandle);
  if (Q == Queues.end() || Q->second.Dispatch == QueueDispatchManual ||
      !Q->second.Dispatching || queuePnpHeld(Q->second) ||
      Q->second.Pending.empty())
    return false;
  if (PendingCall || !Continuations.contains(Token))
    return requestError("queued delivery lost its callback continuation");
  const uint32_t Limit = Q->second.Dispatch == QueueDispatchSequential
                             ? 1
                             : Q->second.PresentedLimit;
  const auto Presented =
      std::count_if(Requests.begin(), Requests.end(), [&](const auto &Entry) {
        return Entry.second.Queue == QueueHandle && !Entry.second.Queued &&
               !Entry.second.Completed;
      });
  // A sequential queue may also have driver-owned requests explicitly
  // retrieved from its pending list. They do not create another automatic
  // presentation slot until they complete or leave the queue.
  if (Presented >= Limit)
    return false;
  const uint64_t Handle = Q->second.Pending.front();
  auto R = Requests.find(Handle);
  if (R == Requests.end() || !R->second.Queued ||
      R->second.Queue != QueueHandle ||
      (!R->second.QueuedCallback && !R->second.QueuedCompletionStatus) ||
      (R->second.QueuedCallback && R->second.QueuedArguments.size() < 2))
    return requestError("automatic queue lost its pending delivery");
  if (R->second.QueuedCompletionStatus) {
    const uint32_t Status = *R->second.QueuedCompletionStatus;
    if (!RequestsHost.ValidateCompletion || !RequestsHost.SetInformation ||
        !RequestsHost.Information || !RequestsHost.Complete)
      return requestError("automatic queue completion host is unavailable");
    if (auto E = RequestsHost.ValidateCompletion(R->second.IRP, Status, 0))
      return E;
    if (auto E = RequestsHost.SetInformation(R->second.IRP, 0))
      return E;
    R->second.Completing = true;
    R->second.CompletionStatus = Status;
    std::vector<Step> Steps;
    if (auto E = planDelete(Handle, Steps)) {
      R->second.Completing = false;
      return E;
    }
    auto Destruction =
        std::find_if(Steps.begin(), Steps.end(), [&](const Step &S) {
          return S.Kind == StepKind::TryDestroy && S.Object == Handle;
        });
    Steps.insert(Destruction, {StepKind::CompleteRequest, Handle});
    Q->second.Pending.pop_front();
    R->second.Queued = false;
    R->second.QueuedCompletionStatus.reset();
    auto &Continuation = Continuations.at(Token);
    Continuation.Steps.insert(Continuation.Steps.begin() + Continuation.Index,
                              Steps.begin(), Steps.end());
    return false;
  }
  if (auto E = retainQueueCallback(Token, QueueHandle))
    return E;
  Q->second.Pending.pop_front();
  R->second.Queued = false;
  R->second.DeliveredOnce = true;
  PendingCall = GuestCall{Token,
                          R->second.QueuedCallback,
                          std::move(R->second.QueuedArguments),
                          {},
                          callbackSynchronizationObject(QueueHandle)};
  R->second.QueuedCallback = 0;
  return true;
}

llvm::Expected<std::optional<KernelFramework::RequestDispatch>>
KernelFramework::routeRequest(uint64_t WdmDevice, uint64_t IRP,
                              bool AfterCaller) {
  auto D = std::find_if(Devices.begin(), Devices.end(), [&](const auto &Entry) {
    return Entry.second.Wdm == WdmDevice;
  });
  if (D == Devices.end())
    return std::optional<RequestDispatch>{};
  if (!D->second.Initialized || Objects.at(D->first).Deleting)
    return requestError("I/O requires a fully initialized live control device");
  if (!RequestsHost.View || !RequestsHost.Complete || !RequestsHost.MarkPending)
    return requestError("underlying WDM request host is unavailable");
  auto View = RequestsHost.View(IRP);
  if (!View)
    return View.takeError();
  auto FileRoute = routeFileRequest(D->first, IRP, *View);
  if (!FileRoute)
    return FileRoute.takeError();
  if (*FileRoute)
    return *FileRoute;
  auto File = requestFileObject(D->first, View->File);
  if (!File)
    return File.takeError();
  auto CompleteImmediately =
      [&](uint32_t Status,
          uint32_t Dispatch) -> llvm::Expected<std::optional<RequestDispatch>> {
    if (auto E = RequestsHost.Complete(IRP, Status, 0))
      return E;
    return std::optional<RequestDispatch>{RequestDispatch{0, {}, Dispatch}};
  };
  uint64_t ExistingHandle = 0;
  uint64_t QueueHandle = D->second.dispatchQueue(View->Major);
  if (AfterCaller) {
    auto Caller = CallerRequests.find(IRP);
    if (Caller == CallerRequests.end())
      return requestError("caller-context continuation lost its request");
    ExistingHandle = Caller->second;
    auto R = Requests.find(ExistingHandle);
    if (R == Requests.end() || !R->second.InCallerContext ||
        !R->second.Enqueued || R->second.Device != D->first)
      return requestError("caller-context request was not enqueued");
    QueueHandle = R->second.Queue;
  }
  auto Q = Queues.find(QueueHandle);
  if (Q == Queues.end() && D->second.Filter)
    return requestError(
        "automatic non-file filter forwarding is outside this profile");
  if (Q == Queues.end())
    return CompleteImmediately(ControlInvalidDeviceRequest,
                               ControlInvalidDeviceRequest);
  if (Objects.at(Q->first).Deleting)
    return requestError("request dispatch queue is deleting");
  if (!Q->second.Accepting) {
    if (AfterCaller)
      return requestError(
          "caller-context queue drained during request routing");
    return CompleteImmediately(QueueInvalidDeviceState,
                               QueueInvalidDeviceState);
  }
  if (AfterCaller) {
    auto O = Objects.find(ExistingHandle);
    if (O == Objects.end() || O->second.Kind != ObjectKind::Request)
      return requestError("caller-context request lost its object");
    auto Source = Objects.find(O->second.Parent);
    if (Source == Objects.end() || Source->second.Kind != ObjectKind::Queue ||
        Source->second.Deleting)
      return requestError("caller-context request lost its source queue");
    auto Child = llvm::find(Source->second.Children, ExistingHandle);
    if (Child == Source->second.Children.end())
      return requestError("source queue lost its caller-context request");
    if (O->second.Parent != Q->first) {
      Objects.at(Q->first).Children.push_back(ExistingHandle);
      Source->second.Children.erase(Child);
      O->second.Parent = Q->first;
    }
    Requests.at(ExistingHandle).InCallerContext = false;
    CallerRequests.erase(IRP);
  } else if (D->second.CallerContext) {
    if (auto E = RequestsHost.MarkPending(IRP))
      return E;
    Attributes Attrs;
    Attrs.Parent = Q->first;
    const uint64_t Globals = Objects.at(D->first).Binding;
    auto Handle = createObject(Globals, Attrs, false);
    if (!Handle)
      return Handle.takeError();
    Objects.at(*Handle).Kind = ObjectKind::Request;
    Requests.emplace(*Handle, Request{IRP, 0, D->first, true});
    Requests.at(*Handle).File = *File;
    CallerRequests.emplace(IRP, *Handle);
    return std::optional<RequestDispatch>{
        RequestDispatch{D->second.CallerContext,
                        {D->first, *Handle},
                        windows::StatusPending,
                        true}};
  }
  const bool Manual = Q->second.Dispatch == QueueDispatchManual;
  if (Manual && View->Major != RequestMajorRead &&
      View->Major != RequestMajorWrite &&
      View->Major != RequestMajorDeviceControl)
    return requestError("unsupported framework request major function");
  auto Planned = Manual ? llvm::Expected<RequestDispatch>(RequestDispatch{})
                        : queueDispatch(Q->first, 0, *View);
  if (!Planned)
    return Planned.takeError();
  if (AfterCaller && !Manual && !Planned->PC)
    return requestError("caller-context queue completion without a guest I/O "
                        "callback is outside this profile");
  auto &Queue = Q->second;
  bool WaitForSlot = !Queue.Dispatching || queuePnpHeld(Queue);
  if (Queue.Dispatch == QueueDispatchSequential ||
      (Queue.Dispatch == QueueDispatchParallel &&
       Queue.PresentedLimit != UINT32_MAX)) {
    const uint32_t Limit =
        Queue.Dispatch == QueueDispatchSequential ? 1 : Queue.PresentedLimit;
    const auto Presented =
        std::count_if(Requests.begin(), Requests.end(), [&](const auto &Entry) {
          return Entry.first != ExistingHandle &&
                 Entry.second.Queue == Q->first && !Entry.second.Queued &&
                 !Entry.second.Completed;
        });
    WaitForSlot |= Presented >= Limit || !Queue.Pending.empty();
  }
  // FxIoQueue::QueueRequest marks accepted IRPs pending before dispatching.
  // That status persists even when delivery completes the IRP immediately.
  if (auto E = RequestsHost.MarkPending(IRP))
    return E;
  if (!Manual && !Planned->PC && !WaitForSlot)
    return CompleteImmediately(Planned->Status, windows::StatusPending);
  uint64_t Handle = ExistingHandle;
  if (!Handle) {
    Attributes Attrs;
    Attrs.Parent = Q->first;
    const uint64_t Globals = Objects.at(D->first).Binding;
    auto Created = createObject(Globals, Attrs, false);
    if (!Created)
      return Created.takeError();
    Handle = *Created;
    Objects.at(Handle).Kind = ObjectKind::Request;
    Requests.emplace(Handle, Request{IRP, Q->first, D->first});
    Requests.at(Handle).File = *File;
  }
  if (Queue.PowerManaged && D->second.PDO)
    if (auto E = powerPolicyActive(D->second.PDO))
      return E;
  WaitForSlot |= queuePnpHeld(Queue);
  if (Manual || WaitForSlot) {
    auto &Pending = Requests.at(Handle);
    Pending.Queued = true;
    if (!Manual) {
      if (Planned->PC) {
        Planned->Arguments[1] = Handle;
        Pending.QueuedCallback = Planned->PC;
        Pending.QueuedArguments = std::move(Planned->Arguments);
      } else {
        Pending.QueuedCompletionStatus = Planned->Status;
      }
    }
    const bool WasEmpty = Queue.Pending.empty();
    Queue.Pending.push_back(Handle);
    if (Manual && WasEmpty && Queue.ReadyNotify)
      Queue.ReadyPending = true;
    return std::optional<RequestDispatch>{
        RequestDispatch{0, {}, windows::StatusPending}};
  }
  RequestDispatch Dispatch = std::move(*Planned);
  Dispatch.Arguments[1] = Handle;
  if (RequestDispatchQueues.contains(IRP))
    return requestError("request dispatch already holds its queue reference");
  if (auto E = retainSynchronizationObject(Q->first))
    return E;
  RequestDispatchQueues.emplace(IRP, Q->first);
  Requests.at(Handle).DeliveredOnce = true;
  return std::optional<RequestDispatch>{std::move(Dispatch)};
}

llvm::Expected<KernelFramework::RequestDispatch>
KernelFramework::continueCallerContext(uint64_t IRP) {
  auto Caller = CallerRequests.find(IRP);
  if (Caller == CallerRequests.end())
    return requestError("no caller-context callback owns this request");
  auto R = Requests.find(Caller->second);
  if (R == Requests.end() || R->second.Completed) {
    CallerRequests.erase(Caller);
    return RequestDispatch{0, {}, windows::StatusPending};
  }
  if (!R->second.Enqueued)
    return requestError(
        "caller-context callback returned without enqueue or completion");
  auto D = Devices.find(R->second.Device);
  if (D == Devices.end())
    return requestError("caller-context request lost its device");
  auto Routed = routeRequest(D->second.Wdm, IRP, true);
  if (!Routed)
    return Routed.takeError();
  if (!*Routed)
    return requestError("caller-context continuation lost framework routing");
  return std::move(**Routed);
}

llvm::Error KernelFramework::writeRequestParameters(uint64_t Address,
                                                    const RequestView &View) {
  auto Size = read(Address, 2);
  if (!Size)
    return Size.takeError();
  if (*Size != RequestParametersSize)
    return requestError("unsupported WDF_REQUEST_PARAMETERS size");
  if (auto E = writable(Address, RequestParametersSize))
    return E;
  if (auto E =
          Memory.write(Address, std::vector<uint8_t>(RequestParametersSize)))
    return E;
  if (auto E = Memory.writeInteger(Address, RequestParametersSize, 2))
    return E;
  if (auto E =
          Memory.writeInteger(Address + RequestParametersType, View.Major, 4))
    return E;
  if (View.Major == RequestMajorDeviceControl) {
    if (auto E = Memory.writeInteger(Address + RequestParametersLength,
                                     View.OutputLength, 8))
      return E;
    if (auto E = Memory.writeInteger(Address + RequestParametersInputLength,
                                     View.InputLength, 8))
      return E;
    if (auto E = Memory.writeInteger(Address + RequestParametersControlCode,
                                     View.ControlCode, 4))
      return E;
  } else {
    const auto Length =
        View.Major == RequestMajorRead ? View.OutputLength : View.InputLength;
    if (auto E =
            Memory.writeInteger(Address + RequestParametersLength, Length, 8))
      return E;
    if (auto E = Memory.writeInteger(Address + RequestParametersOffset,
                                     View.ByteOffset, 8))
      return E;
  }
  return llvm::Error::success();
}

llvm::Error KernelFramework::writeRequestCompletionParams(uint64_t Address,
                                                          uint32_t Status) {
  if (auto E = writable(Address, RequestCompletionParamsSize))
    return E;
  if (auto E = Memory.write(Address,
                            std::vector<uint8_t>(RequestCompletionParamsSize)))
    return E;
  if (auto E = Memory.writeInteger(Address, RequestCompletionParamsSize, 4))
    return E;
  if (auto E = Memory.writeInteger(Address + RequestCompletionTypeOffset,
                                   RequestCompletionTypeNoFormat, 4))
    return E;
  if (auto E = Memory.writeInteger(Address + RequestCompletionStatusOffset,
                                   Status, 4))
    return E;
  return Memory.writeInteger(Address + RequestCompletionInformationOffset, 0,
                             8);
}

llvm::Expected<KernelFramework::GuestCall>
KernelFramework::previewFileSendCompletion(uint64_t IRP, uint32_t Status,
                                           uint64_t EarlierCallbacks) const {
  if (Status == windows::StatusPending)
    return requestError("lower file completion requires a final status");
  if (EarlierCallbacks >= UINT64_MAX - NextContinuation)
    return requestError("completion callback capacity exhausted");
  auto Request = llvm::find_if(Requests, [IRP](const auto &Entry) {
    return Entry.second.IRP == IRP && Entry.second.FileCreate;
  });
  if (Request == Requests.end() || Request->second.Completed ||
      !Request->second.CompletionCallbackPending ||
      Request->second.CompletionCallbackEntered ||
      Request->second.LastSendStatus || !Request->second.CompletionRoutine ||
      !Request->second.CompletionTarget ||
      !Request->second.PendingCompletionParams)
    return requestError("lower file completion lost its sent request");
  return GuestCall{0,
                   Request->second.CompletionRoutine,
                   {Request->first, Request->second.CompletionTarget,
                    Request->second.PendingCompletionParams,
                    Request->second.CompletionContext}};
}

llvm::Expected<KernelFramework::GuestCall>
KernelFramework::queueFileSendCompletion(uint64_t IRP, uint32_t Status,
                                         uint64_t ReturnValue) {
  auto Call = previewFileSendCompletion(IRP, Status);
  if (!Call)
    return Call.takeError();
  if (auto E = writeRequestCompletionParams(Call->Arguments[2], Status))
    return E;
  const uint64_t Token = NextContinuation++;
  Continuation C;
  C.ReturnValue = ReturnValue;
  Continuations.emplace(Token, std::move(C));
  RequestCompletionCallbacks.emplace(
      Token, RequestCompletionCallback{Call->Arguments[0], Call->Arguments[2]});
  auto &Request = Requests.at(Call->Arguments[0]);
  Request.LastSendStatus = Status;
  Request.PendingCompletionParams = 0;
  Call->Token = Token;
  return Call;
}

llvm::Error KernelFramework::beginRequestCompletionCallback(uint64_t Token) {
  auto Callback = RequestCompletionCallbacks.find(Token);
  if (Callback == RequestCompletionCallbacks.end())
    return requestError("completion callback has no retained request");
  auto Request = Requests.find(Callback->second.Request);
  if (Request == Requests.end() || !Request->second.CompletionCallbackPending ||
      Request->second.CompletionCallbackEntered ||
      !Request->second.LastSendStatus)
    return requestError("completion callback lost its lower result");
  Request->second.CompletionCallbackEntered = true;
  return llvm::Error::success();
}

std::optional<bool>
KernelFramework::synchronousFileSendPending(uint64_t Handle) const {
  auto Request = Requests.find(Handle);
  if (Request == Requests.end() || Request->second.Completed)
    return std::nullopt;
  return Request->second.SynchronousSendPending;
}

llvm::Error
KernelFramework::validateSynchronousFileCompletion(uint64_t IRP,
                                                   uint32_t Status) const {
  auto Request = llvm::find_if(
      Requests, [IRP](const auto &Entry) { return Entry.second.IRP == IRP; });
  if (Status == windows::StatusPending || Request == Requests.end() ||
      Request->second.Completed || !Request->second.SynchronousSendPending ||
      Request->second.LastSendStatus)
    return requestError("lower completion lost its synchronous file send");
  return llvm::Error::success();
}

llvm::Error KernelFramework::completeSynchronousFileSend(uint64_t IRP,
                                                         uint32_t Status) {
  if (auto E = validateSynchronousFileCompletion(IRP, Status))
    return E;
  auto Request = llvm::find_if(
      Requests, [IRP](const auto &Entry) { return Entry.second.IRP == IRP; });
  Request->second.LastSendStatus = Status;
  Request->second.SynchronousSendPending = false;
  return llvm::Error::success();
}

} // namespace neverd::emulation
