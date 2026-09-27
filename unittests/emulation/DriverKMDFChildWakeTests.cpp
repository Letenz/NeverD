//===- DriverKMDFChildWakeTests.cpp - Genuine KMDF parent wake -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercise independent parent/child policies using genuine WDK callbacks and
/// retained WAIT_WAKE packets, including cancellation and causal reporting.
///
//===----------------------------------------------------------------------===//
#include "fixtures/driver_kmdf_child_wake_test.h"
#include "gtest/gtest.h"
#include "windows/KernelFramework.h"
#include "windows/WindowsKernelLayout.h"

#include "neverd/emulation/DriverSession.h"

#include <algorithm>
#include <array>

namespace neverd::emulation {
namespace {
#ifdef NEVERD_KMDF_CHILD_WAKE_FIXTURE
constexpr std::array<llvm::StringLiteral, 4> DeviceIDs = {
    "wake-parent", "wake-child", "wake-disabled-sibling", "wake-unrelated"};
constexpr size_t Parent = 0, Child = 1, Disabled = 2, Unrelated = 3;
constexpr uint64_t ProviderDelay100ns = 3;

std::vector<const char *> images() {
  return {
      NEVERD_KMDF_CHILD_WAKE_FIXTURE,
#ifdef NEVERD_KMDF_CHILD_WAKE_CFG_FIXTURE
      NEVERD_KMDF_CHILD_WAKE_CFG_FIXTURE,
#endif
  };
}

DriverRequest pnp(size_t Device, DevicePnpRequest Minor) {
  DriverRequest Request;
  Request.Kind = DriverRequestKind::Pnp;
  Request.DeviceID = DeviceIDs[Device].str();
  Request.Pnp = DriverPnpOperation{Minor, {windows::StatusSuccess, 0}};
  return Request;
}

DriverRequest file(size_t Device, DriverRequestKind Kind) {
  DriverRequest Request;
  Request.Kind = Kind;
  Request.DeviceID = DeviceIDs[Device].str();
  Request.File = uint32_t(Device + 1);
  if (Kind == DriverRequestKind::DeviceControl) {
    Request.ControlCode = KmdfChildWakeSnapshotIoctl;
    Request.OutputSize = KmdfChildWakeSnapshotWords * sizeof(uint32_t);
  }
  return Request;
}

DriverRequest configure(size_t Device, bool Own, bool Children, bool Propagate,
                        bool FailArm = false) {
  auto Request = file(Device, DriverRequestKind::DeviceControl);
  Request.ControlCode = KmdfChildWakeConfigureIoctl;
  Request.OutputSize = 0;
  Request.Input = {uint8_t(Device), uint8_t(Own), uint8_t(Children),
                   uint8_t(Propagate), uint8_t(FailArm)};
  return Request;
}

DriverPowerOperation devicePower(DevicePowerState State) {
  DriverPowerOperation Operation;
  Operation.State = uint32_t(State);
  Operation.Action = State == DevicePowerState::D3 ? DriverPowerAction::Sleep
                                                   : DriverPowerAction::None;
  Operation.BusCompletion = {windows::StatusSuccess, ProviderDelay100ns};
  return Operation;
}

DriverRequest systemPower(size_t Device, SystemPowerState State) {
  DriverRequest Request;
  Request.Kind = DriverRequestKind::Power;
  Request.DeviceID = DeviceIDs[Device].str();
  auto Operation =
      devicePower(State == SystemPowerState::Working ? DevicePowerState::D0
                                                     : DevicePowerState::D3);
  Operation.Type = DriverPowerType::System;
  Operation.State = uint32_t(State);
  Request.Power = Operation;
  return Request;
}

DriverOptions options(bool Own = false, bool Children = true,
                      bool Propagate = true, bool ChildEnabled = true,
                      bool ChildFails = false) {
  DriverOptions Options;
  Options.ServiceName = "NeverDKmdfChildWake";
  Options.Unload = true;
  for (size_t I = 0; I < DeviceIDs.size(); ++I) {
    DriverPnpDevice Device;
    Device.ID = DeviceIDs[I].str();
    if (I == Child || I == Disabled)
      Device.ParentID = DeviceIDs[Parent].str();
    Device.InitialDevicePower = DevicePowerState::D0;
    Device.InitialSystemPower = SystemPowerState::Working;
    Device.WakeCapabilities = DriverWakeCapabilities{false, true};
    Device.RequestedDevicePower = {devicePower(DevicePowerState::D3),
                                   devicePower(DevicePowerState::D0)};
    Options.PnpDevices.push_back(std::move(Device));
    Options.Requests.push_back(pnp(I, DevicePnpRequest::Start));
    Options.Requests.push_back(file(I, DriverRequestKind::Create));
    Options.Requests.push_back(configure(I,
                                         I == Parent  ? Own
                                         : I == Child ? ChildEnabled
                                                      : I == Unrelated,
                                         I == Parent && Children,
                                         I == Parent && Propagate,
                                         I == Child && ChildFails));
  }
  return Options;
}

void sleepDevices(DriverOptions &Options) {
  for (size_t I : {Child, Disabled, Unrelated, Parent})
    Options.Requests.push_back(systemPower(I, SystemPowerState::Sleeping3));
}

size_t snapshot(DriverOptions &Options, size_t Device, bool Wake = false) {
  auto Request = file(Device, DriverRequestKind::DeviceControl);
  if (Wake)
    Request.PowerPolicyEvents = {
        {0, DeviceIDs[Device].str(), DriverPowerPolicyAction::Wake}};
  Options.Requests.push_back(std::move(Request));
  return Options.Requests.size() - 1;
}

void resumeDevices(DriverOptions &Options) {
  for (size_t I : {Parent, Child, Disabled, Unrelated})
    Options.Requests.push_back(systemPower(I, SystemPowerState::Working));
}

void removeDevices(DriverOptions &Options) {
  for (size_t I : {Child, Disabled, Unrelated, Parent}) {
    Options.Requests.push_back(file(I, DriverRequestKind::Cleanup));
    Options.Requests.push_back(file(I, DriverRequestKind::Close));
    Options.Requests.push_back(pnp(I, DevicePnpRequest::QueryRemove));
    Options.Requests.push_back(pnp(I, DevicePnpRequest::Remove));
  }
}

const DriverRequestResult *scenarioRequest(const DriverResult &Result,
                                           size_t Expected) {
  size_t Index = 0;
  for (const auto &Request : Result.Requests)
    if (Request.Origin == DriverRequestOrigin::Scenario && Index++ == Expected)
      return &Request;
  return nullptr;
}

const DriverRequestResult *waitWake(const DriverResult &Result, size_t Device,
                                    size_t Epoch = 0) {
  for (const auto &Request : Result.Requests)
    if (Request.Origin == DriverRequestOrigin::FrameworkWaitWake &&
        Request.DeviceID == DeviceIDs[Device] && Epoch-- == 0)
      return &Request;
  return nullptr;
}

uint32_t word(const DriverRequestResult &Request, size_t Index) {
  uint32_t Value = 0;
  if (Request.Output.size() >= (Index + 1) * sizeof(Value))
    for (size_t I = 0; I < sizeof(Value); ++I)
      Value |= uint32_t(Request.Output[Index * sizeof(Value) + I]) << (I * 8);
  return Value;
}

void expectClean(const DriverResult &Result) {
  EXPECT_EQ(Result.Stop, DriverStopReason::Returned) << Result.Diagnostic;
  EXPECT_TRUE(Result.UnloadCompleted);
  EXPECT_NE(std::find(Result.Messages.begin(), Result.Messages.end(),
                      "KMDF child wake: unload live 0 failures 0\n"),
            Result.Messages.end());
  for (const auto &Message : Result.Messages)
    EXPECT_FALSE(
        llvm::StringRef(Message).starts_with("KMDF child wake: invalid"))
        << Message;
  for (const auto &Request : Result.Requests)
    if (Request.Origin == DriverRequestOrigin::Scenario)
      EXPECT_EQ(Request.IOStatus, windows::StatusSuccess) << Request.DeviceID;
}

void expectSnapshot(const DriverResult &Result, size_t Index, size_t Device,
                    uint32_t Arms, uint32_t Triggers, uint32_t Disarms,
                    bool InD0, bool Armed) {
  const auto *Request = scenarioRequest(Result, Index);
  ASSERT_NE(Request, nullptr);
  ASSERT_EQ(Request->Output.size(),
            KmdfChildWakeSnapshotWords * sizeof(uint32_t));
  EXPECT_EQ(word(*Request, KmdfChildWakeFailures), 0u);
  EXPECT_EQ(word(*Request, KmdfChildWakeDeviceIdentity), Device);
  EXPECT_EQ(word(*Request, KmdfChildWakeArms), Arms);
  EXPECT_EQ(word(*Request, KmdfChildWakeTriggers), Triggers);
  EXPECT_EQ(word(*Request, KmdfChildWakeDisarms), Disarms);
  EXPECT_EQ(word(*Request, KmdfChildWakeInD0), uint32_t(InD0));
  EXPECT_EQ(word(*Request, KmdfChildWakeArmed), uint32_t(Armed));
}

void expectWakeSource(const DriverResult &Result, size_t Device,
                      std::optional<size_t> Source, size_t Epoch = 0) {
  const auto *Wake = waitWake(Result, Device, Epoch);
  ASSERT_NE(Wake, nullptr);
  EXPECT_TRUE(Wake->Completed);
  ASSERT_TRUE(Wake->Power);
  EXPECT_EQ(Wake->IOStatus,
            Source ? windows::StatusSuccess : framework::RequestCancelled);
  if (!Source) {
    EXPECT_FALSE(Wake->Power->WakeSourceDeviceID);
    EXPECT_FALSE(Wake->Power->WakeSourcePDO);
    return;
  }
  EXPECT_EQ(Wake->Power->WakeSourceDeviceID, DeviceIDs[*Source].str());
  ASSERT_LT(*Source, Result.PnpDevices.size());
  EXPECT_EQ(Wake->Power->WakeSourcePDO, Result.PnpDevices[*Source].PDO);
}
#endif

TEST(DriverKMDFChildWake, ReportsIndependentOwnAndChildReasons) {
#ifdef NEVERD_KMDF_CHILD_WAKE_FIXTURE
  for (const auto *Image : images())
    for (bool Own : {false, true})
      for (uint64_t Base : {uint64_t(0), uint64_t(0x190000000)}) {
        SCOPED_TRACE(Image);
        SCOPED_TRACE(Own);
        SCOPED_TRACE(Base);
        auto Input = options(Own);
        Input.LoadAddress = Base;
        sleepDevices(Input);
        const size_t Asleep = snapshot(Input, Parent, true);
        resumeDevices(Input);
        const size_t ParentAwake = snapshot(Input, Parent);
        const size_t ChildAwake = snapshot(Input, Child);
        const size_t DisabledAwake = snapshot(Input, Disabled);
        const size_t UnrelatedAwake = snapshot(Input, Unrelated);
        removeDevices(Input);
        auto Result = emulateDriver(Image, Input);
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        ASSERT_EQ(Result->Stop, DriverStopReason::Returned)
            << Result->Diagnostic;
        expectClean(*Result);
        expectSnapshot(*Result, Asleep, Parent, 1, 0, 0, false, true);
        const auto *Reason = scenarioRequest(*Result, Asleep);
        ASSERT_NE(Reason, nullptr);
        EXPECT_EQ(word(*Reason, KmdfChildWakeOwnReason), uint32_t(Own));
        EXPECT_EQ(word(*Reason, KmdfChildWakeChildrenReason), 1u);
        expectSnapshot(*Result, ParentAwake, Parent, 1, 1, 1, true, false);
        expectSnapshot(*Result, ChildAwake, Child, 1, 1, 1, true, false);
        expectSnapshot(*Result, DisabledAwake, Disabled, 0, 0, 0, true, false);
        expectSnapshot(*Result, UnrelatedAwake, Unrelated, 1, 0, 1, true,
                       false);
        expectWakeSource(*Result, Parent, Parent);
        expectWakeSource(*Result, Child, Parent);
        expectWakeSource(*Result, Unrelated, std::nullopt);
        EXPECT_EQ(waitWake(*Result, Disabled), nullptr);
      }
#else
  GTEST_SKIP()
      << "NEVERD_KMDF_CHILD_WAKE_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverKMDFChildWake, OwnWakeDoesNotRequireArmedChildren) {
#ifdef NEVERD_KMDF_CHILD_WAKE_FIXTURE
  for (const auto *Image : images()) {
    auto Input = options(true, true, true, false);
    sleepDevices(Input);
    const size_t Asleep = snapshot(Input, Parent, true);
    resumeDevices(Input);
    const size_t Awake = snapshot(Input, Parent);
    removeDevices(Input);
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    expectClean(*Result);
    const auto *Reason = scenarioRequest(*Result, Asleep);
    ASSERT_NE(Reason, nullptr);
    EXPECT_EQ(word(*Reason, KmdfChildWakeOwnReason), 1u);
    EXPECT_EQ(word(*Reason, KmdfChildWakeChildrenReason), 0u);
    expectSnapshot(*Result, Awake, Parent, 1, 1, 1, true, false);
    expectWakeSource(*Result, Parent, Parent);
    EXPECT_EQ(waitWake(*Result, Child), nullptr);
  }
#else
  GTEST_SKIP()
      << "NEVERD_KMDF_CHILD_WAKE_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverKMDFChildWake, DisabledPropagationLeavesChildWakePending) {
#ifdef NEVERD_KMDF_CHILD_WAKE_FIXTURE
  for (const auto *Image : images()) {
    auto Input = options(false, true, false);
    sleepDevices(Input);
    snapshot(Input, Parent, true);
    Input.Requests.push_back(systemPower(Parent, SystemPowerState::Working));
    const size_t ChildSleeping = snapshot(Input, Child);
    for (size_t I : {Child, Disabled, Unrelated})
      Input.Requests.push_back(systemPower(I, SystemPowerState::Working));
    const size_t ChildAwake = snapshot(Input, Child);
    removeDevices(Input);
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    expectClean(*Result);
    expectSnapshot(*Result, ChildSleeping, Child, 1, 0, 0, false, true);
    expectSnapshot(*Result, ChildAwake, Child, 1, 0, 1, true, false);
    expectWakeSource(*Result, Parent, Parent);
    expectWakeSource(*Result, Child, std::nullopt);
  }
#else
  GTEST_SKIP()
      << "NEVERD_KMDF_CHILD_WAKE_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverKMDFChildWake, FailedChildArmDoesNotBecomeParentWakeReason) {
#ifdef NEVERD_KMDF_CHILD_WAKE_FIXTURE
  for (const auto *Image : images()) {
    auto Input = options(true, true, true, true, true);
    sleepDevices(Input);
    const size_t Asleep = snapshot(Input, Parent, true);
    resumeDevices(Input);
    const size_t ChildAwake = snapshot(Input, Child);
    removeDevices(Input);
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    expectClean(*Result);
    const auto *Reason = scenarioRequest(*Result, Asleep);
    ASSERT_NE(Reason, nullptr);
    EXPECT_EQ(word(*Reason, KmdfChildWakeOwnReason), 1u);
    EXPECT_EQ(word(*Reason, KmdfChildWakeChildrenReason), 0u);
    expectSnapshot(*Result, ChildAwake, Child, 1, 0, 1, true, false);
    expectWakeSource(*Result, Child, std::nullopt);
  }
#else
  GTEST_SKIP()
      << "NEVERD_KMDF_CHILD_WAKE_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverKMDFChildWake, LastChildCancellationDisarmsOnlyChildOwnedParent) {
#ifdef NEVERD_KMDF_CHILD_WAKE_FIXTURE
  for (const auto *Image : images())
    for (bool Own : {false, true}) {
      SCOPED_TRACE(Own);
      auto Input = options(Own);
      sleepDevices(Input);
      Input.Requests.push_back(systemPower(Child, SystemPowerState::Working));
      const size_t ParentSleeping = snapshot(Input, Parent);
      for (size_t I : {Parent, Disabled, Unrelated})
        Input.Requests.push_back(systemPower(I, SystemPowerState::Working));
      const size_t ParentAwake = snapshot(Input, Parent);
      removeDevices(Input);
      auto Result = emulateDriver(Image, Input);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
      expectClean(*Result);
      expectSnapshot(*Result, ParentSleeping, Parent, 1, 0, Own ? 0 : 1, false,
                     Own);
      expectSnapshot(*Result, ParentAwake, Parent, 1, 0, 1, true, false);
      expectWakeSource(*Result, Parent, std::nullopt);
      expectWakeSource(*Result, Child, std::nullopt);
    }
#else
  GTEST_SKIP()
      << "NEVERD_KMDF_CHILD_WAKE_FIXTURE requires a genuine WDK fixture";
#endif
}
TEST(DriverKMDFChildWake, PropagationRequiresOptInAtEveryParent) {
#ifdef NEVERD_KMDF_CHILD_WAKE_FIXTURE
  for (const auto *Image : images())
    for (bool RelayPropagates : {false, true}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(RelayPropagates);
      auto Input = options();
      Input.PnpDevices[Disabled].ParentID = DeviceIDs[Child].str();
      Input.Requests[Child * 3 + 2] =
          configure(Child, false, true, RelayPropagates);
      Input.Requests[Disabled * 3 + 2] =
          configure(Disabled, true, false, false);
      for (size_t I : {Disabled, Child, Unrelated, Parent})
        Input.Requests.push_back(systemPower(I, SystemPowerState::Sleeping3));
      snapshot(Input, Parent, true);
      resumeDevices(Input);
      const size_t RelayAwake = snapshot(Input, Child);
      const size_t LeafAwake = snapshot(Input, Disabled);
      // The relay cannot retire before its own child provider.
      for (size_t I : {Disabled, Child, Unrelated, Parent}) {
        Input.Requests.push_back(file(I, DriverRequestKind::Cleanup));
        Input.Requests.push_back(file(I, DriverRequestKind::Close));
        Input.Requests.push_back(pnp(I, DevicePnpRequest::QueryRemove));
        Input.Requests.push_back(pnp(I, DevicePnpRequest::Remove));
      }
      auto Result = emulateDriver(Image, Input);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
      expectClean(*Result);
      expectSnapshot(*Result, RelayAwake, Child, 1, 1, 1, true, false);
      expectSnapshot(*Result, LeafAwake, Disabled, 1, RelayPropagates ? 1 : 0,
                     1, true, false);
      expectWakeSource(*Result, Parent, Parent);
      expectWakeSource(*Result, Child, Parent);
      expectWakeSource(*Result, Disabled,
                       RelayPropagates ? std::optional<size_t>{Parent}
                                       : std::nullopt);
    }
#else
  GTEST_SKIP()
      << "NEVERD_KMDF_CHILD_WAKE_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverKMDFChildWake, RestartDoesNotReuseCancelledWakeObligations) {
#ifdef NEVERD_KMDF_CHILD_WAKE_FIXTURE
  for (const auto *Image : images()) {
    auto Input = options();
    Input.PnpDevices.resize(2);
    Input.Requests.resize(6);
    for (auto &Device : Input.PnpDevices) {
      Device.RequestedDevicePower.push_back(devicePower(DevicePowerState::D3));
      Device.RequestedDevicePower.push_back(devicePower(DevicePowerState::D0));
    }
    for (size_t I : {Child, Parent})
      Input.Requests.push_back(systemPower(I, SystemPowerState::Sleeping3));
    for (size_t I : {Parent, Child})
      Input.Requests.push_back(systemPower(I, SystemPowerState::Working));
    for (size_t I : {Child, Parent}) {
      Input.Requests.push_back(file(I, DriverRequestKind::Cleanup));
      Input.Requests.push_back(file(I, DriverRequestKind::Close));
      Input.Requests.push_back(pnp(I, DevicePnpRequest::QueryStop));
      Input.Requests.push_back(pnp(I, DevicePnpRequest::Stop));
    }
    for (size_t I : {Parent, Child}) {
      Input.Requests.push_back(pnp(I, DevicePnpRequest::Start));
      Input.Requests.push_back(file(I, DriverRequestKind::Create));
    }
    for (size_t I : {Child, Parent})
      Input.Requests.push_back(systemPower(I, SystemPowerState::Sleeping3));
    snapshot(Input, Parent, true);
    for (size_t I : {Parent, Child})
      Input.Requests.push_back(systemPower(I, SystemPowerState::Working));
    const size_t ParentAwake = snapshot(Input, Parent);
    const size_t ChildAwake = snapshot(Input, Child);
    for (size_t I : {Child, Parent}) {
      Input.Requests.push_back(file(I, DriverRequestKind::Cleanup));
      Input.Requests.push_back(file(I, DriverRequestKind::Close));
      Input.Requests.push_back(pnp(I, DevicePnpRequest::QueryRemove));
      Input.Requests.push_back(pnp(I, DevicePnpRequest::Remove));
    }
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    expectClean(*Result);
    expectSnapshot(*Result, ParentAwake, Parent, 2, 1, 2, true, false);
    expectSnapshot(*Result, ChildAwake, Child, 2, 1, 2, true, false);
    expectWakeSource(*Result, Parent, std::nullopt, 0);
    expectWakeSource(*Result, Child, std::nullopt, 0);
    expectWakeSource(*Result, Parent, Parent, 1);
    expectWakeSource(*Result, Child, Parent, 1);
  }
#else
  GTEST_SKIP()
      << "NEVERD_KMDF_CHILD_WAKE_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverKMDFChildWake, FailedParentArmNeverInventsChildWakeSuccess) {
#ifdef NEVERD_KMDF_CHILD_WAKE_FIXTURE
  for (const auto *Image : images()) {
    auto Input = options();
    Input.Requests[2] = configure(Parent, false, true, true, true);
    sleepDevices(Input);
    const size_t ParentSleeping = snapshot(Input, Parent);
    const size_t ChildSleeping = snapshot(Input, Child);
    resumeDevices(Input);
    const size_t ChildAwake = snapshot(Input, Child);
    removeDevices(Input);
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    expectClean(*Result);
    expectSnapshot(*Result, ParentSleeping, Parent, 1, 0, 1, false, false);
    expectSnapshot(*Result, ChildSleeping, Child, 1, 0, 0, false, true);
    expectSnapshot(*Result, ChildAwake, Child, 1, 0, 1, true, false);
    expectWakeSource(*Result, Parent, std::nullopt);
    expectWakeSource(*Result, Child, std::nullopt);
  }
#else
  GTEST_SKIP()
      << "NEVERD_KMDF_CHILD_WAKE_FIXTURE requires a genuine WDK fixture";
#endif
}

} // namespace
} // namespace neverd::emulation
