//===- DriverWdmWaitWakeTests.cpp - Genuine native WAIT_WAKE ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "fixtures/driver_wdm_wait_wake_test.h"
#include "gtest/gtest.h"
#include "windows/WindowsKernelLayout.h"

#include "neverd/emulation/DriverSession.h"

#include <algorithm>
#include <array>

namespace neverd::emulation {
namespace {
#ifdef NEVERD_WDM_WAIT_WAKE_FIXTURE
constexpr llvm::StringLiteral DeviceID = "native-wake";

std::vector<const char *> images() {
  return {
      NEVERD_WDM_WAIT_WAKE_FIXTURE,
#ifdef NEVERD_WDM_WAIT_WAKE_CFG_FIXTURE
      NEVERD_WDM_WAIT_WAKE_CFG_FIXTURE,
#endif
  };
}

DriverRequest pnp(DevicePnpRequest Minor) {
  DriverRequest Request;
  Request.Kind = DriverRequestKind::Pnp;
  Request.DeviceID = DeviceID.str();
  Request.Pnp = DriverPnpOperation{Minor, {windows::StatusSuccess, 3}};
  return Request;
}

DriverRequest file(DriverRequestKind Kind) {
  DriverRequest Request;
  Request.Kind = Kind;
  Request.DeviceID = DeviceID.str();
  Request.File = 1;
  if (Kind == DriverRequestKind::DeviceControl) {
    Request.ControlCode = WdmWakeSnapshotIoctl;
    Request.OutputSize = WdmWakeSnapshotWords * sizeof(uint32_t);
  }
  return Request;
}

DriverRequest command(uint8_t Command) {
  auto Request = file(DriverRequestKind::DeviceControl);
  Request.ControlCode = WdmWakeCommandIoctl;
  Request.Input = {Command};
  Request.OutputSize = 0;
  return Request;
}

DriverRequest snapshot(bool Wake = false) {
  auto Request = file(DriverRequestKind::DeviceControl);
  if (Wake)
    Request.PowerPolicyEvents = {
        {0, DeviceID.str(), DriverPowerPolicyAction::Wake}};
  return Request;
}

DriverPowerOperation operation(DevicePowerState State) {
  DriverPowerOperation Power;
  Power.State = uint32_t(State);
  Power.BusCompletion = {windows::StatusSuccess, 7};
  return Power;
}

DriverRequest power(DevicePowerState State) {
  DriverRequest Request;
  Request.Kind = DriverRequestKind::Power;
  Request.DeviceID = DeviceID.str();
  Request.Power = operation(State);
  return Request;
}

DriverRequest systemPower(SystemPowerState State) {
  auto Request = power(DevicePowerState::D0);
  Request.Power->Type = DriverPowerType::System;
  Request.Power->State = uint32_t(State);
  Request.Power->Action = State == SystemPowerState::Working
                              ? DriverPowerAction::None
                              : DriverPowerAction::Sleep;
  return Request;
}

DriverOptions options(uint8_t Mode = WdmWakeModeWorking) {
  DriverOptions Options;
  Options.ServiceName = std::string("NeverDWdmWaitWake") + char(Mode);
  Options.Unload = true;
  DriverPnpDevice Device;
  Device.ID = DeviceID.str();
  Device.InitialDevicePower = DevicePowerState::D0;
  Device.InitialSystemPower = SystemPowerState::Working;
  Device.WakeCapabilities = DriverWakeCapabilities{true, true};
  Options.PnpDevices.push_back(std::move(Device));
  Options.Requests = {pnp(DevicePnpRequest::Start),
                      file(DriverRequestKind::Create)};
  return Options;
}

void finish(DriverOptions &Options) {
  Options.Requests.push_back(file(DriverRequestKind::Cleanup));
  Options.Requests.push_back(file(DriverRequestKind::Close));
  Options.Requests.push_back(pnp(DevicePnpRequest::QueryRemove));
  Options.Requests.push_back(pnp(DevicePnpRequest::Remove));
}

uint32_t word(const DriverRequestResult &Request, size_t Index) {
  uint32_t Value = 0;
  if (Request.Output.size() >= (Index + 1) * sizeof(Value))
    for (size_t I = 0; I < sizeof(Value); ++I)
      Value |= uint32_t(Request.Output[Index * sizeof(Value) + I]) << (I * 8);
  return Value;
}

const DriverRequestResult *scenario(const DriverResult &Result, size_t Index) {
  size_t Found = 0;
  for (const auto &Request : Result.Requests)
    if (Request.Origin == DriverRequestOrigin::Scenario && Found++ == Index)
      return &Request;
  return nullptr;
}

std::vector<const DriverRequestResult *> wakes(const DriverResult &Result) {
  std::vector<const DriverRequestResult *> Requests;
  for (const auto &Request : Result.Requests)
    if (Request.Origin == DriverRequestOrigin::PoRequestPowerIrp &&
        Request.Power && Request.Power->Minor == DevicePowerRequest::WaitWake)
      Requests.push_back(&Request);
  return Requests;
}

void clean(const DriverResult &Result) {
  ASSERT_EQ(Result.Stop, DriverStopReason::Returned) << Result.Diagnostic;
  EXPECT_TRUE(Result.UnloadCompleted);
  EXPECT_TRUE(Result.Devices.empty());
  EXPECT_NE(std::find(Result.Messages.begin(), Result.Messages.end(),
                      "WDM wait wake: unload live 0 failures 0\n"),
            Result.Messages.end());
  for (const auto &Request : Result.Requests) {
    EXPECT_TRUE(Request.Completed);
    if (!Request.CancelRequestedAt100ns)
      EXPECT_EQ(Request.IOStatus, windows::StatusSuccess);
  }
}

void expectSnapshot(const DriverResult &Result, size_t Index, unsigned Sends,
                    unsigned Completions, unsigned Callbacks, bool Active) {
  const auto *Snapshot = scenario(Result, Index);
  ASSERT_NE(Snapshot, nullptr);
  ASSERT_EQ(Snapshot->Output.size(), WdmWakeSnapshotWords * sizeof(uint32_t));
  EXPECT_EQ(word(*Snapshot, WdmWakeFailures), 0u);
  EXPECT_EQ(word(*Snapshot, WdmWakeSubmissions), Sends);
  EXPECT_EQ(word(*Snapshot, WdmWakeDispatches), Sends);
  EXPECT_EQ(word(*Snapshot, WdmWakeIoCompletions), Completions);
  EXPECT_EQ(word(*Snapshot, WdmWakeCallbacks), Callbacks);
  EXPECT_EQ(word(*Snapshot, WdmWakeActive), unsigned(Active));
  EXPECT_EQ(word(*Snapshot, WdmWakeHasCancelRoutine), unsigned(Active));
}
#endif

TEST(DriverWdmWaitWake, StartRetainsRealPacketAndWakeDoesNotPowerUpD2) {
#ifdef NEVERD_WDM_WAIT_WAKE_FIXTURE
  for (const auto *Image : images())
    for (uint8_t Mode : {WdmWakeModeWorking, WdmWakeModeSleeping})
      for (uint64_t Base : {uint64_t(0), uint64_t(0x190000000)}) {
        SCOPED_TRACE(Image);
        SCOPED_TRACE(char(Mode));
        SCOPED_TRACE(Base);
        auto Input = options(Mode);
        Input.LoadAddress = Base;
        Input.Requests.insert(Input.Requests.end(),
                              {snapshot(), power(DevicePowerState::D2)});
        if (Mode == WdmWakeModeSleeping)
          Input.Requests.push_back(systemPower(SystemPowerState::Sleeping3));
        Input.Requests.push_back(snapshot(true));
        const size_t AwakeIndex = Input.Requests.size();
        Input.Requests.push_back(snapshot());
        if (Mode == WdmWakeModeSleeping)
          Input.Requests.push_back(systemPower(SystemPowerState::Working));
        finish(Input);
        auto Result = emulateDriver(Image, Input);
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        clean(*Result);
        ASSERT_FALSE(HasFatalFailure());
        expectSnapshot(*Result, 2, 1, 0, 0, true);
        expectSnapshot(*Result, AwakeIndex, 1, 1, 1, false);
        const auto Wake = wakes(*Result);
        ASSERT_EQ(Wake.size(), 1u);
        EXPECT_FALSE(Wake.front()->ResponseIndex);
        EXPECT_EQ(Wake.front()->DispatchStatus, windows::StatusPending);
        EXPECT_FALSE(Wake.front()->CancelRequestedAt100ns);
        const auto &Power = *Wake.front()->Power;
        EXPECT_EQ(Power.Type, DriverPowerType::System);
        EXPECT_EQ(Power.State, uint32_t(Mode == WdmWakeModeSleeping
                                            ? SystemPowerState::Sleeping3
                                            : SystemPowerState::Working));
        EXPECT_EQ(Power.DeviceStateBefore, DevicePowerState::D0);
        EXPECT_EQ(Power.DeviceStateAfter, DevicePowerState::D2);
        EXPECT_EQ(Power.SystemStateAfter, Mode == WdmWakeModeSleeping
                                              ? SystemPowerState::Sleeping3
                                              : SystemPowerState::Working);
        EXPECT_EQ(Power.WakeSourceDeviceID, DeviceID.str());
        ASSERT_TRUE(Power.BusReceivedAt100ns);
        ASSERT_TRUE(Power.BusCompletedAt100ns);
        EXPECT_LT(*Power.BusReceivedAt100ns, *Power.BusCompletedAt100ns);
        const auto *Before = scenario(*Result, 2);
        ASSERT_NE(Before, nullptr);
        EXPECT_EQ(uint64_t(word(*Before, WdmWakeIRPLow)) |
                      uint64_t(word(*Before, WdmWakeIRPHigh)) << 32,
                  Wake.front()->IRP);
        EXPECT_EQ(word(*Before, WdmWakePublishedOutputs), 1u);
        EXPECT_EQ(Result->Requests.size(), Input.Requests.size() + 1);
      }
#else
  GTEST_SKIP() << "NEVERD_WDM_WAIT_WAKE_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverWdmWaitWake,
     OptionalCallbackOutputAndOriginalDeviceStayIndependent) {
#ifdef NEVERD_WDM_WAIT_WAKE_FIXTURE
  for (const auto *Image : images())
    for (uint8_t Mode :
         {WdmWakeModeNoCallback, WdmWakeModeNoOutput, WdmWakeModePDO}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(char(Mode));
      auto Input = options(Mode);
      Input.Requests.insert(Input.Requests.end(), {snapshot(true), snapshot()});
      finish(Input);
      auto Result = emulateDriver(Image, Input);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      clean(*Result);
      ASSERT_FALSE(HasFatalFailure());
      expectSnapshot(*Result, 3, 1, 1, Mode != WdmWakeModeNoCallback, false);
      const auto *Snapshot = scenario(*Result, 3);
      ASSERT_NE(Snapshot, nullptr);
      EXPECT_EQ(word(*Snapshot, WdmWakePublishedOutputs),
                unsigned(Mode != WdmWakeModeNoOutput));
      const auto Wake = wakes(*Result);
      ASSERT_EQ(Wake.size(), 1u);
      ASSERT_EQ(Result->PnpDevices.size(), 1u);
      ASSERT_TRUE(Wake.front()->Power->RequestedDeviceObject);
      EXPECT_EQ(*Wake.front()->Power->RequestedDeviceObject ==
                    Result->PnpDevices.front().PDO,
                Mode == WdmWakeModePDO);
    }
#else
  GTEST_SKIP() << "NEVERD_WDM_WAIT_WAKE_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverWdmWaitWake, PassiveCancellationAndRearmUseDifferentPackets) {
#ifdef NEVERD_WDM_WAIT_WAKE_FIXTURE
  for (const auto *Image : images()) {
    auto Input = options();
    Input.Requests.insert(Input.Requests.end(),
                          {command(WdmWakeCancel), snapshot(),
                           command(WdmWakeRearm), snapshot(true), snapshot()});
    finish(Input);
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    ASSERT_FALSE(HasFatalFailure());
    expectSnapshot(*Result, 3, 1, 1, 1, false);
    expectSnapshot(*Result, 6, 2, 2, 2, false);
    const auto *Cancelled = scenario(*Result, 3);
    ASSERT_NE(Cancelled, nullptr);
    EXPECT_EQ(word(*Cancelled, WdmWakeCancelCalls), 1u);
    EXPECT_EQ(word(*Cancelled, WdmWakeCancelTrue), 1u);
    EXPECT_EQ(word(*Cancelled, WdmWakeLastStatus), WdmWakeCancelledStatus);
    EXPECT_EQ(word(*Cancelled, WdmWakeLastIoIrql), 0u);
    EXPECT_EQ(word(*Cancelled, WdmWakeLastCallbackIrql), 0u);
    const auto Wake = wakes(*Result);
    ASSERT_EQ(Wake.size(), 2u);
    EXPECT_NE(Wake[0]->IRP, Wake[1]->IRP);
    EXPECT_TRUE(Wake[0]->CancelRequestedAt100ns);
    EXPECT_EQ(Wake[0]->IOStatus, WdmWakeCancelledStatus);
    EXPECT_FALSE(Wake[0]->Power->WakeSourceDeviceID);
    EXPECT_EQ(Wake[1]->Power->WakeSourceDeviceID, DeviceID.str());
  }
#else
  GTEST_SKIP() << "NEVERD_WDM_WAIT_WAKE_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverWdmWaitWake, TerminalCallbackCanRearmBeforeReturning) {
#ifdef NEVERD_WDM_WAIT_WAKE_FIXTURE
  for (const auto *Image : images()) {
    auto Input = options();
    Input.Requests.insert(Input.Requests.end(),
                          {command(WdmWakeRearmOnSuccess), snapshot(true),
                           snapshot(), snapshot(true), snapshot()});
    finish(Input);
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    ASSERT_FALSE(HasFatalFailure());
    expectSnapshot(*Result, 4, 2, 1, 1, true);
    expectSnapshot(*Result, 6, 2, 2, 2, false);
    const auto Wake = wakes(*Result);
    ASSERT_EQ(Wake.size(), 2u);
    EXPECT_NE(Wake[0]->IRP, Wake[1]->IRP);
    for (const auto *Request : Wake)
      EXPECT_EQ(Request->Power->WakeSourceDeviceID, DeviceID.str());
  }
#else
  GTEST_SKIP() << "NEVERD_WDM_WAIT_WAKE_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverWdmWaitWake, CallbackRequestsAnIndependentD0PowerTransaction) {
#ifdef NEVERD_WDM_WAIT_WAKE_FIXTURE
  for (const auto *Image : images()) {
    auto Input = options();
    Input.PnpDevices.front().RequestedDevicePower = {
        operation(DevicePowerState::D0)};
    Input.Requests.insert(Input.Requests.end(), {command(WdmWakeD0OnSuccess),
                                                 power(DevicePowerState::D2),
                                                 snapshot(true), snapshot()});
    finish(Input);
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    ASSERT_FALSE(HasFatalFailure());
    expectSnapshot(*Result, 5, 1, 1, 1, false);
    const auto *Snapshot = scenario(*Result, 5);
    ASSERT_NE(Snapshot, nullptr);
    EXPECT_EQ(word(*Snapshot, WdmWakeD0Callbacks), 1u);
    EXPECT_EQ(word(*Snapshot, WdmWakeDeviceDispatches), 2u);
    const auto Wake = wakes(*Result);
    ASSERT_EQ(Wake.size(), 1u);
    EXPECT_EQ(Wake.front()->Power->DeviceStateAfter, DevicePowerState::D2);
    const auto D0 = std::find_if(
        Result->Requests.begin(), Result->Requests.end(),
        [](const auto &Request) { return Request.ResponseIndex == 0; });
    ASSERT_NE(D0, Result->Requests.end());
    ASSERT_TRUE(D0->Power);
    EXPECT_EQ(D0->Origin, DriverRequestOrigin::PoRequestPowerIrp);
    EXPECT_EQ(D0->Power->DeviceStateBefore, DevicePowerState::D2);
    EXPECT_EQ(D0->Power->DeviceStateAfter, DevicePowerState::D0);
    EXPECT_NE(D0->IRP, Wake.front()->IRP);
  }
#else
  GTEST_SKIP() << "NEVERD_WDM_WAIT_WAKE_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverWdmWaitWake, DpcCancellationPreservesIrqlAndQueuesPassiveD0) {
#ifdef NEVERD_WDM_WAIT_WAKE_FIXTURE
  for (const auto *Image : images())
    for (uint64_t Base : {uint64_t(0), uint64_t(0x190000000)}) {
      auto Input = options();
      Input.LoadAddress = Base;
      Input.PnpDevices.front().RequestedDevicePower = {
          operation(DevicePowerState::D0)};
      Input.Requests.insert(
          Input.Requests.end(),
          {power(DevicePowerState::D2), command(WdmWakeCancelDpc), snapshot()});
      finish(Input);
      auto Result = emulateDriver(Image, Input);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      clean(*Result);
      ASSERT_FALSE(HasFatalFailure());
      expectSnapshot(*Result, 4, 1, 1, 1, false);
      const auto *Snapshot = scenario(*Result, 4);
      ASSERT_NE(Snapshot, nullptr);
      EXPECT_EQ(word(*Snapshot, WdmWakeLastIoIrql), 2u);
      EXPECT_EQ(word(*Snapshot, WdmWakeLastCallbackIrql), 2u);
      EXPECT_EQ(word(*Snapshot, WdmWakeCancelTrue), 1u);
      EXPECT_EQ(word(*Snapshot, WdmWakeWorkers), 1u);
      EXPECT_EQ(word(*Snapshot, WdmWakeD0Callbacks), 1u);
      const auto *Outer = scenario(*Result, 3);
      ASSERT_NE(Outer, nullptr);
      EXPECT_EQ(Outer->DispatchStatus, windows::StatusPending);
      const auto Wake = wakes(*Result);
      ASSERT_EQ(Wake.size(), 1u);
      EXPECT_EQ(Wake.front()->IOStatus, WdmWakeCancelledStatus);
      EXPECT_EQ(Wake.front()->Power->DeviceStateAfter, DevicePowerState::D2);
      ASSERT_FALSE(Result->PnpDevices.empty());
      EXPECT_EQ(Result->PnpDevices.front().DevicePower, DevicePowerState::D0);
    }
#else
  GTEST_SKIP() << "NEVERD_WDM_WAIT_WAKE_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverWdmWaitWake, StopAndRestartCancelTheOriginalStartObligation) {
#ifdef NEVERD_WDM_WAIT_WAKE_FIXTURE
  for (const auto *Image : images()) {
    auto Input = options();
    Input.Requests.insert(
        Input.Requests.end(),
        {file(DriverRequestKind::Cleanup), file(DriverRequestKind::Close),
         pnp(DevicePnpRequest::QueryStop), pnp(DevicePnpRequest::Stop),
         pnp(DevicePnpRequest::Start), file(DriverRequestKind::Create),
         snapshot()});
    finish(Input);
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    ASSERT_FALSE(HasFatalFailure());
    expectSnapshot(*Result, 8, 2, 1, 1, true);
    const auto *Snapshot = scenario(*Result, 8);
    ASSERT_NE(Snapshot, nullptr);
    EXPECT_EQ(word(*Snapshot, WdmWakeStarts), 2u);
    EXPECT_EQ(word(*Snapshot, WdmWakeStops), 1u);
    const auto Wake = wakes(*Result);
    ASSERT_EQ(Wake.size(), 2u);
    for (const auto *Request : Wake) {
      EXPECT_EQ(Request->IOStatus, WdmWakeCancelledStatus);
      EXPECT_TRUE(Request->CancelRequestedAt100ns);
    }
    EXPECT_NE(Wake[0]->IRP, Wake[1]->IRP);
  }
#else
  GTEST_SKIP() << "NEVERD_WDM_WAIT_WAKE_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverWdmWaitWake, CapturedWakeCannotCompleteASameStartReplacement) {
#ifdef NEVERD_WDM_WAIT_WAKE_FIXTURE
  for (const auto *Image : images()) {
    auto Input = options();
    auto Replace = command(WdmWakeCancelRearm);
    Replace.PowerPolicyEvents = {
        {17, DeviceID.str(), DriverPowerPolicyAction::Wake}};
    Input.Requests.push_back(std::move(Replace));
    Input.Unload = false;
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
    EXPECT_NE(Result->Diagnostic.find("wake"), std::string::npos)
        << Result->Diagnostic;
    const auto Wake = wakes(*Result);
    ASSERT_EQ(Wake.size(), 2u);
    EXPECT_TRUE(Wake[0]->Completed);
    EXPECT_EQ(Wake[0]->IOStatus, WdmWakeCancelledStatus);
    EXPECT_FALSE(Wake[1]->Completed);
    ASSERT_EQ(Result->PowerPolicyEvents.size(), 1u);
    EXPECT_FALSE(Result->PowerPolicyEvents.front().OccurredAt100ns);
  }
#else
  GTEST_SKIP() << "NEVERD_WDM_WAIT_WAKE_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverWdmWaitWake, WakeEventsCannotBindToAFutureRearm) {
#ifdef NEVERD_WDM_WAIT_WAKE_FIXTURE
  for (const auto *Image : images()) {
    auto Input = options();
    Input.Requests.insert(Input.Requests.end(),
                          {command(WdmWakeCancel), snapshot(true),
                           command(WdmWakeRearm)});
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
    EXPECT_NE(Result->Diagnostic.find("wake"), std::string::npos)
        << Result->Diagnostic;
    const auto Wake = wakes(*Result);
    ASSERT_EQ(Wake.size(), 1u);
    EXPECT_TRUE(Wake.front()->Completed);
    EXPECT_EQ(Wake.front()->IOStatus, WdmWakeCancelledStatus);
  }
#else
  GTEST_SKIP() << "NEVERD_WDM_WAIT_WAKE_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverWdmWaitWake, IndependentDevicesReceiveOnlyTheirOwnSignal) {
#ifdef NEVERD_WDM_WAIT_WAKE_FIXTURE
  for (const auto *Image : images()) {
    auto Input = options();
    constexpr llvm::StringLiteral OtherID = "other-native-wake";
    auto Other = Input.PnpDevices.front();
    Other.ID = OtherID.str();
    Input.PnpDevices.push_back(std::move(Other));
    const auto ForOther = [&](DriverRequest Request) {
      Request.DeviceID = OtherID.str();
      if (Request.File)
        Request.File = 2;
      for (auto &Event : Request.PowerPolicyEvents)
        Event.DeviceID = OtherID.str();
      return Request;
    };
    Input.Requests.insert(Input.Requests.end(),
                          {ForOther(pnp(DevicePnpRequest::Start)),
                           ForOther(file(DriverRequestKind::Create)),
                           snapshot(true), snapshot(), ForOther(snapshot())});
    finish(Input);
    for (auto Request :
         {file(DriverRequestKind::Cleanup), file(DriverRequestKind::Close),
          pnp(DevicePnpRequest::QueryRemove), pnp(DevicePnpRequest::Remove)})
      Input.Requests.push_back(ForOther(std::move(Request)));
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    ASSERT_FALSE(HasFatalFailure());
    expectSnapshot(*Result, 5, 1, 1, 1, false);
    expectSnapshot(*Result, 6, 1, 0, 0, true);
    const auto Wake = wakes(*Result);
    ASSERT_EQ(Wake.size(), 2u);
    EXPECT_EQ(Wake[0]->Power->WakeSourceDeviceID, DeviceID.str());
    EXPECT_EQ(Wake[1]->DeviceID, OtherID.str());
    EXPECT_EQ(Wake[1]->IOStatus, WdmWakeCancelledStatus);
    EXPECT_FALSE(Wake[1]->Power->WakeSourceDeviceID);
  }
#else
  GTEST_SKIP() << "NEVERD_WDM_WAIT_WAKE_FIXTURE requires a genuine WDK fixture";
#endif
}
} // namespace
} // namespace neverd::emulation
