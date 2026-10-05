//===- KernelFrameworkCalls.cpp - KMDF object call handlers -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "KernelFramework.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
using namespace framework;
llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "KMDF: " + Message);
}
} // namespace

llvm::Expected<KernelFramework::Object *>
KernelFramework::objectForCall(Binding &B, uint64_t Handle) {
  auto I = Objects.find(Handle);
  if (I == Objects.end() || I->second.Binding != B.Globals)
    return invalid("invalid, foreign or deleted framework handle");
  return &I->second;
}

llvm::Expected<uint64_t>
KernelFramework::callCallbackLock(llvm::StringRef Name, Binding &B,
                                  llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Object = Objects.find(A[1]);
  if (Object == Objects.end() || Object->second.Binding != B.Globals)
    return invalid("callback lock requires a matching framework handle");
  auto Owner = synchronizationObject(A[1], false);
  if (!Owner)
    return Owner.takeError();
  const auto &Operation = Name == api::WdfObjectAcquireLock
                              ? DevicesHost.AcquireCallbackLock
                              : DevicesHost.ReleaseCallbackLock;
  if (!Operation)
    return invalid("callback lock host is unavailable");
  if (auto E = Operation(*Owner))
    return E;
  if (Name == api::WdfObjectReleaseLock)
    if (auto E = flushSynchronizationDestructions())
      return E;
  return 0;
}

llvm::Expected<uint64_t>
KernelFramework::callWdmDriverHandle(llvm::StringRef, Binding &B,
                                     llvm::ArrayRef<uint64_t> A, uint8_t) {
  if (A[1] != Driver || !Objects.count(B.DriverHandle))
    return invalid("WDM driver does not own a live framework driver");
  return B.DriverHandle;
}

llvm::Expected<uint64_t>
KernelFramework::callObjectCreate(llvm::StringRef, Binding &B,
                                  llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Validation = attributes(A[1], AttributesUse::Object);
  if (!Validation)
    return Validation.takeError();
  if (const auto *Status = std::get_if<uint32_t>(&*Validation))
    return *Status;
  const auto &Attrs = std::get<Attributes>(*Validation);
  if (Attrs.Synchronization != SynchronizationInherit &&
      Attrs.Synchronization != SynchronizationNone)
    return invalid("synchronization scope requires a driver, device or queue");
  if (auto E = writable(A[2], 8))
    return E;
  auto Handle = createObject(B.Globals, Attrs, false);
  if (!Handle)
    return Handle.takeError();
  if (auto E = Memory.writeInteger(A[2], *Handle, 8))
    return E;
  return 0;
}

llvm::Expected<uint64_t>
KernelFramework::callObjectFromContext(llvm::StringRef, Binding &B,
                                       llvm::ArrayRef<uint64_t> A, uint8_t) {
  for (const auto &[Handle, O] : Objects)
    if (O.Binding == B.Globals)
      for (const auto &[Type, Context] : O.Contexts)
        if (Context.Address && Context.Address == A[1])
          return Handle;
  return invalid("context pointer has no live framework owner");
}

llvm::Expected<uint64_t>
KernelFramework::callDriverAccessor(llvm::StringRef Name, Binding &B,
                                    llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Object = objectForCall(B, A[1]);
  if (!Object)
    return Object.takeError();
  auto &O = **Object;
  if (O.Kind != ObjectKind::Driver)
    return invalid("framework handle has the wrong object type");
  return Name == api::WdfDriverGetRegistryPath ? B.RegistryCopy : Driver;
}

llvm::Expected<uint64_t>
KernelFramework::callTypedContext(llvm::StringRef, Binding &B,
                                  llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Object = objectForCall(B, A[1]);
  if (!Object)
    return Object.takeError();
  auto &O = **Object;
  auto TypeSize = read(A[2], 4);
  if (!TypeSize)
    return TypeSize.takeError();
  if (*TypeSize != ContextTypeSize && *TypeSize != ContextTypeLegacySize)
    return invalid("unsupported typed context record size");
  auto CI = O.Contexts.find(A[2]);
  return CI == O.Contexts.end() ? 0 : CI->second.Address;
}

