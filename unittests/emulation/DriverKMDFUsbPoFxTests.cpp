//===- DriverKMDFUsbPoFxTests.cpp - Genuine USB PoFx grants -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "fixtures/driver_kmdf_usb_idle_test.h"
#include "gtest/gtest.h"
#include "os/windows/KernelUsbIdle.h"
#include "os/windows/WindowsKernelLayout.h"

#include "neverd/emulation/DriverSession.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
#ifdef NEVERD_KMDF_USB_IDLE_FIXTURE
constexpr llvm::StringLiteral Function = "usb-function";
constexpr uint64_t D2Delay = 11, D0Delay = 13;

std::vector<const char *> images() {
  return {
      NEVERD_KMDF_USB_IDLE_FIXTURE,
#ifdef NEVERD_KMDF_USB_IDLE_CFG_FIXTURE
      NEVERD_KMDF_USB_IDLE_CFG_FIXTURE,
#endif
  };
}

DriverRequest pnp(llvm::StringRef ID, DevicePnpRequest Minor) {
  DriverRequest Request;
  Request.Kind = DriverRequestKind::Pnp;
  Request.DeviceID = ID.str();
  Request.Pnp = DriverPnpOperation{Minor, {windows::StatusSuccess, 0}};
  return Request;
}

DriverRequest file(llvm::StringRef ID, DriverRequestKind Kind,
                   uint32_t Code = KmdfUsbPoFxSnapshotIoctl) {
  DriverRequest Request;
  Request.Kind = Kind;
  Request.DeviceID = ID.str();
  Request.File = ID == Function ? 1 : ID == "usb-sibling" ? 2 : 3;
  if (Kind == DriverRequestKind::DeviceControl) {
    Request.ControlCode = Code;
    if (Code == KmdfUsbPoFxSnapshotIoctl)
      Request.OutputSize = KmdfUsbPoFxSnapshotWords * sizeof(uint32_t);
  } else if (Kind == DriverRequestKind::Read)
    Request.OutputSize = sizeof(uint32_t);
  return Request;
}

DriverPowerOperation power(DevicePowerState State) {
  DriverPowerOperation Operation;
  Operation.State = uint32_t(State);
  Operation.BusCompletion = {windows::StatusSuccess,
                             State == DevicePowerState::D2 ? D2Delay : D0Delay};
  return Operation;
}

DriverPnpDevice device(llvm::StringRef ID, bool RemoteWake = false) {
  DriverPnpDevice Device;
  Device.ID = ID.str();
  Device.InitialDevicePower = DevicePowerState::D0;
  Device.InitialSystemPower = SystemPowerState::Working;
  Device.WakeCapabilities = DriverWakeCapabilities{RemoteWake, false};
  Device.UsbIdle = DriverUsbIdleConfig{DriverUsbIdleRole::IndependentFunction,
                                       RemoteWake, DevicePowerState::D2};
  Device.RequestedDevicePower = {power(DevicePowerState::D2),
                                 power(DevicePowerState::D0)};
  return Device;
}

DriverOptions options(char Mode, bool RemoteWake = false,
                      bool DirectRead = true, uint8_t Behavior = 0) {
  DriverOptions Options;
  Options.ServiceName = std::string("NeverDKmdfUsbIdle") + Mode;
  Options.Unload = true;
  Options.PnpDevices.push_back(device(Function, RemoteWake));
  Options.Requests = {pnp(Function, DevicePnpRequest::Start),
                      file(Function, DriverRequestKind::Create)};
  if (DirectRead)
    Options.Requests.push_back(file(Function, DriverRequestKind::DeviceControl,
                                    KmdfUsbDirectReadIoctl));
  if (Behavior) {
    auto Request =
        file(Function, DriverRequestKind::DeviceControl, KmdfUsbBehaviorIoctl);
    Request.Input = {Behavior};
    Options.Requests.push_back(std::move(Request));
  }
  return Options;
}

size_t observe(DriverOptions &Options, llvm::StringRef ID = Function,
               std::optional<DriverPowerPolicyAction> Action = std::nullopt,
               llvm::StringRef Target = {}) {
  auto Request = file(ID, DriverRequestKind::DeviceControl);
  if (Action)
    Request.PowerPolicyEvents = {
        {0, Target.empty() ? ID.str() : Target.str(), *Action}};
  Options.Requests.push_back(std::move(Request));
  return Options.Requests.size() - 1;
}

