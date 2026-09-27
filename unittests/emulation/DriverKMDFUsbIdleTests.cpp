//===- DriverKMDFUsbIdleTests.cpp - Genuine framework USB idle -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "fixtures/driver_kmdf_usb_idle_test.h"
#include "gtest/gtest.h"
#include "windows/KernelUsbIdle.h"
#include "windows/WindowsKernelLayout.h"

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
                   uint32_t Code = KmdfUsbSnapshotIoctl) {
  DriverRequest Request;
  Request.Kind = Kind;
  Request.DeviceID = ID.str();
  Request.File = ID == Function ? 1 : ID == "usb-sibling" ? 2 : 3;
  if (Kind == DriverRequestKind::DeviceControl) {
    Request.ControlCode = Code;
    if (Code == KmdfUsbSnapshotIoctl)
      Request.OutputSize = KmdfUsbSnapshotWords * sizeof(uint32_t);
  } else if (Kind == DriverRequestKind::Read)
    Request.OutputSize = sizeof(uint32_t);
  return Request;
}

DriverRequest configure(llvm::StringRef ID, bool Maximum = false,
                        bool FailArm = false, bool StopInArm = false) {
  auto Request =
      file(ID, DriverRequestKind::DeviceControl, KmdfUsbConfigureIoctl);
  Request.Input = {uint8_t(ID == Function ? 1 : 2), 1, uint8_t(Maximum),
                   uint8_t(FailArm), uint8_t(StopInArm)};
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

DriverOptions options(bool RemoteWake = false, bool Maximum = false,
                      bool FailArm = false, bool StopInArm = false) {
  DriverOptions Options;
  Options.ServiceName = "NeverDKmdfUsbIdle";
  Options.Unload = true;
  Options.PnpDevices.push_back(device(Function, RemoteWake));
  Options.Requests = {pnp(Function, DevicePnpRequest::Start),
                      file(Function, DriverRequestKind::Create),
                      configure(Function, Maximum, FailArm, StopInArm)};
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
    if (Request.ControlCode == KmdfUsbSnapshotIoctl &&
        Request.Kind == DriverRequestKind::DeviceControl) {
      EXPECT_EQ(Request.Output.size(), KmdfUsbSnapshotWords * sizeof(uint32_t));
      EXPECT_EQ(word(Request, KmdfUsbFailures), 0u);
    }
  }
}

TEST(DriverKMDFUsbIdle, NoPermissionRetainsAFrameworkPacketInD0) {
  for (const auto *Image : images()) {
    auto Input = options();
    observe(Input, Function, DriverPowerPolicyAction::Idle);
    const auto Snapshot = observe(Input);
    remove(Input);
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    const auto *State = scenario(*Result, Snapshot);
    ASSERT_NE(State, nullptr);
    EXPECT_EQ(word(*State, KmdfUsbInD0), 1u);
    EXPECT_EQ(word(*State, KmdfUsbD0Entries), 1u);
    EXPECT_EQ(word(*State, KmdfUsbD0Exits), 0u);
    EXPECT_EQ(word(*State, KmdfUsbArms), 0u);
    EXPECT_TRUE(powers(*Result).empty());
    const auto *Idle = idle(*Result);
    ASSERT_NE(Idle, nullptr);
    ASSERT_TRUE(Idle->UsbIdle);
    EXPECT_TRUE(Idle->UsbIdle->BusReceivedAt100ns);
    EXPECT_FALSE(Idle->UsbIdle->CallbackEnteredAt100ns);
    EXPECT_FALSE(Idle->UsbIdle->D2IRP);
    EXPECT_EQ(Idle->UsbIdle->CompletionCause,
              DriverUsbIdleCompletionCause::Cancel);
    EXPECT_EQ(Idle->IOStatus, windows::StatusCancelled);
  }
}

