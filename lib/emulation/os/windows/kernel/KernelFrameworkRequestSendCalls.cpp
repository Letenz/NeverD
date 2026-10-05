//===- KernelFrameworkRequestSendCalls.cpp - KMDF request calls -*- C++ -*-===//
//
// NeverD Decompiler
//
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

llvm::Expected<uint64_t>
KernelFramework::callRequestFormat(llvm::StringRef, Binding &B,
                                   llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = requestForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &R = **Selected;
  if (!R.FileCreate || !R.File || R.Queue || R.LastSendStatus ||
      R.CompletionCallbackPending)
    return requestError(
        "current-type formatting requires an unsent framework-file CREATE");
  R.FormattedForSend = true;
  return 0;
}

llvm::Expected<uint64_t> KernelFramework::callRequestSetCompletionRoutine(
    llvm::StringRef, Binding &B, llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = requestForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &R = **Selected;
  if (!R.FileCreate || !R.File || R.Queue || R.LastSendStatus ||
      R.CompletionCallbackPending || (!A[2] && A[3]))
    return requestError(
        "completion routine requires an unsent framework-file CREATE");
  R.CompletionRoutine = A[2];
  R.CompletionContext = A[3];
  return 0;
}

llvm::Expected<uint64_t>
KernelFramework::callRequestSend(llvm::StringRef, Binding &B,
                                 llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = requestForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &R = **Selected;
  auto Target = Objects.find(A[2]);
  auto Device = Devices.find(R.Device);
  if (Target == Objects.end() || Target->second.Kind != ObjectKind::IoTarget ||
      Target->second.Binding != B.Globals || Target->second.Deleting ||
      Device == Devices.end() || Device->second.LocalTarget != A[2] ||
      Target->second.Parent != R.Device)
    return requestError("send requires the request device's local target");
  uint64_t Flags = 0;
  std::optional<int64_t> SendTimeout;
  if (A[3]) {
    if (auto E = ValidateAccess(A[3], RequestSendOptionsSize, false))
      return E;
    auto Size = read(A[3], sizeof(uint32_t));
    auto Options = read(A[3] + RequestSendFlagsOffset, sizeof(uint32_t));
    if (!Size || !Options)
      return llvm::joinErrors(Size.takeError(), Options.takeError());
    if (*Size != RequestSendOptionsSize)
      return requestError("unsupported request send options size");
    Flags = *Options;
    if (Flags & RequestSendTimeout) {
      auto RawTimeout = read(A[3] + RequestSendTimeoutOffset);
      if (!RawTimeout)
        return RawTimeout.takeError();
      SendTimeout = std::bit_cast<int64_t>(*RawTimeout);
    }
  }
  if (Flags != 0 && Flags != RequestSendTimeout &&
      Flags != RequestSendAndForget && Flags != RequestSendSynchronous &&
      Flags != (RequestSendTimeout | RequestSendSynchronous))
    return requestError(
        "only default asynchronous, synchronous or send-and-forget file "
        "forwarding with an optional timeout is modeled");
  if (!R.FileCreate || R.Queue || R.Cancellation != CancelState::Unmarked ||
      !Device->second.Files.forwards(Device->second.Filter) ||
      R.LastSendStatus || R.CompletionCallbackPending)
    return requestError("send requires an unsent forwardable CREATE request");
  const bool SendAndForget = Flags == RequestSendAndForget;
  const bool Synchronous = Flags & RequestSendSynchronous;
  const bool Asynchronous = !SendAndForget && !Synchronous;
  if ((SendAndForget && R.File) || (!SendAndForget && !R.File))
    return requestError("send with completion ownership requires a "
                        "framework file object; send-and-forget requires "
                        "none");
  if (Asynchronous && (!R.FormattedForSend || !R.CompletionRoutine))
    return requestError(
        "asynchronous CREATE send requires formatting and a completion "
        "routine");
  if (Synchronous && R.CompletionRoutine)
    return requestError(
        "synchronous CREATE send with a completion routine is unsupported");
  if (!RequestsHost.ValidateFileForward ||
      (SendAndForget ? !RequestsHost.ForwardFile
       : Synchronous ? !RequestsHost.SendFileSynchronously
                     : !RequestsHost.SendFileAsynchronously))
    return requestError("lower file-request host is unavailable");
  if (auto E = RequestsHost.ValidateFileForward(R.IRP))
    return E;
  uint64_t CompletionParams = 0;
  if (Asynchronous) {
    if (PendingCall || NextContinuation == UINT64_MAX)
      return requestError("completion callback capacity exhausted");
    auto Storage = allocate(RequestCompletionParamsSize, true, false);
    if (!Storage)
      return Storage.takeError();
    CompletionParams = *Storage;
    R.CompletionTarget = A[2];
    R.PendingCompletionParams = CompletionParams;
    R.CompletionCallbackPending = true;
  }
  auto Status = SendAndForget ? RequestsHost.ForwardFile(R.IRP)
                : Synchronous
                    ? RequestsHost.SendFileSynchronously(R.IRP, SendTimeout)
                    : RequestsHost.SendFileAsynchronously(R.IRP, SendTimeout);
  if (!Status) {
    auto E = Status.takeError();
    if (CompletionParams) {
      R.CompletionCallbackPending = false;
      R.CompletionTarget = 0;
      R.PendingCompletionParams = 0;
      E = llvm::joinErrors(std::move(E), retire(CompletionParams));
    }
    return E;
  }
  if (!SendAndForget) {
    if (Asynchronous) {
      if (*Status != windows::StatusPending) {
        auto Call = queueFileSendCompletion(R.IRP, *Status, 1);
        if (!Call)
          return Call.takeError();
        PendingCall = std::move(*Call);
      }
    } else if (*Status == windows::StatusPending)
      R.SynchronousSendPending = true;
    else
      R.LastSendStatus = *Status;
    return 1;
  }
  R.Completed = true;
  R.CompletionStatus = *Status;
  std::vector<Step> Steps;
  if (auto E = planDelete(A[1], Steps))
    return E;
  auto Retired = start(std::move(Steps));
  if (!Retired)
    return Retired.takeError();
  return 1;
}

llvm::Expected<uint64_t>
KernelFramework::callRequestGetStatus(llvm::StringRef, Binding &B,
                                      llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = requestForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &R = **Selected;
  if (!R.LastSendStatus)
    return requestError("request has no completed lower send");
  return *R.LastSendStatus;
}

llvm::Expected<uint64_t> KernelFramework::callRequestGetCompletionParams(
    llvm::StringRef, Binding &B, llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = requestForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &R = **Selected;
  if (!R.LastSendStatus)
    return requestError("request has no completed lower send");
  if (auto E = writable(A[2], RequestCompletionParamsSize))
    return E;
  auto Size = read(A[2], sizeof(uint32_t));
  if (!Size)
    return Size.takeError();
  if (*Size != RequestCompletionParamsSize)
    return requestError("unsupported completion parameter size");
  if (auto E = writeRequestCompletionParams(A[2], *R.LastSendStatus))
    return E;
  return 0;
}

} // namespace neverd::emulation
