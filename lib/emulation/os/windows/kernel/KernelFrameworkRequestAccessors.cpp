//===- KernelFrameworkRequestAccessors.cpp - KMDF request access ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Request accessors use one authoritative WDM packet and distinguish the
/// lifetime of that packet from a referenced framework request handle.
///
//===----------------------------------------------------------------------===//

#include "KernelFramework.h"
#include "WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
using namespace framework;

llvm::Error accessorError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "KMDF request: " + Message);
}
} // namespace

bool KernelFramework::ownsRequestIRP(uint64_t IRP) const {
  return std::any_of(Requests.begin(), Requests.end(), [&](const auto &Entry) {
    return Entry.second.IRP == IRP;
  });
}

bool KernelFramework::isPowerParkedIRP(uint64_t IRP) const {
  const auto Request =
      std::find_if(Requests.begin(), Requests.end(),
                   [&](const auto &Entry) { return Entry.second.IRP == IRP; });
  if (Request == Requests.end() || Request->second.Completed)
    return false;
  auto Queue = Queues.find(Request->second.Queue);
  if (Queue == Queues.end() || !Queue->second.PowerManaged)
    return false;
  auto Device = Devices.find(Queue->second.Device);
  return Device != Devices.end() && Device->second.queuesHeld() &&
         (Request->second.Queued || Request->second.PowerSuspended);
}

llvm::Expected<KernelFramework::Request *>
KernelFramework::requestAccessorForCall(Binding &B, uint64_t Handle,
                                        RequestLifetime Lifetime) {
  auto O = Objects.find(Handle);
  auto R = Requests.find(Handle);
  if (O == Objects.end() || O->second.Kind != ObjectKind::Request ||
      O->second.Binding != B.Globals || R == Requests.end())
    return accessorError("invalid or foreign framework request");
  if (R->second.Queued)
    return accessorError("framework owns the request in a manual queue");
  if (R->second.SynchronousSendPending)
    return accessorError("lower target owns the pending synchronous request");
  if (Lifetime == RequestLifetime::Active &&
      (R->second.Completed || R->second.Completing))
    return accessorError("request is completed or completion is in progress");
  return &R->second;
}

// These documented neutral results require a surviving object handle, not
// a surviving IRP. Completion detaches the queue before request cleanup.
// https://learn.microsoft.com/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestgetinformation
// https://learn.microsoft.com/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestgetioqueue
llvm::Expected<uint64_t>
KernelFramework::callRequestGetIoQueue(llvm::StringRef, Binding &B,
                                       llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = requestAccessorForCall(B, A[1], RequestLifetime::Retained);
  if (!Selected)
    return Selected.takeError();
  auto &R = **Selected;
  const bool Completed = R.Completed || R.Completing;
  return Completed ? 0 : R.Queue;
}

llvm::Expected<uint64_t> KernelFramework::callRequestGetInformation(
    llvm::StringRef, Binding &B, llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = requestAccessorForCall(B, A[1], RequestLifetime::Retained);
  if (!Selected)
    return Selected.takeError();
  auto &R = **Selected;
  const bool Completed = R.Completed || R.Completing;
  if (Completed)
    return 0;
  if (!RequestsHost.Information)
    return accessorError("information host is unavailable");
  auto Information = RequestsHost.Information(R.IRP);
  if (!Information)
    return Information.takeError();
  return *Information;
}

llvm::Expected<uint64_t>
KernelFramework::callRequestRetrieveMdl(llvm::StringRef Name, Binding &B,
                                        llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = requestAccessorForCall(B, A[1], RequestLifetime::Retained);
  if (!Selected)
    return Selected.takeError();
  auto &R = **Selected;
  const bool Completed = R.Completed || R.Completing;
  const bool InputMdl = Name == api::WdfRequestRetrieveInputWdmMdl;
  const bool OutputMdl = Name == api::WdfRequestRetrieveOutputWdmMdl;
  if (auto E = writable(A[2], sizeof(uint64_t)))
    return E;
  if (auto E = Memory.writeInteger(A[2], 0, sizeof(uint64_t)))
    return E;
  // The public API supplies a status for a retained, completed request.
  // No host lookup may touch its already retired packet or descriptor.
  // https://learn.microsoft.com/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestretrieveinputwdmmdl
  if (Completed)
    return RequestInternalError;
  if (!RequestsHost.View || !RequestsHost.Mdl)
    return accessorError("MDL host is unavailable");
  auto View = RequestsHost.View(R.IRP);
  if (!View)
    return View.takeError();
  const bool IsRead = View->Major == RequestMajorRead;
  const bool IsWrite = View->Major == RequestMajorWrite;
  const bool IsIOCTL = View->Major == RequestMajorDeviceControl;
  if ((!IsRead && !IsWrite && !IsIOCTL) || (IsRead && InputMdl) ||
      (IsWrite && OutputMdl) || View->Neither ||
      (IsIOCTL && (View->ControlCode & windows::IoControlMethodMask) ==
                      windows::MethodNeither))
    return ControlInvalidDeviceRequest;
  const uint32_t Length = OutputMdl ? View->OutputLength : View->InputLength;
  if (!Length)
    return RequestBufferTooSmall;
  auto MDL = RequestsHost.Mdl(R.IRP, OutputMdl);
  if (!MDL)
    return MDL.takeError();
  if (!*MDL)
    return windows::StatusInsufficientResources;
  if (auto E = Memory.writeInteger(A[2], *MDL, sizeof(uint64_t)))
    return E;
  return 0;
}

llvm::Expected<uint64_t> KernelFramework::callRequestSetInformation(
    llvm::StringRef, Binding &B, llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = requestAccessorForCall(B, A[1], RequestLifetime::Active);
  if (!Selected)
    return Selected.takeError();
  auto &R = **Selected;
  if (!RequestsHost.SetInformation)
    return accessorError("information host is unavailable");
  if (auto E = RequestsHost.SetInformation(R.IRP, A[2]))
    return E;
  return 0;
}

llvm::Expected<uint64_t>
KernelFramework::callRequestGetFileObject(llvm::StringRef, Binding &B,
                                          llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = requestAccessorForCall(B, A[1], RequestLifetime::Active);
  if (!Selected)
    return Selected.takeError();
  auto &R = **Selected;
  return R.File;
}

llvm::Expected<uint64_t>
KernelFramework::callRequestGetIrp(llvm::StringRef, Binding &B,
                                   llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = requestAccessorForCall(B, A[1], RequestLifetime::Active);
  if (!Selected)
    return Selected.takeError();
  auto &R = **Selected;
  if (!RequestsHost.View)
    return accessorError("request inspection host is unavailable");
  auto View = RequestsHost.View(R.IRP);
  if (!View)
    return View.takeError();
  return View->IRP;
}

} // namespace neverd::emulation