void remove(DriverOptions &Options, llvm::StringRef ID = Function) {
  Options.Requests.push_back(file(ID, DriverRequestKind::Cleanup));
  Options.Requests.push_back(file(ID, DriverRequestKind::Close));
  Options.Requests.push_back(pnp(ID, DevicePnpRequest::QueryRemove));
  Options.Requests.push_back(pnp(ID, DevicePnpRequest::Remove));
}

uint32_t word(const DriverRequestResult &Request, size_t Index) {
  uint32_t Value = 0;
  if (Request.Output.size() >= (Index + 1) * sizeof(Value))
    for (size_t I = 0; I < sizeof(Value); ++I)
      Value |= uint32_t(Request.Output[Index * sizeof(Value) + I]) << (I * 8);
  return Value;
}

const DriverRequestResult *scenario(const DriverResult &Result, size_t Index) {
  for (const auto &Request : Result.Requests)
    if (Request.Origin == DriverRequestOrigin::Scenario && Index-- == 0)
      return &Request;
  return nullptr;
}

const DriverRequestResult *idle(const DriverResult &Result,
                                llvm::StringRef ID = Function) {
  for (const auto &Request : Result.Requests)
    if (Request.Origin == DriverRequestOrigin::FrameworkUsbIdle &&
        Request.DeviceID == ID)
      return &Request;
  return nullptr;
}

std::vector<const DriverRequestResult *> powers(const DriverResult &Result,
                                                llvm::StringRef ID = Function) {
  std::vector<const DriverRequestResult *> Values;
  for (const auto &Request : Result.Requests)
    if (Request.Origin == DriverRequestOrigin::FrameworkPowerPolicy &&
        Request.DeviceID == ID)
      Values.push_back(&Request);
  return Values;
}

void clean(const DriverResult &Result) {
  ASSERT_EQ(Result.Stop, DriverStopReason::Returned) << Result.Diagnostic;
  EXPECT_TRUE(Result.UnloadCompleted);
  EXPECT_NE(std::find(Result.Messages.begin(), Result.Messages.end(),
                      "KMDF USB idle: unload live 0 failures 0\n"),
            Result.Messages.end());
  for (const auto &Request : Result.Requests) {
    if (Request.Origin != DriverRequestOrigin::Scenario)
      continue;
    EXPECT_TRUE(Request.Completed);
    EXPECT_EQ(Request.IOStatus, windows::StatusSuccess);
    if (Request.ControlCode == KmdfUsbPoFxSnapshotIoctl &&
        Request.Kind == DriverRequestKind::DeviceControl) {
      EXPECT_EQ(Request.Output.size(),
                KmdfUsbPoFxSnapshotWords * sizeof(uint32_t));
      EXPECT_EQ(word(Request, KmdfUsbFailures), 0u);
    }
  }
}

// The OS grant and component state are explicit observations, not a local
// timer. WithHint permits the OS decision only after its advertised lower
// bound.
void grant(DriverOptions &Options) {
  auto Request = file(Function, DriverRequestKind::DeviceControl);
  Request.PowerPolicyEvents = {
      {0, Function.str(), DriverPowerPolicyAction::Idle},
      {3, Function.str(), DriverPowerPolicyAction::ComponentIdleState, 0, 1},
      {KmdfUsbIdleTimeout100ns + 1, Function.str(),
       DriverPowerPolicyAction::PowerNotRequired}};
  Options.Requests.push_back(std::move(Request));
}

void readyAfterComponent(const DriverRequestResult &Snapshot,
                         uint32_t Entries) {
  EXPECT_EQ(word(Snapshot, KmdfUsbInD0), 1u);
  EXPECT_EQ(word(Snapshot, KmdfUsbD0Entries), Entries);
  EXPECT_EQ(word(Snapshot, KmdfUsbPoFxPosts), 1u);
  EXPECT_EQ(word(Snapshot, KmdfUsbPoFxPres), 0u);
  EXPECT_EQ(word(Snapshot, KmdfUsbPoFxActive), 1u);
  EXPECT_EQ(word(Snapshot, KmdfUsbPoFxF0Transitions), 1u);
  EXPECT_EQ(word(Snapshot, KmdfUsbPoFxF1Transitions), 1u);
  EXPECT_EQ(word(Snapshot, KmdfUsbPoFxCurrentState), 0u);
  EXPECT_EQ(word(Snapshot, KmdfUsbReadsDelivered), 1u);
  EXPECT_LT(word(Snapshot, KmdfUsbEntrySequence),
            word(Snapshot, KmdfUsbPoFxF0Sequence));
  EXPECT_LT(word(Snapshot, KmdfUsbPoFxF0Sequence),
            word(Snapshot, KmdfUsbPoFxActiveSequence));
  EXPECT_LT(word(Snapshot, KmdfUsbPoFxActiveSequence),
            word(Snapshot, KmdfUsbReadDeliverySequence));
}

