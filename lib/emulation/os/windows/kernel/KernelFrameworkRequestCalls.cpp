//===- KernelFrameworkRequestCalls.cpp - KMDF request calls -*- C++ -*-===//
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

llvm::Expected<KernelFramework::Request *>
KernelFramework::requestForCall(Binding &B, uint64_t Handle,
                                RequestOwner Owner) {
  auto O = Objects.find(Handle);
  auto R = Requests.find(Handle);
  if (O == Objects.end() || O->second.Kind != ObjectKind::Request ||
      O->second.Binding != B.Globals || R == Requests.end() ||
      R->second.Completed)
    return requestError("invalid, foreign or completed framework request");
  if (R->second.Completing)
    return requestError("request completion in progress");
  if (R->second.SynchronousSendPending)
    return requestError("lower target owns the pending synchronous request");
  if (Owner == RequestOwner::Driver && R->second.Queued)
    return requestError("framework owns the request in a manual queue");
  return &R->second;
}

llvm::Expected<KernelFramework::RequestView>
KernelFramework::requestViewForCall(const Request &R) {
  if (!RequestsHost.View)
    return requestError("request view host is unavailable");
  return RequestsHost.View(R.IRP);
}

llvm::Expected<uint64_t>
KernelFramework::callRequestComplete(llvm::StringRef Name, Binding &B,
                                     llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = requestForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &R = **Selected;
  if (R.CompletionCallbackPending && !R.CompletionCallbackEntered)
    return requestError("request completion precedes its lower callback");
  if (R.Cancellation == CancelState::Marked ||
      R.Cancellation == CancelState::Queued)
    return requestError(
        "completion requires successful UnmarkCancelable or delivered "
        "EvtRequestCancel");
  if (!RequestsHost.Complete || !RequestsHost.Information ||
      !RequestsHost.SetInformation || !RequestsHost.ValidateCompletion)
    return requestError("completion host is unavailable");
  uint64_t Information = 0;
  if (Name == api::WdfRequestCompleteWithInformation) {
    Information = A[3];
  } else {
    auto Value = RequestsHost.Information(R.IRP);
    if (!Value)
      return Value.takeError();
    Information = *Value;
  }
  if (auto E =
          RequestsHost.ValidateCompletion(R.IRP, uint32_t(A[2]), Information))
    return E;
  // CompleteWithInformation publishes to the IRP before EarlyDispose, so a
  // cleanup callback holding the raw packet observes the supplied value.
  // Keep that packet authoritative through cleanup and final completion.
  // https://github.com/microsoft/Windows-Driver-Frameworks/blob/b6191d9543441329154da32f7ab9bdd97228dd3c/src/framework/shared/inc/private/common/fxrequest.hpp#L810-L821
  if (Name == api::WdfRequestCompleteWithInformation)
    if (auto E = RequestsHost.SetInformation(R.IRP, Information))
      return E;
  // FxRequest::CompleteInternal performs EarlyDispose before giving up the
  // IRP. A cleanup callback may still use its buffers or release resources
  // embedded in them; explicit references only extend the object lifetime.
  R.Completing = true;
  R.CompletionStatus = uint32_t(A[2]);
  std::vector<Step> Steps;
  if (auto E = planDelete(A[1], Steps))
    return E;
  auto Destruction =
      std::find_if(Steps.begin(), Steps.end(), [&](const Step &S) {
        return S.Kind == StepKind::TryDestroy && S.Object == A[1];
      });
  Steps.insert(Destruction, {StepKind::CompleteRequest, A[1]});
  auto Value = start(std::move(Steps));
  if (!Value)
    return Value.takeError();
  return *Value;
}

} // namespace neverd::emulation
