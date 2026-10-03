//===- KernelFrameworkPowerWake.cpp - KMDF provider child wake -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Capture child wake obligations by provider identity and START epoch. The
/// host completes retained WAIT_WAKE packets; framework state owns callback
/// reasons, propagation and cancellation of child-only parent arms.
///
//===----------------------------------------------------------------------===//

#include "KernelFramework.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
llvm::Error wakeError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "KMDF child wake: " + Text);
}
} // namespace

bool KernelFramework::isLiveWakeChild(
    const KernelPowerPolicy::WakeChild &Child) const {
  const auto Handle = PnpDeviceHandles.find(Child.PDO);
  if (Handle == PnpDeviceHandles.end())
    return false;
  const auto &Policy = Devices.at(Handle->second).Policy;
  return Policy.Started && Policy.Epoch == Child.Epoch &&
         Policy.Armed == KernelPowerPolicy::WakeSource::Sx &&
         !Policy.WakeTriggered;
}

llvm::Expected<std::vector<KernelPowerPolicy::WakeChild>>
KernelFramework::armedWakeChildren(uint64_t Device) const {
  const auto &D = Devices.at(Device);
  std::vector<KernelPowerPolicy::WakeChild> Result;
  if (!D.Policy.Wake ||
      (!D.Policy.Wake->ArmForChildren && !D.Policy.Wake->PropagateParentWake))
    return Result;
  if (!PowerHost.Children)
    return wakeError("child wake requires explicit provider topology");
  auto Children = PowerHost.Children(D.PDO);
  if (!Children)
    return Children.takeError();
  for (uint64_t PDO : *Children) {
    const auto Handle = PnpDeviceHandles.find(PDO);
    if (Handle == PnpDeviceHandles.end())
      continue;
    KernelPowerPolicy::WakeChild Child{PDO,
                                       Devices.at(Handle->second).Policy.Epoch};
    if (isLiveWakeChild(Child))
      Result.push_back(Child);
  }
  return Result;
}

llvm::Error KernelFramework::validateWakeEnrollment(uint64_t Device) const {
  if (!PowerHost.Children)
    return llvm::Error::success();
  const uint64_t PDO = Devices.at(Device).PDO;
  for (const auto &[Handle, Parent] : Devices) {
    const auto &Policy = Parent.Policy;
    const bool Leaving = std::any_of(
        PnpTransitions.begin(), PnpTransitions.end(), [&](const auto &Entry) {
          return Entry.second.Device == Handle && !Entry.second.Entering;
        });
    if (!Policy.Started || !Policy.SystemSleeping ||
        (Parent.InD0 && !Leaving) || !Policy.Wake ||
        (!Policy.Wake->ArmForChildren && !Policy.Wake->PropagateParentWake))
      continue;
    auto Children = PowerHost.Children(Parent.PDO);
    if (!Children)
      return Children.takeError();
    if (std::find(Children->begin(), Children->end(), PDO) != Children->end())
      return wakeError("late child enrollment requires the parent to return "
                       "to D0 before another Sx arm");
  }
  return llvm::Error::success();
}