TEST(DriverKMDFUsbPoFx, IdleWithoutOsDecisionCannotSubmitUsb) {
  for (const auto *Image : images())
    for (char Mode :
         {char(KmdfUsbSystemManagedMode), char(KmdfUsbSystemManagedHintMode)}) {
      auto Input = options(Mode);
      observe(Input, Function, DriverPowerPolicyAction::Idle);
      const auto Snapshot = observe(Input);
      remove(Input);
      auto Result = emulateDriver(Image, Input);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      clean(*Result);
      const auto *State = scenario(*Result, Snapshot);
      ASSERT_NE(State, nullptr);
      EXPECT_EQ(word(*State, KmdfUsbInD0), 1u);
      EXPECT_EQ(word(*State, KmdfUsbPoFxIdleConditions), 1u);
      EXPECT_EQ(word(*State, KmdfUsbD0Exits), 0u);
      EXPECT_EQ(idle(*Result), nullptr);
      EXPECT_TRUE(powers(*Result).empty());
    }
}

TEST(DriverKMDFUsbPoFx,
     OsGrantRemainsInD0AndActivityCancelsBeforeBusPermission) {
  for (const auto *Image : images())
    for (char Mode :
         {char(KmdfUsbSystemManagedMode), char(KmdfUsbSystemManagedHintMode)})
      for (bool Direct : {false, true}) {
        auto Input = options(Mode, false, Direct);
        grant(Input);
        const auto Parked = observe(Input);
        const auto Read = Input.Requests.size();
        Input.Requests.push_back(file(Function, DriverRequestKind::Read));
        const auto Resumed = observe(Input);
        remove(Input);
        auto Result = emulateDriver(Image, Input);
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        clean(*Result);
        const auto *Before = scenario(*Result, Parked);
        const auto *After = scenario(*Result, Resumed);
        const auto *Data = scenario(*Result, Read);
        ASSERT_NE(Before, nullptr);
        ASSERT_NE(After, nullptr);
        ASSERT_NE(Data, nullptr);
        EXPECT_EQ(word(*Before, KmdfUsbInD0), 1u);
        EXPECT_EQ(word(*Before, KmdfUsbD0Exits), 0u);
        EXPECT_EQ(word(*Before, KmdfUsbPoFxActive), 0u);
        EXPECT_EQ(word(*Before, KmdfUsbPoFxCurrentState), 1u);
        EXPECT_EQ(word(*Data, 0), KmdfUsbReadMarker);
        readyAfterComponent(*After, 1);
        EXPECT_EQ(word(*After, KmdfUsbReadsRouted), Direct ? 0u : 1u);
        EXPECT_TRUE(powers(*Result).empty());
        const auto *Idle = idle(*Result);
        ASSERT_NE(Idle, nullptr);
        ASSERT_TRUE(Idle->UsbIdle);
        EXPECT_TRUE(Idle->UsbIdle->BusReceivedAt100ns);
        EXPECT_FALSE(Idle->UsbIdle->CallbackEnteredAt100ns);
        EXPECT_FALSE(Idle->UsbIdle->D2IRP);
        EXPECT_EQ(Idle->UsbIdle->CompletionCause,
                  DriverUsbIdleCompletionCause::Cancel);
      }
}