llvm::Expected<uint64_t>
KernelFramework::callAllocateContext(llvm::StringRef, Binding &B,
                                     llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Object = objectForCall(B, A[1]);
  if (!Object)
    return Object.takeError();
  auto &O = **Object;
  if (O.Deleting)
    return DeletePending;
  auto Validation = attributes(A[2], AttributesUse::AdditionalContext);
  if (!Validation)
    return Validation.takeError();
  if (const auto *Status = std::get_if<uint32_t>(&*Validation))
    return *Status;
  const auto &Attrs = std::get<Attributes>(*Validation);
  if (!Attrs.Type)
    return ObjectNameInvalid;
  if (A[3])
    if (auto E = writable(A[3], 8))
      return E;
  const bool Exists = O.Contexts.count(Attrs.Type);
  auto Context = addContext(O, Attrs);
  if (!Context)
    return Context.takeError();
  if (A[3])
    if (auto E = Memory.writeInteger(A[3], *Context, 8))
      return E;
  return Exists ? ObjectNameExists : 0;
}

llvm::Expected<uint64_t>
KernelFramework::callReference(llvm::StringRef, Binding &B,
                               llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Object = objectForCall(B, A[1]);
  if (!Object)
    return Object.takeError();
  auto &O = **Object;
  if (O.Cleaned)
    return invalid(
        "references after cleanup are outside this framework profile");
  if (O.References == UINT64_MAX)
    return invalid("framework reference count overflow");
  ++O.References;
  return 0;
}

llvm::Expected<uint64_t>
KernelFramework::callDereference(llvm::StringRef, Binding &B,
                                 llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Object = objectForCall(B, A[1]);
  if (!Object)
    return Object.takeError();
  auto &O = **Object;
  if (!O.References)
    return invalid("framework reference count underflow");
  --O.References;
  if (O.Cleaned && O.DestroyEligible && !O.References && !O.InternalReferences)
    return start({{StepKind::TryDestroy, A[1]}});
  return 0;
}

llvm::Expected<uint64_t> KernelFramework::callDelete(llvm::StringRef,
                                                     Binding &B,
                                                     llvm::ArrayRef<uint64_t> A,
                                                     uint8_t) {
  auto Object = objectForCall(B, A[1]);
  if (!Object)
    return Object.takeError();
  auto &O = **Object;
  if (O.Kind == ObjectKind::Driver)
    return invalid("WDFDRIVER cannot be deleted by the driver");
  if (O.Kind == ObjectKind::Queue && Queues.at(A[1]).IsDefault)
    return invalid("the default queue cannot be deleted by the driver");
  if (O.Kind == ObjectKind::Queue) {
    const auto &Routes = Devices.at(Queues.at(A[1]).Device).DispatchQueues;
    if (std::any_of(Routes.begin(), Routes.end(),
                    [&](const auto &Route) { return Route.second == A[1]; }))
      return invalid("a dispatch queue cannot be deleted by the driver");
  }
  if (O.Kind == ObjectKind::Request)
    return invalid("an incoming framework request is released by completion");
  if (O.Kind == ObjectKind::IoTarget)
    return invalid("the local I/O target is owned by its device");
  if (O.Kind == ObjectKind::File)
    return invalid("a framework file object is released by CLOSE");
  if (O.Kind == ObjectKind::Device && Devices.at(A[1]).PDO)
    return invalid("a PnP framework device is deleted by removal");
  std::vector<Step> Steps;
  if (auto E = planDelete(A[1], Steps))
    return E;
  return start(std::move(Steps));
}

llvm::Expected<uint64_t>
KernelFramework::callDriverCreate(llvm::StringRef, Binding &B,
                                  llvm::ArrayRef<uint64_t> A, uint8_t) {
  return createDriver(B, A);
}

llvm::Expected<uint64_t> KernelFramework::callUnload(llvm::StringRef,
                                                     Binding &B,
                                                     llvm::ArrayRef<uint64_t> A,
                                                     uint8_t IRQL) {
  if (IRQL)
    return invalid("framework unload requires PASSIVE_LEVEL");
  if (A[0] != Driver || !B.DriverHandle || B.Unloaded)
    return invalid("invalid or repeated framework driver unload");
  std::vector<Step> Steps;
  if (B.UnloadCallback)
    Steps.push_back({StepKind::Callback, B.DriverHandle, B.UnloadCallback});
  Steps.push_back({StepKind::BeginDriverDelete, B.Globals});
  Steps.push_back({StepKind::DriverUnloaded, B.Globals});
  return start(std::move(Steps));
}

} // namespace neverd::emulation
