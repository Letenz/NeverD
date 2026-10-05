//===- KernelFrameworkDeviceCalls.cpp - KMDF device calls ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "KernelFramework.h"
#include "KernelResources.h"

namespace neverd::emulation {
namespace {
using namespace framework;
llvm::Error controlError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "KMDF control device: " + Message);
}
} // namespace

llvm::Expected<KernelFramework::Device *>
KernelFramework::deviceForCall(Binding &B, uint64_t Handle) {
  auto O = Objects.find(Handle);
  auto D = Devices.find(Handle);
  if (O == Objects.end() || O->second.Kind != ObjectKind::Device ||
      O->second.Binding != B.Globals || D == Devices.end())
    return controlError("operation requires a live framework device");
  return &D->second;
}

llvm::Expected<uint64_t>
KernelFramework::callResourceListQuery(llvm::StringRef Name, Binding &B,
                                       llvm::ArrayRef<uint64_t> A, uint8_t) {
  for (const auto &[Handle, Device] : Devices) {
    const auto &Object = Objects.at(Handle);
    if (Object.Binding != B.Globals || Object.Deleting ||
        !Device.ResourcesActive)
      continue;
    for (const ResourceList *List :
         {&Device.RawResources, &Device.TranslatedResources})
      if (A[1] == List->Handle) {
        if (Name == api::WdfCmResourceListGetCount)
          return List->Count;
        return A[2] < List->Count ? List->Descriptors +
                                        A[2] * resources::ResourceDescriptorSize
                                  : 0;
      }
  }
  return controlError("resource-list query requires a live assigned list");
}

llvm::Expected<uint64_t>
KernelFramework::callWdmDeviceHandle(llvm::StringRef, Binding &B,
                                     llvm::ArrayRef<uint64_t> A, uint8_t) {
  for (const auto &[Handle, Device] : Devices) {
    auto Object = Objects.find(Handle);
    if (Device.Wdm == A[1] && Object != Objects.end() &&
        Object->second.Binding == B.Globals && !Object->second.Deleting)
      return Handle;
  }
  return 0;
}

llvm::Expected<uint64_t>
KernelFramework::callDeviceEnqueueRequest(llvm::StringRef, Binding &B,
                                          llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto D = Devices.find(A[1]);
  auto O = Objects.find(A[1]);
  auto R = Requests.find(A[2]);
  if (D == Devices.end() || O == Objects.end() ||
      O->second.Kind != ObjectKind::Device || O->second.Binding != B.Globals ||
      R == Requests.end() || R->second.Completed || R->second.Completing)
    return controlError("enqueue requires a live device and request");
  auto Caller = CallerRequests.find(R->second.IRP);
  if (Caller == CallerRequests.end() || Caller->second != A[2] ||
      !R->second.InCallerContext || R->second.Enqueued ||
      R->second.Device != A[1])
    return controlError(
        "request must be enqueued once from its caller-context callback");
  if (!RequestsHost.View)
    return controlError("request inspection host is unavailable");
  auto View = RequestsHost.View(R->second.IRP);
  if (!View)
    return View.takeError();
  const uint64_t QueueHandle = D->second.dispatchQueue(View->Major);
  auto Q = Queues.find(QueueHandle);
  if (Q == Queues.end() || Q->second.Device != A[1] ||
      Objects.at(QueueHandle).Deleting)
    return controlError("caller-context request has no live dispatch queue");
  if (!Q->second.Accepting)
    return QueueBusy;
  R->second.Enqueued = true;
  R->second.Queue = QueueHandle;
  return 0;
}

llvm::Expected<uint64_t>
KernelFramework::callDeviceGetWdmObject(llvm::StringRef, Binding &B,
                                        llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = deviceForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  return (**Selected).Wdm;
}

llvm::Expected<uint64_t> KernelFramework::callDeviceGetPhysicalDevice(
    llvm::StringRef, Binding &B, llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = deviceForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  return (**Selected).PDO;
}

llvm::Expected<uint64_t>
KernelFramework::callDeviceGetDriver(llvm::StringRef, Binding &B,
                                     llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = deviceForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  return B.DriverHandle;
}

llvm::Expected<uint64_t>
KernelFramework::callDeviceGetIoTarget(llvm::StringRef, Binding &B,
                                       llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = deviceForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &D = **Selected;
  if (Objects.at(A[1]).Deleting)
    return controlError("cannot use a deleting device's local target");
  if (!D.PDO)
    return 0;
  if (!D.LocalTarget) {
    Attributes Attrs;
    Attrs.Parent = A[1];
    auto Target = createObject(B.Globals, Attrs, false);
    if (!Target)
      return Target.takeError();
    Objects.at(*Target).Kind = ObjectKind::IoTarget;
    D.LocalTarget = *Target;
  }
  return D.LocalTarget;
}

llvm::Expected<uint64_t> KernelFramework::callControlFinishInitializing(
    llvm::StringRef, Binding &B, llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = deviceForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &D = **Selected;
  if (Objects.at(A[1]).Deleting)
    return controlError("cannot change a deleting control device");
  if (D.PDO)
    return controlError("PnP devices finish initialization after AddDevice");
  if (!DevicesHost.FinishInitializing)
    return controlError("underlying WDM initialization host is unavailable");
  if (auto E = DevicesHost.FinishInitializing(D.Wdm))
    return E;
  D.Initialized = true;
  return 0;
}

llvm::Expected<uint64_t> KernelFramework::callDeviceCreateSymbolicLink(
    llvm::StringRef, Binding &B, llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = deviceForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &D = **Selected;
  if (Objects.at(A[1]).Deleting)
    return controlError("cannot change a deleting control device");
  auto Link = readControlString(A[2]);
  if (!Link)
    return Link.takeError();
  if (Link->empty() || D.HasLink)
    return ControlInvalidDeviceRequest;
  if (!DevicesHost.Link)
    return controlError("underlying WDM link host is unavailable");
  auto Status = DevicesHost.Link(D.Wdm, *Link);
  if (!Status)
    return Status.takeError();
  if (!*Status)
    D.HasLink = true;
  return *Status;
}

} // namespace neverd::emulation