TEST(DriverKMDFUsbPoFx, BothGrantsUseRealD2AndReadWaitsForD0AndComponent) {
  for (const auto *Image : images())
    for (char Mode :
         {char(KmdfUsbSystemManagedMode), char(KmdfUsbSystemManagedHintMode)})
      for (uint64_t Base : {uint64_t(0), uint64_t(0x190000000)}) {
        auto Input = options(Mode);
        Input.LoadAddress = Base;
        grant(Input);
        observe(Input, Function, DriverPowerPolicyAction::UsbIdlePermission);
        const auto Suspended = observe(Input);
        const auto Read = Input.Requests.size();
        Input.Requests.push_back(file(Function, DriverRequestKind::Read));
        const auto Resumed = observe(Input);
        remove(Input);
        auto Result = emulateDriver(Image, Input);
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        clean(*Result);
        const auto *Before = scenario(*Result, Suspended);
        const auto *After = scenario(*Result, Resumed);
        const auto *Data = scenario(*Result, Read);
        ASSERT_NE(Before, nullptr);
        ASSERT_NE(After, nullptr);
        ASSERT_NE(Data, nullptr);
        EXPECT_EQ(word(*Before, KmdfUsbInD0), 0u);
        EXPECT_EQ(word(*Before, KmdfUsbPoFxActive), 0u);
        EXPECT_EQ(word(*Before, KmdfUsbTargetState),
                  uint32_t(DevicePowerState::D2));
        EXPECT_EQ(word(*Data, 0), KmdfUsbReadMarker);
        readyAfterComponent(*After, 2);
        EXPECT_EQ(word(*After, KmdfUsbReadsRouted), 0u);
        const auto *Idle = idle(*Result);
        ASSERT_NE(Idle, nullptr);
        ASSERT_TRUE(Idle->UsbIdle);
        const auto Power = powers(*Result);
        ASSERT_EQ(Power.size(), 2u);
        ASSERT_TRUE(Power[0]->Power);
        ASSERT_TRUE(Power[1]->Power);
        EXPECT_EQ(Power[0]->ResponseIndex, 0u);
        EXPECT_EQ(Power[1]->ResponseIndex, 1u);
        EXPECT_EQ(Idle->UsbIdle->D2IRP, Power[0]->IRP);
        EXPECT_EQ(Idle->UsbIdle->D2CompletedAt100ns,
                  Power[0]->Power->BusCompletedAt100ns);
        ASSERT_TRUE(Idle->UsbIdle->CallbackReturnedAt100ns);
        ASSERT_TRUE(Power[0]->Power->BusCompletedAt100ns);
        EXPECT_LE(*Power[0]->Power->BusCompletedAt100ns,
                  *Idle->UsbIdle->CallbackReturnedAt100ns);
        EXPECT_EQ(Idle->UsbIdle->CompletionCause,
                  DriverUsbIdleCompletionCause::Cancel);
        ASSERT_TRUE(Idle->UsbIdle->CompletedAt100ns);
        ASSERT_TRUE(Power[1]->Power->BusCompletedAt100ns);
        EXPECT_LT(*Idle->UsbIdle->CompletedAt100ns,
                  *Power[1]->Power->BusCompletedAt100ns);
      }
}

TEST(DriverKMDFUsbPoFx, RemoteWakeRestoresBothDeviceAndComponentBeforeRead) {
  for (const auto *Image : images())
    for (char Mode :
         {char(KmdfUsbSystemManagedMode), char(KmdfUsbSystemManagedHintMode)}) {
      auto Input = options(Mode, true);
      grant(Input);
      observe(Input, Function, DriverPowerPolicyAction::UsbIdlePermission);
      observe(Input, Function, DriverPowerPolicyAction::Wake);
      const auto Woken = observe(Input);
      Input.Requests.push_back(file(Function, DriverRequestKind::Read));
      const auto Resumed = observe(Input);
      remove(Input);
      auto Result = emulateDriver(Image, Input);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      clean(*Result);
      const auto *BeforeRead = scenario(*Result, Woken);
      ASSERT_NE(BeforeRead, nullptr);
      EXPECT_EQ(word(*BeforeRead, KmdfUsbInD0), 1u);
      EXPECT_EQ(word(*BeforeRead, KmdfUsbD0Entries), 2u);
      EXPECT_EQ(word(*BeforeRead, KmdfUsbPoFxActive), 1u);
      EXPECT_EQ(word(*BeforeRead, KmdfUsbPoFxF0Transitions), 1u);
      EXPECT_EQ(word(*BeforeRead, KmdfUsbPoFxCurrentState), 0u);
      EXPECT_EQ(word(*BeforeRead, KmdfUsbReadsDelivered), 0u);
      EXPECT_LT(word(*BeforeRead, KmdfUsbEntrySequence),
                word(*BeforeRead, KmdfUsbPoFxF0Sequence));
      EXPECT_LT(word(*BeforeRead, KmdfUsbPoFxF0Sequence),
                word(*BeforeRead, KmdfUsbPoFxActiveSequence));
      const auto *State = scenario(*Result, Resumed);
      ASSERT_NE(State, nullptr);
      readyAfterComponent(*State, 2);
      EXPECT_EQ(word(*State, KmdfUsbTriggers), 1u);
      EXPECT_EQ(word(*State, KmdfUsbDisarms), 1u);
      ASSERT_EQ(powers(*Result).size(), 2u);
      const auto *Idle = idle(*Result);
      ASSERT_NE(Idle, nullptr);
      ASSERT_TRUE(Idle->UsbIdle);
      EXPECT_EQ(Idle->UsbIdle->CompletionCause,
                DriverUsbIdleCompletionCause::DeviceD0);
      EXPECT_EQ(Idle->IOStatus, windows::StatusSuccess);
    }
}