llvm::Error KernelFramework::powerPolicyWake(uint64_t PDO) {
  const auto Handle = PnpDeviceHandles.find(PDO);
  if (Handle == PnpDeviceHandles.end())
    return wakeError("wake observation requires a framework PDO");
  const auto &Policy = Devices.at(Handle->second).Policy;
  if (Policy.Armed == KernelPowerPolicy::WakeSource::None ||
      Policy.WakeTriggered)
    return wakeError("wake signal requires one successfully armed source");

  std::vector<uint64_t> Handles;
  std::vector<uint64_t> WdmDevices;
  auto Collect = [&](auto &&Self, uint64_t Current) -> llvm::Error {
    if (std::find(Handles.begin(), Handles.end(), Current) != Handles.end())
      return wakeError("propagation requires an acyclic provider tree");
    Handles.push_back(Current);
    const auto &D = Devices.at(Current);
    WdmDevices.push_back(D.Wdm);
    if (D.Policy.Armed != KernelPowerPolicy::WakeSource::Sx || !D.Policy.Wake ||
        !D.Policy.Wake->PropagateParentWake)
      return llvm::Error::success();
    for (const auto &Child : D.Policy.ArmedChildren)
      if (isLiveWakeChild(Child))
        if (auto E = Self(Self, PnpDeviceHandles.at(Child.PDO)))
          return E;
    return llvm::Error::success();
  };
  if (auto E = Collect(Collect, Handle->second))
    return E;
  if (PowerHost.CompleteWakes) {
    if (auto E = PowerHost.CompleteWakes(WdmDevices.front(), WdmDevices))
      return E;
  } else {
    if (WdmDevices.size() != 1 || !PowerHost.FinishWake)
      return wakeError("wake signal lost its provider WAIT_WAKE bridge");
    if (auto E = PowerHost.FinishWake(WdmDevices.front(), true))
      return E;
  }
  for (uint64_t Current : Handles) {
    auto &P = Devices.at(Current).Policy;
    P.WakeTriggered = true;
    P.IdleSince.reset();
    P.PowerUpRequested = true;
  }
  // An Sx indication leaves each explicit system S0 transaction authoritative.
  return llvm::Error::success();
}

llvm::Error KernelFramework::retireChildWake(uint64_t Device,
                                             PnpTransition &Transition) {
  const auto &D = Devices.at(Device);
  std::vector<KernelPowerPolicy::WakeChild> Retired{{D.PDO, D.Policy.Epoch}};
  std::vector<uint64_t> Parents;
  std::vector<uint64_t> WdmDevices{D.Wdm};
  const auto IsRetired = [&](const KernelPowerPolicy::WakeChild &Child) {
    return std::any_of(Retired.begin(), Retired.end(), [&](const auto &Entry) {
      return Entry.PDO == Child.PDO && Entry.Epoch == Child.Epoch;
    });
  };
  for (size_t I = 0; I < Retired.size(); ++I) {
    // Copy the identity because discovering an ancestor can grow Retired.
    const auto Child = Retired[I];
    for (const auto &[Handle, Parent] : Devices) {
      const auto &P = Parent.Policy;
      if (P.ArmedForDevice || P.WakeTriggered ||
          P.Armed != KernelPowerPolicy::WakeSource::Sx ||
          std::find(Parents.begin(), Parents.end(), Handle) != Parents.end())
        continue;
      const bool Depends = std::any_of(
          P.ArmedChildren.begin(), P.ArmedChildren.end(),
          [&](const auto &Entry) {
            return Entry.PDO == Child.PDO && Entry.Epoch == Child.Epoch;
          });
      if (!Depends || !std::all_of(P.ArmedChildren.begin(),
                                   P.ArmedChildren.end(), IsRetired))
        continue;
      Parents.push_back(Handle);
      WdmDevices.push_back(Parent.Wdm);
      Retired.push_back({Parent.PDO, P.Epoch});
    }
  }
  if (PowerHost.CancelWakes) {
    if (auto E = PowerHost.CancelWakes(WdmDevices))
      return E;
  } else {
    if (WdmDevices.size() != 1 || !PowerHost.FinishWake)
      return wakeError("wake cancellation requires its retained packet batch");
    if (auto E = PowerHost.FinishWake(D.Wdm, false))
      return E;
  }
  for (auto &[Handle, Parent] : Devices)
    std::erase_if(Parent.Policy.ArmedChildren, IsRetired);
  for (uint64_t Parent : Parents) {
    Devices.at(Parent).Policy.Armed = KernelPowerPolicy::WakeSource::None;
    Transition.WakeParentsToDisarm.push_back(Parent);
  }
  return llvm::Error::success();
}

} // namespace neverd::emulation