TEST(DriverKMDFUsbIdle, PermissionWaitsForD2AndManagedReadWaitsForDelayedD0) {
  for (const auto *Image : images())
    for (uint64_t Base : {uint64_t(0), uint64_t(0x190000000)}) {
      auto Input = options(false, true);
      Input.LoadAddress = Base;
      observe(Input, Function, DriverPowerPolicyAction::Idle);
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
      EXPECT_EQ(word(*Before, KmdfUsbTargetState),
                uint32_t(DevicePowerState::D2));
      EXPECT_EQ(word(*Before, KmdfUsbReadsDelivered), 0u);
      EXPECT_EQ(word(*Data, 0), KmdfUsbReadMarker);
      EXPECT_EQ(word(*After, KmdfUsbInD0), 1u);
      EXPECT_EQ(word(*After, KmdfUsbPreviousState),
                uint32_t(DevicePowerState::D2));
      EXPECT_EQ(word(*After, KmdfUsbD0Entries), 2u);
      EXPECT_EQ(word(*After, KmdfUsbReadsRouted), 1u);
      EXPECT_EQ(word(*After, KmdfUsbReadsDelivered), 1u);
      EXPECT_LT(word(*After, KmdfUsbReadRouteSequence),
                word(*After, KmdfUsbEntrySequence));
      EXPECT_LT(word(*After, KmdfUsbEntrySequence),
                word(*After, KmdfUsbReadDeliverySequence));
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
      EXPECT_EQ(Idle->UsbIdle->D2Status, windows::StatusSuccess);
      EXPECT_EQ(Idle->UsbIdle->D2CompletedAt100ns,
                Power[0]->Power->BusCompletedAt100ns);
      EXPECT_EQ(Idle->UsbIdle->CompletionCause,
                DriverUsbIdleCompletionCause::Cancel);
      ASSERT_TRUE(Idle->UsbIdle->CompletedAt100ns);
      ASSERT_TRUE(Power[1]->Power->BusReceivedAt100ns);
      EXPECT_LE(*Idle->UsbIdle->CompletedAt100ns,
                *Power[1]->Power->BusReceivedAt100ns);
      EXPECT_EQ(Idle->IOStatus, windows::StatusCancelled);
      ASSERT_TRUE(Idle->UsbIdle->CompletedAt100ns);
      ASSERT_TRUE(Power[1]->Power->BusCompletedAt100ns);
      EXPECT_LT(*Idle->UsbIdle->CompletedAt100ns,
                *Power[1]->Power->BusCompletedAt100ns);
      ASSERT_EQ(Result->PowerPolicyEvents.size(), 2u);
      ASSERT_EQ(Result->PowerPolicyEvents[1].UsbIdleMembers.size(), 1u);
      EXPECT_EQ(Result->PowerPolicyEvents[1].UsbIdleMembers[0].IRP, Idle->IRP);
    }
}

TEST(DriverKMDFUsbIdle, ExplicitRemoteWakeCompletesWaitWakeBeforeDelayedD0) {
  for (const auto *Image : images()) {
    auto Input = options(true);
    observe(Input, Function, DriverPowerPolicyAction::Idle);
    observe(Input, Function, DriverPowerPolicyAction::UsbIdlePermission);
    const auto Suspended = observe(Input);
    observe(Input, Function, DriverPowerPolicyAction::Wake);
    const auto Resumed = observe(Input);
    remove(Input);
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    const auto *Before = scenario(*Result, Suspended);
    const auto *After = scenario(*Result, Resumed);
    ASSERT_NE(Before, nullptr);
    ASSERT_NE(After, nullptr);
    EXPECT_EQ(word(*Before, KmdfUsbInD0), 0u);
    EXPECT_EQ(word(*Before, KmdfUsbArmed), 1u);
    EXPECT_EQ(word(*After, KmdfUsbInD0), 1u);
    EXPECT_EQ(word(*After, KmdfUsbArms), 1u);
    EXPECT_EQ(word(*After, KmdfUsbTriggers), 1u);
    EXPECT_EQ(word(*After, KmdfUsbDisarms), 1u);
    EXPECT_EQ(word(*After, KmdfUsbArmed), 0u);
    const auto *Idle = idle(*Result);
    ASSERT_NE(Idle, nullptr);
    ASSERT_TRUE(Idle->UsbIdle);
    EXPECT_EQ(Idle->IOStatus, windows::StatusSuccess);
    EXPECT_EQ(Idle->UsbIdle->CompletionCause,
              DriverUsbIdleCompletionCause::DeviceD0);
    const auto Wake = std::find_if(
        Result->Requests.begin(), Result->Requests.end(),
        [](const auto &Request) {
          return Request.Origin == DriverRequestOrigin::FrameworkWaitWake;
        });
    ASSERT_NE(Wake, Result->Requests.end());
    ASSERT_TRUE(Wake->Power);
    EXPECT_EQ(Wake->IOStatus, windows::StatusSuccess);
    EXPECT_EQ(Wake->Power->DeviceStateAfter, DevicePowerState::D2);
    EXPECT_EQ(Wake->Power->WakeSourceDeviceID, Function.str());
    const auto Power = powers(*Result);
    ASSERT_EQ(Power.size(), 2u);
    EXPECT_EQ(Idle->UsbIdle->CompletedAt100ns,
              Power[1]->Power->BusReceivedAt100ns);
    ASSERT_TRUE(Idle->UsbIdle->CompletedAt100ns);
    ASSERT_TRUE(Power[1]->Power->BusCompletedAt100ns);
    EXPECT_LT(*Idle->UsbIdle->CompletedAt100ns,
              *Power[1]->Power->BusCompletedAt100ns);
  }
}

