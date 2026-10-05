//===- KernelFrameworkRequestCancellationCalls.cpp - KMDF request calls -*- C++
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

llvm::Expected<uint64_t> KernelFramework::callRequestMarkCancelable(
    llvm::StringRef Name, Binding &B, llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = requestForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &R = **Selected;
  auto &O = Objects.at(A[1]);
  const bool Legacy = Name == api::WdfRequestMarkCancelable;
  if (!A[2])
    return requestError("marking cancelable requires a cancel callback");
  if (R.Cancellation != CancelState::Unmarked) {
    if (Legacy)
      return requestError("legacy marking requires an unmarked request");
    return ControlInvalidDeviceRequest;
  }
  if (!RequestsHost.IsCanceled)
    return requestError("cancellation host is unavailable");
  auto Canceled = RequestsHost.IsCanceled(R.IRP);
  if (!Canceled)
    return Canceled.takeError();
  // Unlike the legacy void MarkCancelable API, Ex never delivers a cancel
  // callback for an IRP that was already canceled when registration began.
  if (*Canceled && !Legacy)
    return RequestCancelled;
  if (*Canceled && PendingCall)
    return requestError("cancellation cannot replace a pending guest callback");
  if (*Canceled)
    if (auto E = preflightCancellationToken(0))
      return E;
  if (O.InternalReferences == UINT64_MAX)
    return requestError("internal reference count overflow");
  ++O.InternalReferences;
  R.CancelRoutine = A[2];
  R.Cancellation = CancelState::Marked;
  if (*Canceled) {
    // RequestCancelable(..., FALSE) takes the same cancellation reference
    // even when insertion finds an already canceled IRP. DispatchEvents can
    // invoke the driver before this API returns when the callback lock and
    // execution level permit it; otherwise the callback remains queued.
    // https://github.com/microsoft/Windows-Driver-Frameworks/blob/b6191d9543441329154da32f7ab9bdd97228dd3c/src/framework/shared/irphandlers/io/fxioqueue.cpp#L2195-L2223
    auto Call = requestCancellation(R.IRP);
    if (!Call)
      return Call.takeError();
    if (!*Call)
      return requestError("legacy cancellation lost its guest callback");
    PendingCall = std::move(**Call);
  }
  return 0;
}

llvm::Expected<uint64_t> KernelFramework::callRequestUnmarkCancelable(
    llvm::StringRef, Binding &B, llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = requestForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &R = **Selected;
  auto &O = Objects.at(A[1]);
  if (R.Cancellation == CancelState::Unmarked)
    return ControlInvalidDeviceRequest;
  if (R.Cancellation != CancelState::Marked)
    return RequestCancelled;
  if (!O.InternalReferences)
    return requestError("cancelable request lost its callback reference");
  --O.InternalReferences;
  R.CancelRoutine = 0;
  R.Cancellation = CancelState::Unmarked;
  return 0;
}

llvm::Expected<uint64_t>
KernelFramework::callRequestIsCanceled(llvm::StringRef, Binding &B,
                                       llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = requestForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &R = **Selected;
  // The public verifier requires an owned, noncancelable request here.
  // https://learn.microsoft.com/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestiscanceled
  if (R.Cancellation != CancelState::Unmarked)
    return requestError("IsCanceled requires an unmarked request");
  if (!RequestsHost.IsCanceled)
    return requestError("cancellation host is unavailable");
  auto Canceled = RequestsHost.IsCanceled(R.IRP);
  if (!Canceled)
    return Canceled.takeError();
  return *Canceled ? 1 : 0;
}

} // namespace neverd::emulation