TEST(DriverKMDFUsbPoFx, ArmFailureRestoresComponentWithoutD2) {
  for (const auto *Image : images())
    for (char Mode :
         {char(KmdfUsbSystemManagedMode), char(KmdfUsbSystemManagedHintMode)}) {
      constexpr uint8_t Behavior = KmdfUsbFailArmBehavior;
      auto Input = options(Mode, true, true, Behavior);
      grant(Input);
      observe(Input, Function, DriverPowerPolicyAction::UsbIdlePermission);
      const auto Restored = observe(Input);
      Input.Requests.push_back(file(Function, DriverRequestKind::Read));
      const auto Resumed = observe(Input);
      remove(Input);
      auto Result = emulateDriver(Image, Input);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      clean(*Result);
      const auto *BeforeRead = scenario(*Result, Restored);
      ASSERT_NE(BeforeRead, nullptr);
      EXPECT_EQ(word(*BeforeRead, KmdfUsbInD0), 1u);
      EXPECT_EQ(word(*BeforeRead, KmdfUsbD0Entries), 1u);
      EXPECT_EQ(word(*BeforeRead, KmdfUsbPoFxActive), 1u);
      EXPECT_EQ(word(*BeforeRead, KmdfUsbPoFxF0Transitions), 1u);
      EXPECT_EQ(word(*BeforeRead, KmdfUsbPoFxF1Transitions), 1u);
      EXPECT_EQ(word(*BeforeRead, KmdfUsbPoFxCurrentState), 0u);
      EXPECT_EQ(word(*BeforeRead, KmdfUsbReadsRouted), 0u);
      EXPECT_EQ(word(*BeforeRead, KmdfUsbReadsDelivered), 0u);
      EXPECT_EQ(word(*BeforeRead, KmdfUsbStopCalls), 0u);
      EXPECT_LT(word(*BeforeRead, KmdfUsbEntrySequence),
                word(*BeforeRead, KmdfUsbPoFxF0Sequence));
      EXPECT_LT(word(*BeforeRead, KmdfUsbPoFxF0Sequence),
                word(*BeforeRead, KmdfUsbPoFxActiveSequence));
      const auto *State = scenario(*Result, Resumed);
      ASSERT_NE(State, nullptr);
      readyAfterComponent(*State, 1);
      EXPECT_EQ(word(*BeforeRead, KmdfUsbPoFxActiveSequence),
                word(*State, KmdfUsbPoFxActiveSequence));
      EXPECT_EQ(word(*State, KmdfUsbArms), 1u);
      EXPECT_EQ(word(*State, KmdfUsbD0Exits), 0u);
      EXPECT_TRUE(powers(*Result).empty());
      const auto *Idle = idle(*Result);
      ASSERT_NE(Idle, nullptr);
      ASSERT_TRUE(Idle->UsbIdle);
      EXPECT_TRUE(Idle->UsbIdle->CallbackEnteredAt100ns);
      EXPECT_TRUE(Idle->UsbIdle->CallbackReturnedAt100ns);
      EXPECT_FALSE(Idle->UsbIdle->D2IRP);
    }
}