TEST(DriverKMDFUsbIdle, StopIdleBeforePermissionCancelsTheRetainedPacket) {
  for (const auto *Image : images()) {
    auto Input = options();
    observe(Input, Function, DriverPowerPolicyAction::Idle);
    Input.Requests.push_back(
        file(Function, DriverRequestKind::DeviceControl, KmdfUsbStopIdleIoctl));
    const auto Snapshot = observe(Input);
    Input.Requests.push_back(file(Function, DriverRequestKind::DeviceControl,
                                  KmdfUsbResumeIdleIoctl));
    remove(Input);
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    const auto *Idle = idle(*Result);
    const auto *State = scenario(*Result, Snapshot);
    ASSERT_NE(Idle, nullptr);
    ASSERT_NE(State, nullptr);
    ASSERT_TRUE(Idle->UsbIdle);
    EXPECT_EQ(Idle->UsbIdle->CompletionCause,
              DriverUsbIdleCompletionCause::Cancel);
    EXPECT_FALSE(Idle->UsbIdle->CallbackEnteredAt100ns);
    EXPECT_TRUE(powers(*Result).empty());
    EXPECT_EQ(word(*State, KmdfUsbStopCalls), 1u);
    EXPECT_EQ(word(*State, KmdfUsbInD0), 1u);
  }
}

TEST(DriverKMDFUsbIdle, FailedWakeArmDoesNotConsumeTheD2Response) {
  for (const auto *Image : images()) {
    auto Input = options(true, false, true);
    observe(Input, Function, DriverPowerPolicyAction::Idle);
    observe(Input, Function, DriverPowerPolicyAction::UsbIdlePermission);
    const auto Snapshot = observe(Input);
    remove(Input);
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    const auto *Idle = idle(*Result);
    const auto *State = scenario(*Result, Snapshot);
    ASSERT_NE(Idle, nullptr);
    ASSERT_NE(State, nullptr);
    ASSERT_TRUE(Idle->UsbIdle);
    EXPECT_TRUE(Idle->UsbIdle->CallbackEnteredAt100ns);
    EXPECT_TRUE(Idle->UsbIdle->CallbackReturnedAt100ns);
    EXPECT_FALSE(Idle->UsbIdle->D2IRP);
    EXPECT_TRUE(powers(*Result).empty());
    EXPECT_EQ(word(*State, KmdfUsbArms), 1u);
    EXPECT_EQ(word(*State, KmdfUsbD0Exits), 0u);
    EXPECT_EQ(word(*State, KmdfUsbInD0), 1u);
  }
}

TEST(DriverKMDFUsbIdle, StopWorkerDuringPermissionWaitsForCallbackRetirement) {
  for (const auto *Image : images()) {
    auto Input = options(true, false, false, true);
    observe(Input, Function, DriverPowerPolicyAction::Idle);
    observe(Input, Function, DriverPowerPolicyAction::UsbIdlePermission);
    const auto Snapshot = observe(Input);
    Input.Requests.push_back(file(Function, DriverRequestKind::DeviceControl,
                                  KmdfUsbResumeIdleIoctl));
    remove(Input);
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    const auto *Idle = idle(*Result);
    const auto *State = scenario(*Result, Snapshot);
    ASSERT_NE(Idle, nullptr);
    ASSERT_NE(State, nullptr);
    ASSERT_TRUE(Idle->UsbIdle);
    EXPECT_TRUE(Idle->UsbIdle->CallbackEnteredAt100ns);
    EXPECT_TRUE(Idle->UsbIdle->CallbackReturnedAt100ns);
    EXPECT_TRUE(Idle->UsbIdle->CompletedAt100ns);
    if (Idle->UsbIdle->CallbackReturnedAt100ns &&
        Idle->UsbIdle->CompletedAt100ns)
      EXPECT_LE(*Idle->UsbIdle->CallbackReturnedAt100ns,
                *Idle->UsbIdle->CompletedAt100ns);
    EXPECT_EQ(Idle->UsbIdle->CompletionCause,
              DriverUsbIdleCompletionCause::Cancel);
    EXPECT_EQ(word(*State, KmdfUsbStopCalls), 1u);
    EXPECT_EQ(word(*State, KmdfUsbInD0), 1u);
  }
}

