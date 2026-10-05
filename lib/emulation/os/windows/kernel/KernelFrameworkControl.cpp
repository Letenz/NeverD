//===- KernelFrameworkControl.cpp - KMDF control-device initialization
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Own WDF initializers and handles while the host bridge owns the underlying
/// WDM device, namespace and memory. This profile requires a named control
/// device and an explicit universal-access DACL; it never guesses a caller's
/// Windows token or claims to enforce an unmodeled security descriptor.
///
//===----------------------------------------------------------------------===//

#include "KernelFramework.h"
#include "KernelResources.h"
#include "WindowsKernelLayout.h"

namespace neverd::emulation {
namespace {
using namespace framework;
#define NEVERD_KERNEL_NAME(Name, Value)                                        \
  constexpr llvm::StringLiteral Name = Value;
#include "KernelNames.def"
#undef NEVERD_KERNEL_NAME

llvm::Error controlError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "KMDF control device: " + Message);
}
} // namespace

llvm::Expected<std::string>
KernelFramework::readControlString(uint64_t Address) {
  constexpr auto CodeUnitBytes = sizeof(char16_t);
  auto Length = read(Address, CodeUnitBytes);
  auto Maximum = read(Address + windows::UnicodeMaximumOffset, CodeUnitBytes);
  auto Buffer = read(Address + windows::UnicodeBufferOffset);
  if (!Length || !Maximum || !Buffer)
    return llvm::joinErrors(
        Length.takeError(),
        llvm::joinErrors(Maximum.takeError(), Buffer.takeError()));
  if (*Length % CodeUnitBytes || *Length > *Maximum ||
      *Length > MaxRegistryPathBytes)
    return controlError("invalid counted Unicode string");
  if (!*Length)
    return std::string{};
  if (auto E = ValidateAccess(*Buffer, *Length, false))
    return E;
  std::vector<uint8_t> Bytes(*Length);
  if (auto E = Memory.read(*Buffer, Bytes))
    return E;
  std::string Text;
  for (size_t I = 0; I < Bytes.size(); I += CodeUnitBytes) {
    if (Bytes[I + 1] || Bytes[I] < ' ' || Bytes[I] > '~')
      return controlError(
          "only printable ASCII namespace and SDDL text is modeled");
    Text.push_back(char(Bytes[I]));
  }
  return Text;
}

llvm::Expected<uint64_t> KernelFramework::callControlDeviceInitAllocate(
    llvm::StringRef, Binding &B, llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Driver = Objects.find(A[1]);
  if (Driver == Objects.end() || Driver->second.Kind != ObjectKind::Driver ||
      Driver->second.Binding != B.Globals || Driver->second.Deleting)
    return controlError("initializer requires the live bound driver");
  auto SDDL = readControlString(A[2]);
  if (!SDDL)
    return SDDL.takeError();
  if (*SDDL != FrameworkWorldFullAccess)
    return controlError(llvm::Twine("this profile supports only ") +
                        FrameworkWorldFullAccess +
                        "; other DACLs require a caller security-token model");
  auto Address = allocate(HandleSize, false, true);
  if (!Address)
    return Address.takeError();
  DeviceInits.emplace(*Address, DeviceInit{B.Globals, DeviceInitKind::Control});
  return *Address;
}

llvm::Expected<uint64_t>
KernelFramework::callDeviceCreate(llvm::StringRef Name, Binding &B,
                                  llvm::ArrayRef<uint64_t> A, uint8_t) {
  if (auto E = writable(A[3], sizeof(uint64_t)))
    return E;
  if (auto E = Memory.writeInteger(A[3], 0, sizeof(uint64_t)))
    return E;
  if (auto E = writable(A[1], sizeof(uint64_t)))
    return E;
  auto InitAddress = read(A[1]);
  if (!InitAddress)
    return InitAddress.takeError();
  auto I = DeviceInits.find(*InitAddress);
  if (I == DeviceInits.end() || I->second.Binding != B.Globals)
    return controlError("creation requires a live device initializer");
  auto Validation = attributes(A[2], AttributesUse::Device);
  if (!Validation)
    return Validation.takeError();
  if (const auto *Status = std::get_if<uint32_t>(&*Validation))
    return *Status;
  const auto &DeviceAttrs = std::get<Attributes>(*Validation);
  const auto &DriverObject = Objects.at(B.DriverHandle);
  const uint32_t Execution = DeviceAttrs.Execution == ExecutionInherit
                                 ? DriverObject.Execution
                                 : DeviceAttrs.Execution;
  const uint32_t Synchronization =
      DeviceAttrs.Synchronization == SynchronizationInherit
          ? DriverObject.Synchronization
          : DeviceAttrs.Synchronization;
  const auto &Files = I->second.Files;
  const uint32_t FileSynchronization =
      Files.ObjectAttributes.Synchronization == SynchronizationInherit
          ? Synchronization
          : Files.ObjectAttributes.Synchronization;
  if (Files.Enabled && FileSynchronization == SynchronizationDevice &&
      Execution != ExecutionPassive)
    return ControlInvalidDeviceRequest;
  if (I->second.Kind == DeviceInitKind::Control && I->second.Name.empty())
    return controlError(
        "unnamed or autogenerated control devices are not modeled");
  if (!DevicesHost.Create || !DevicesHost.Delete ||
      (I->second.Kind == DeviceInitKind::Pnp && !DevicesHost.CreatePnp))
    return controlError("underlying WDM device host is unavailable");
  auto Wdm =
      I->second.Kind == DeviceInitKind::Control
          ? DevicesHost.Create(I->second.Name, I->second.IoType,
                               I->second.Exclusive)
          : DevicesHost.CreatePnp(
                I->second.PDO, I->second.Name, I->second.IoType,
                I->second.DeviceType.value_or(windows::UnknownDeviceType),
                I->second.Exclusive, I->second.Filter);
  if (!Wdm)
    return Wdm.takeError();
  if (Wdm->Status)
    return Wdm->Status;
  auto Handle =
      createObject(B.Globals, std::get<Attributes>(*Validation), false);
  if (!Handle)
    return llvm::joinErrors(Handle.takeError(),
                            DevicesHost.Delete(Wdm->Address));
  Objects.at(*Handle).Kind = ObjectKind::Device;
  Devices.emplace(*Handle, Device{Wdm->Address, I->second.PDO});
  Devices.at(*Handle).CallerContext = I->second.CallerContext;
  Devices.at(*Handle).Files = I->second.Files;
  if (FileSynchronization == SynchronizationDevice)
    Devices.at(*Handle).Files.SynchronizationObject = *Handle;
  Devices.at(*Handle).Filter = I->second.Filter;
  Devices.at(*Handle).PowerPolicyOwner =
      I->second.PowerPolicyOwner.value_or(!I->second.Filter);
  Devices.at(*Handle).Callbacks = I->second.Callbacks;
  Devices.at(*Handle).Policy.Events = I->second.PowerCallbacks;
  if (I->second.Kind == DeviceInitKind::Pnp)
    PnpDeviceHandles.emplace(I->second.PDO, *Handle);
  if (auto E = Memory.writeInteger(A[3], *Handle, sizeof(uint64_t)))
    return E;
  if (auto E = Memory.writeInteger(A[1], 0, sizeof(uint64_t)))
    return E;
  if (auto E = retire(*InitAddress))
    return E;
  DeviceInits.erase(I);
  return 0;
}

} // namespace neverd::emulation