TEST(DriverKMDFUsbPoFx,
     StopIdleBeforeDuringAndAfterPermissionSettlesBothOwners) {
  for (const auto *Image : images())
    for (unsigned Stage : {0u, 1u, 2u}) {
      auto Input = options(KmdfUsbSystemManagedHintMode, true, true,
                           Stage == 1 ? KmdfUsbStopInArmBehavior : 0);
      grant(Input);
      if (Stage != 0)
        observe(Input, Function, DriverPowerPolicyAction::UsbIdlePermission);
      if (Stage != 1)
        Input.Requests.push_back(file(
            Function, DriverRequestKind::DeviceControl, KmdfUsbStopIdleIoctl));
      Input.Requests.push_back(file(Function, DriverRequestKind::Read));
      const auto Resumed = observe(Input);
      Input.Requests.push_back(file(Function, DriverRequestKind::DeviceControl,
                                    KmdfUsbResumeIdleIoctl));
      remove(Input);
      auto Result = emulateDriver(Image, Input);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      clean(*Result);
      const auto *State = scenario(*Result, Resumed);
      ASSERT_NE(State, nullptr);
      readyAfterComponent(*State, Stage == 0 ? 1 : 2);
      EXPECT_EQ(word(*State, KmdfUsbStopCalls), 1u);
      EXPECT_EQ(powers(*Result).size(), Stage == 0 ? 0u : 2u);
      const auto *Idle = idle(*Result);
      ASSERT_NE(Idle, nullptr);
      ASSERT_TRUE(Idle->UsbIdle);
      EXPECT_EQ(Idle->UsbIdle->CompletionCause,
                DriverUsbIdleCompletionCause::Cancel);
    }
}

TEST(DriverKMDFUsbPoFx, RemovalWithoutUsbPermissionRetiresRegistration) {
  for (const auto *Image : images()) {
    auto Input = options(KmdfUsbSystemManagedHintMode);
    grant(Input);
    remove(Input);
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    EXPECT_TRUE(powers(*Result).empty());
    const auto *Idle = idle(*Result);
    ASSERT_NE(Idle, nullptr);
    ASSERT_TRUE(Idle->UsbIdle);
    EXPECT_EQ(Idle->UsbIdle->CompletionCause,
              DriverUsbIdleCompletionCause::Cancel);
    EXPECT_FALSE(Idle->UsbIdle->CallbackEnteredAt100ns);
  }
}
TEST(DriverKMDFUsbPoFx, StopAndRestartUseNewPoFxAndUsbOwners) {
  for (const auto *Image : images()) {
    auto Input = options(KmdfUsbSystemManagedHintMode);
    grant(Input);
    Input.Requests.push_back(pnp(Function, DevicePnpRequest::QueryStop));
    Input.Requests.push_back(pnp(Function, DevicePnpRequest::Stop));
    Input.Requests.push_back(pnp(Function, DevicePnpRequest::Start));
    grant(Input);
    observe(Input, Function, DriverPowerPolicyAction::UsbIdlePermission);
    Input.Requests.push_back(file(Function, DriverRequestKind::Read));
    const auto Resumed = observe(Input);
    remove(Input);
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    const auto *State = scenario(*Result, Resumed);
    ASSERT_NE(State, nullptr);
    EXPECT_EQ(word(*State, KmdfUsbPoFxPosts), 2u);
    EXPECT_EQ(word(*State, KmdfUsbPoFxPres), 1u);
    EXPECT_EQ(word(*State, KmdfUsbPoFxActive), 1u);
    EXPECT_EQ(word(*State, KmdfUsbReadsDelivered), 1u);
    std::vector<const DriverRequestResult *> Packets;
    for (const auto &Request : Result->Requests)
      if (Request.Origin == DriverRequestOrigin::FrameworkUsbIdle)
        Packets.push_back(&Request);
    ASSERT_EQ(Packets.size(), 2u);
    ASSERT_TRUE(Packets[0]->UsbIdle);
    ASSERT_TRUE(Packets[1]->UsbIdle);
    EXPECT_NE(Packets[0]->IRP, Packets[1]->IRP);
    EXPECT_NE(Packets[0]->UsbIdle->StartEpoch, Packets[1]->UsbIdle->StartEpoch);
    EXPECT_FALSE(Packets[0]->UsbIdle->CallbackEnteredAt100ns);
    EXPECT_TRUE(Packets[1]->UsbIdle->CallbackEnteredAt100ns);
    ASSERT_EQ(Result->PowerPolicyEvents.back().UsbIdleMembers.size(), 1u);
    EXPECT_EQ(Result->PowerPolicyEvents.back().UsbIdleMembers[0].IRP,
              Packets[1]->IRP);
    EXPECT_EQ(Result->PowerPolicyEvents.back().UsbIdleMembers[0].StartEpoch,
              Packets[1]->UsbIdle->StartEpoch);
    EXPECT_EQ(powers(*Result).size(), 2u);
  }
}

#else
TEST(DriverKMDFUsbPoFx, RequiresGenuineWdkFixture) {
  GTEST_SKIP() << "NEVERD_KMDF_USB_IDLE_FIXTURE requires a genuine WDK fixture";
}
#endif
} // namespace
} // namespace neverd::emulation