TEST(DriverKMDFUsbIdle, MaximumCannotInventAnUnknownBusCapability) {
  auto Input = options(false, true);
  Input.PnpDevices[0].UsbIdle->DeviceWake.reset();
  auto Result = emulateDriver(NEVERD_KMDF_USB_IDLE_FIXTURE, Input);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_NE(Result->Stop, DriverStopReason::Returned);
  EXPECT_NE(Result->Diagnostic.find("DeviceWake"), std::string::npos);
  EXPECT_EQ(idle(*Result), nullptr);
  EXPECT_TRUE(powers(*Result).empty());
}

TEST(DriverKMDFUsbIdle, CompositePermissionCapturesEveryFrameworkFunction) {
  for (const auto *Image : images()) {
    DriverOptions Input;
    Input.ServiceName = "NeverDKmdfUsbComposite";
    Input.Unload = true;
    auto Parent = device("usb-parent");
    Parent.UsbIdle =
        DriverUsbIdleConfig{DriverUsbIdleRole::CompositeParent, false};
    Parent.RequestedDevicePower.clear();
    auto First = device(Function), Second = device("usb-sibling");
    for (auto *Child : {&First, &Second}) {
      Child->ParentID = Parent.ID;
      Child->UsbIdle->Role = DriverUsbIdleRole::CompositeFunction;
    }
    Input.PnpDevices = {Parent, First, Second};
    for (const auto *ID : {"usb-parent", "usb-function", "usb-sibling"}) {
      Input.Requests.push_back(pnp(ID, DevicePnpRequest::Start));
      Input.Requests.push_back(file(ID, DriverRequestKind::Create));
      if (llvm::StringRef(ID) != "usb-parent") {
        Input.Requests.push_back(configure(ID));
        observe(Input, ID, DriverPowerPolicyAction::Idle);
      }
    }
    observe(Input, Function, DriverPowerPolicyAction::UsbIdlePermission,
            "usb-parent");
    const auto FirstSnapshot = observe(Input);
    const auto SecondSnapshot = observe(Input, "usb-sibling");
    Input.Requests.push_back(file(Function, DriverRequestKind::Read));
    Input.Requests.push_back(file("usb-sibling", DriverRequestKind::Read));
    remove(Input, Function);
    remove(Input, "usb-sibling");
    remove(Input, "usb-parent");
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    ASSERT_EQ(Result->PowerPolicyEvents.size(), 3u);
    const auto &Members = Result->PowerPolicyEvents.back().UsbIdleMembers;
    ASSERT_EQ(Members.size(), 2u);
    for (const auto &Member : Members) {
      const auto *Idle = idle(*Result, Member.DeviceID);
      ASSERT_NE(Idle, nullptr);
      ASSERT_TRUE(Idle->UsbIdle);
      EXPECT_EQ(Member.IRP, Idle->IRP);
      EXPECT_EQ(Member.StartEpoch, Idle->UsbIdle->StartEpoch);
      EXPECT_TRUE(Idle->UsbIdle->D2IRP);
      EXPECT_EQ(powers(*Result, Member.DeviceID).size(), 2u);
    }
    const auto *FirstState = scenario(*Result, FirstSnapshot);
    const auto *SecondState = scenario(*Result, SecondSnapshot);
    ASSERT_NE(FirstState, nullptr);
    ASSERT_NE(SecondState, nullptr);
    EXPECT_EQ(word(*FirstState, KmdfUsbInD0), 0u);
    EXPECT_EQ(word(*SecondState, KmdfUsbInD0), 0u);
  }
}
#else
TEST(DriverKMDFUsbIdle, RequiresGenuineWdkFixture) {
  GTEST_SKIP() << "NEVERD_KMDF_USB_IDLE_FIXTURE is unavailable";
}
#endif
} // namespace
} // namespace neverd::emulation
