//===- KernelModelResources.cpp - PnP resource packet ownership
//------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Deliver the configured raw/translated lists and advance hardware facts at
/// actual lower-driver completion, independently of upper completion state.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

namespace neverd::emulation {

llvm::Error KernelModel::canReleaseResources(uint64_t PDO) const {
  return llvm::joinErrors(
      llvm::joinErrors(MMIO.canRemove(PDO), Interrupts.canRelease(PDO)),
      llvm::joinErrors(DMA.canReleasePDO(PDO), PoFx.canReleasePDO(PDO)));
}

llvm::Error KernelModel::initializePnpResources(ActiveRequest &Request) {
  if (Request.PnpOperation->Minor == DevicePnpRequest::Start &&
      Resources.hasResources(Request.PnpDevice)) {
    auto Raw = Resources.resourceList(Request.PnpDevice, false);
    auto Translated = Resources.resourceList(Request.PnpDevice, true);
    if (!Raw || !Translated)
      return llvm::joinErrors(Raw.takeError(), Translated.takeError());
    auto RawAddress = allocate(Raw->size());
    if (!RawAddress)
      return RawAddress.takeError();
    auto TranslatedAddress = allocate(Translated->size());
    if (!TranslatedAddress)
      return TranslatedAddress.takeError();
    Request.RawResources = *RawAddress;
    Request.TranslatedResources = *TranslatedAddress;
    Request.ResourceListSize = Raw->size();
    if (auto E = Memory.write(*RawAddress, *Raw))
      return E;
    if (auto E = Memory.write(*TranslatedAddress, *Translated))
      return E;
  }
  if (auto E = Memory.writeInteger(Request.Stack +
                                       windows::StackStartResourcesOffset,
                                   Request.RawResources, profile::PointerSize))
    return E;
  return Memory.writeInteger(Request.Stack +
                                 windows::StackStartTranslatedResourcesOffset,
                             Request.TranslatedResources, profile::PointerSize);
}

llvm::Error KernelModel::validatePnpRequestCompletion(
    const ActiveRequest &Request, uint32_t Status, bool ProviderProbe) const {
  if (auto E = Lifecycle.validatePnpCompletion(*Request.PnpTicket, Status))
    return E;
  // START has not assigned resources at the provider probe. All STOP/REMOVE
  // routes, including framework ReleaseHardware, retire resources before
  // forwarding to the provider. Final completion validates both paths.
  if (ProviderProbe && Request.PnpOperation->Minor == DevicePnpRequest::Start)
    return llvm::Error::success();
  const auto Minor = Request.PnpOperation->Minor;
  if (!(Status & profile::NTStatusFailureMask) &&
      (Minor == DevicePnpRequest::Stop || Minor == DevicePnpRequest::Remove))
    if (auto E = PoFx.canReleasePDO(Request.PnpDevice))
      return E;
  return Resources.validateCompletion(Request.PnpDevice, Minor, Status);
}

llvm::Error KernelModel::publishProviderHardware(ActiveRequest &Request,
                                                 uint32_t Status) {
  if (Request.PnpOperation &&
      Request.PnpOperation->Minor == DevicePnpRequest::Start)
    return Resources.completeLowerStart(Request.PnpDevice, Status);
  if (Request.PowerOperation &&
      Request.PowerOperation->Type == DriverPowerType::Device &&
      Request.PowerOperation->Minor == DevicePowerRequest::Set &&
      !(Status & profile::NTStatusFailureMask)) {
    const auto State =
        static_cast<DevicePowerState>(Request.PowerOperation->State);
    Resources.setPhysicalPower(Request.PnpDevice, State);
    if (State == DevicePowerState::D3 && Framework) {
      auto Cold = Framework->allowsD3Cold(Request.PnpDevice);
      if (!Cold)
        return Cold.takeError();
      if (*Cold) {
        auto Snapshot = Lifecycle.snapshot(Request.PnpDevice);
        if (!Snapshot)
          return Snapshot.takeError();
        auto System = Snapshot->SystemPower;
        // A child D3 completion precedes its retained system IRP. The pending
        // SET target, rather than the last completed state, owns wake policy.
        if (Snapshot->SystemPowerOperation)
          for (const auto &[IRP, Active] : Requests) {
            (void)IRP;
            if (Active.PowerTicket != Snapshot->SystemPowerOperation ||
                !Active.PowerOperation || Active.Completed)
              continue;
            const auto &Operation = *Active.PowerOperation;
            if (Operation.Type == DriverPowerType::System &&
                Operation.Minor == DevicePowerRequest::Set)
              System = static_cast<SystemPowerState>(Operation.State);
            break;
          }
        if (auto E = Resources.enterD3Cold(
                Request.PnpDevice, System,
                FrameworkWakeIRPs.contains(Request.PnpDevice)))
          return E;
      }
    }
  }
  return llvm::Error::success();
}

} // namespace neverd::emulation
