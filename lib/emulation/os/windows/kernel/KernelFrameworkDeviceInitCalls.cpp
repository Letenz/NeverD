//===- KernelFrameworkDeviceInitCalls.cpp - KMDF initializers ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "KernelFramework.h"

namespace neverd::emulation {
namespace {
using namespace framework;
llvm::Error controlError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "KMDF control device: " + Message);
}
} // namespace

llvm::Expected<KernelFramework::DeviceInit *>
KernelFramework::deviceInitForCall(Binding &B, uint64_t Handle) {
  auto I = DeviceInits.find(Handle);
  if (I == DeviceInits.end() || I->second.Binding != B.Globals)
    return controlError("invalid, consumed or foreign device initializer");
  return &I->second;
}

llvm::Expected<uint64_t>
KernelFramework::callDeviceInitFree(llvm::StringRef, Binding &B,
                                    llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = deviceInitForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &Init = **Selected;
  if (Init.Kind != DeviceInitKind::Control)
    return controlError("framework-owned PnP initializer is freed after "
                        "EvtDriverDeviceAdd returns");
  if (auto E = retire(A[1]))
    return E;
  DeviceInits.erase(A[1]);
  return 0;
}

llvm::Expected<uint64_t>
KernelFramework::callDeviceInitAssignName(llvm::StringRef, Binding &B,
                                          llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = deviceInitForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &Init = **Selected;
  if (!A[2]) {
    Init.Name.clear();
  } else {
    auto Text = readControlString(A[2]);
    if (!Text)
      return Text.takeError();
    Init.Name = std::move(*Text);
  }
  return 0;
}

llvm::Expected<uint64_t> KernelFramework::callDeviceInitSetDeviceType(
    llvm::StringRef, Binding &B, llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = deviceInitForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &Init = **Selected;
  if (Init.Kind != DeviceInitKind::Pnp)
    return controlError("device type requires an FDO initializer");
  Init.DeviceType = static_cast<uint32_t>(A[2]);
  return 0;
}

llvm::Expected<uint64_t> KernelFramework::callDeviceInitSetExclusive(
    llvm::StringRef, Binding &B, llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = deviceInitForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &Init = **Selected;
  Init.Exclusive = static_cast<uint8_t>(A[2]) != 0;
  return 0;
}

llvm::Expected<uint64_t> KernelFramework::callDeviceInitSetCallerContext(
    llvm::StringRef, Binding &B, llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = deviceInitForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &Init = **Selected;
  if (!A[2])
    return controlError("caller-context callback must name guest code");
  Init.CallerContext = A[2];
  return 0;
}

llvm::Expected<uint64_t> KernelFramework::callDeviceInitSetPowerPolicyOwner(
    llvm::StringRef, Binding &B, llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = deviceInitForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &Init = **Selected;
  if (Init.Kind != DeviceInitKind::Pnp)
    return controlError("power policy ownership requires an FDO initializer");
  Init.PowerPolicyOwner = static_cast<uint8_t>(A[2]) != 0;
  return 0;
}

llvm::Expected<uint64_t> KernelFramework::callDeviceInitSetPnpCallbacks(
    llvm::StringRef, Binding &B, llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = deviceInitForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &Init = **Selected;
  if (Init.Kind != DeviceInitKind::Pnp)
    return controlError("PnP power callbacks require an FDO initializer");
  if (auto E = ValidateAccess(A[2], PnpPowerCallbacksSize, false))
    return E;
  auto Size = read(A[2], 4);
  if (!Size)
    return Size.takeError();
  if (*Size != PnpPowerCallbacksSize)
    return controlError("unsupported PnP power callback structure size");
  PnpCallbacks Callbacks;
  for (unsigned Index = 0; Index < PnpPowerCallbacksCount; ++Index) {
    auto Callback =
        read(A[2] + PnpPowerCallbacksFirstOffset + Index * sizeof(uint64_t));
    if (!Callback)
      return Callback.takeError();
    switch (Index) {
#define NEVERD_FRAMEWORK_PNP_CALLBACK(Name, Slot, Result)                      \
  case Slot:                                                                   \
    Callbacks.Name = *Callback;                                                \
    break;
#include "KernelFrameworkPnpCallbacks.def"
#undef NEVERD_FRAMEWORK_PNP_CALLBACK
    default:
      if (*Callback)
        return controlError("unsupported PnP power event callback");
    }
  }
  Init.Callbacks = Callbacks;
  return 0;
}

llvm::Expected<uint64_t>
KernelFramework::callDeviceInitSetIoType(llvm::StringRef, Binding &B,
                                         llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = deviceInitForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  if (A[2] != ControlIoNeither && A[2] != ControlIoBuffered &&
      A[2] != ControlIoDirect)
    return controlError("unsupported READ/WRITE I/O type");
  (**Selected).IoType = uint32_t(A[2]);
  return 0;
}

llvm::Expected<uint64_t> KernelFramework::callFdoInitGetPhysicalDevice(
    llvm::StringRef, Binding &B, llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto I = DeviceInits.find(A[1]);
  if (I == DeviceInits.end() || I->second.Binding != B.Globals ||
      I->second.Kind != DeviceInitKind::Pnp)
    return controlError("physical device requires a live FDO initializer");
  return I->second.PDO;
}

llvm::Expected<uint64_t>
KernelFramework::callFdoInitSetFilter(llvm::StringRef, Binding &B,
                                      llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto I = DeviceInits.find(A[1]);
  if (I == DeviceInits.end() || I->second.Binding != B.Globals ||
      I->second.Kind != DeviceInitKind::Pnp)
    return controlError("filter registration requires a live FDO initializer");
  I->second.Filter = true;
  return 0;
}

} // namespace neverd::emulation
