//===- DriverKMDFPowerPolicyTests.cpp - Genuine WDK idle and wake --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "fixtures/driver_kmdf_power_policy_test.h"
#include "gtest/gtest.h"
#include "os/windows/KernelFramework.h"
#include "os/windows/WindowsKernelLayout.h"

#include "neverd/emulation/DriverSession.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
#ifdef NEVERD_KMDF_PNP_FIXTURE
constexpr llvm::StringLiteral DeviceID = "power-policy-pdo";
constexpr uint64_t CompletionDelay100ns = 3;
constexpr uint64_t WakeAfter100ns = KmdfPowerTimeout100ns + 17;

std::vector<const char *> images() {
  std::vector<const char *> Result{NEVERD_KMDF_PNP_FIXTURE};
#ifdef NEVERD_KMDF_PNP_CFG_FIXTURE
  Result.push_back(NEVERD_KMDF_PNP_CFG_FIXTURE);
#endif
  return Result;
}
DriverRequest pnp(DevicePnpRequest Minor) {
  DriverRequest Request;
  Request.Kind = DriverRequestKind::Pnp;
  Request.DeviceID = DeviceID.str();
  Request.Pnp = DriverPnpOperation{Minor, {windows::StatusSuccess, 0}};
  return Request;
}
DriverRequest file(DriverRequestKind Kind,
                   uint8_t Command = KmdfPowerSnapshot) {
  DriverRequest Request;
  Request.Kind = Kind;
  Request.DeviceID = DeviceID.str();
  Request.File = 1;
  if (Kind == DriverRequestKind::DeviceControl) {
    Request.ControlCode = 0x222000;
    Request.Input = {Command};
    Request.OutputSize = KmdfPowerSnapshotWords * sizeof(uint32_t);
  }
  return Request;
}
DriverPowerOperation devicePower(DevicePowerState Target) {
  DriverPowerOperation Operation;
  Operation.Minor = DevicePowerRequest::Set;
  Operation.Type = DriverPowerType::Device;
  Operation.State = uint32_t(Target);
  Operation.BusCompletion = {windows::StatusSuccess, CompletionDelay100ns};
  return Operation;
}
DriverOptions options(char Mode) {
  DriverOptions Options;
  Options.ServiceName = std::string("NeverDKmdfPower") + Mode + "-";
  Options.Unload = true;
  DriverPnpDevice Device;
  Device.ID = DeviceID.str();
  Device.InitialDevicePower = DevicePowerState::D0;
  Device.InitialSystemPower = SystemPowerState::Working;
  Device.WakeCapabilities = DriverWakeCapabilities{true, true};
  Device.RequestedDevicePower = {devicePower(DevicePowerState::D3),
                                 devicePower(DevicePowerState::D0)};
  Options.PnpDevices.push_back(std::move(Device));
  Options.Requests = {pnp(DevicePnpRequest::Start),
                      file(DriverRequestKind::Create),
                      file(DriverRequestKind::DeviceControl),
                      file(DriverRequestKind::DeviceControl),
                      file(DriverRequestKind::Cleanup),
                      file(DriverRequestKind::Close),
                      pnp(DevicePnpRequest::QueryRemove),
                      pnp(DevicePnpRequest::Remove)};
  return Options;
}
uint32_t word(const DriverRequestResult &Request, size_t Index) {
  uint32_t Value = 0;
  if (Request.Output.size() >= (Index + 1) * sizeof(Value))
    for (size_t I = 0; I < sizeof(Value); ++I)
      Value |= uint32_t(Request.Output[Index * sizeof(Value) + I]) << (I * 8);
  return Value;
}
const DriverRequestResult *scenarioRequest(const DriverResult &Result,
                                           size_t Index) {
  size_t Found = 0;
  for (const auto &Request : Result.Requests)
    if (Request.Origin == DriverRequestOrigin::Scenario && Found++ == Index)
      return &Request;
  return nullptr;
}

TEST(DriverKMDFPowerPolicy, IdleWakeRetainsWaitWakeAndOrdersCallbacks) {
  for (const auto *Image : images())
    for (const uint64_t Base : {uint64_t(0), uint64_t(0x190000000)}) {
      auto Input = options(KmdfPowerIdleWake);
      Input.LoadAddress = Base;
      Input.Requests[2].PowerPolicyEvents = {
          {0, DeviceID.str(), DriverPowerPolicyAction::Idle},
          {WakeAfter100ns, DeviceID.str(), DriverPowerPolicyAction::Wake}};
      auto Result = emulateDriver(Image, Input);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
      EXPECT_TRUE(Result->UnloadCompleted);
      ASSERT_EQ(Result->PowerPolicyEvents.size(), 2u);
      EXPECT_TRUE(Result->PowerPolicyEvents[0].OccurredAt100ns);
      EXPECT_EQ(Result->PowerPolicyEvents[1].OccurredAt100ns, WakeAfter100ns);
      const auto *Snapshot = scenarioRequest(*Result, 3);
      ASSERT_NE(Snapshot, nullptr);
      ASSERT_EQ(Snapshot->Output.size(),
                KmdfPowerSnapshotWords * sizeof(uint32_t));
      EXPECT_EQ(word(*Snapshot, 0), 2u);
      EXPECT_EQ(word(*Snapshot, 1), 1u);
      EXPECT_EQ(word(*Snapshot, 2), 1u);
      EXPECT_EQ(word(*Snapshot, 3), 1u);
      EXPECT_EQ(word(*Snapshot, 4), 1u);
      EXPECT_EQ(word(*Snapshot, 5), 1u);
      const auto Wake = std::find_if(
          Result->Requests.begin(), Result->Requests.end(),
          [](const auto &Request) {
            return Request.Origin == DriverRequestOrigin::FrameworkWaitWake;
          });
      ASSERT_NE(Wake, Result->Requests.end());
      ASSERT_TRUE(Wake->Power);
      EXPECT_TRUE(Wake->Completed);
      EXPECT_EQ(Wake->DispatchStatus, windows::StatusPending);
      EXPECT_EQ(Wake->IOStatus, windows::StatusSuccess);
      EXPECT_EQ(Wake->Power->Minor, DevicePowerRequest::WaitWake);
      EXPECT_EQ(Wake->Power->State, uint32_t(SystemPowerState::Working));
      EXPECT_EQ(Wake->Power->DeviceStateAfter, DevicePowerState::D3);
      EXPECT_EQ(Result->PowerPolicyEvents[0].DeviceEpoch, 1u);
      EXPECT_EQ(Wake->Power->BusReceivedAt100ns, KmdfPowerTimeout100ns);
      EXPECT_EQ(Wake->Power->BusCompletedAt100ns, WakeAfter100ns);
      EXPECT_EQ(Result->Requests.size(), Input.Requests.size() + 3);
    }
}

TEST(DriverKMDFPowerPolicy, StopIdleWaitResumesOnlyAfterDevicePowerCompletion) {
  for (const auto *Image : images()) {
    auto Input = options(KmdfPowerIdleOnly);
    Input.Requests[2].PowerPolicyEvents = {
        {0, DeviceID.str(), DriverPowerPolicyAction::Idle}};
    Input.Requests[3] = file(DriverRequestKind::DeviceControl, KmdfPowerWaitD0);
    Input.Requests[3].PowerPolicyEvents = {
        {0, DeviceID.str(), DriverPowerPolicyAction::Active}};
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    const auto *Snapshot = scenarioRequest(*Result, 3);
    ASSERT_NE(Snapshot, nullptr);
    EXPECT_EQ(Snapshot->IOStatus, windows::StatusSuccess);
    EXPECT_EQ(word(*Snapshot, 0), 2u);
    EXPECT_EQ(word(*Snapshot, 1), 1u);
    EXPECT_EQ(word(*Snapshot, 2), 0u);
    EXPECT_EQ(word(*Snapshot, 5), 1u);
    EXPECT_EQ(Result->Requests.size(), Input.Requests.size() + 2);
  }
}

TEST(DriverKMDFPowerPolicy, IdleReferencesPreventTimeoutUntilMatchingResume) {
  auto Input = options(KmdfPowerIdleOnly);
  Input.Requests[2] = file(DriverRequestKind::DeviceControl, KmdfPowerHold);
  Input.Requests[2].PowerPolicyEvents = {
      {0, DeviceID.str(), DriverPowerPolicyAction::Idle}};
  Input.Requests[3] = file(DriverRequestKind::DeviceControl, KmdfPowerRelease);
  Input.Requests.insert(
      Input.Requests.begin() + 4,
      file(DriverRequestKind::DeviceControl, KmdfPowerWaitD0));
  Input.Requests[4].PowerPolicyEvents = {
      {0, DeviceID.str(), DriverPowerPolicyAction::Active}};
  auto Result = emulateDriver(NEVERD_KMDF_PNP_FIXTURE, Input);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  const auto *Held = scenarioRequest(*Result, 3);
  ASSERT_NE(Held, nullptr);
  EXPECT_EQ(word(*Held, 0), 1u);
  EXPECT_EQ(word(*Held, 1), 0u);
  const auto *Resumed = scenarioRequest(*Result, 4);
  ASSERT_NE(Resumed, nullptr);
  EXPECT_EQ(word(*Resumed, 0), 2u);
  EXPECT_EQ(word(*Resumed, 1), 1u);
}

DriverRequest systemPower(SystemPowerState State) {
  DriverRequest Request;
  Request.Kind = DriverRequestKind::Power;
  Request.DeviceID = DeviceID.str();
  auto Operation = devicePower(DevicePowerState::D0);
  Operation.Type = DriverPowerType::System;
  Operation.State = uint32_t(State);
  Operation.Action = State == SystemPowerState::Working
                         ? DriverPowerAction::None
                         : DriverPowerAction::Sleep;
  Request.Power = Operation;
  return Request;
}

TEST(DriverKMDFPowerPolicy, SleepingWorkerWaitsForExplicitSystemResume) {
  for (const auto *Image : images()) {
    auto Input = options(KmdfPowerRemainIdleOnSystemWake);
    Input.PnpDevices.front().RequestedDevicePower.front().Action =
        DriverPowerAction::Sleep;
    Input.Requests[1].AsynchronousFile = true;
    auto Waiting =
        file(DriverRequestKind::DeviceControl, KmdfPowerWaitD0Worker);
    Waiting.DeferCallbackDrain = true;
    Input.Requests = {Input.Requests[0],
                      Input.Requests[1],
                      systemPower(SystemPowerState::Sleeping3),
                      Waiting,
                      systemPower(SystemPowerState::Working),
                      Input.Requests[3],
                      Input.Requests[4],
                      Input.Requests[5],
                      Input.Requests[6],
                      Input.Requests[7]};
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    EXPECT_TRUE(Result->UnloadCompleted);
    const auto *Snapshot = scenarioRequest(*Result, 3);
    ASSERT_NE(Snapshot, nullptr);
    EXPECT_EQ(Snapshot->DispatchStatus, windows::StatusPending);
    EXPECT_EQ(Snapshot->IOStatus, windows::StatusSuccess);
    EXPECT_EQ(word(*Snapshot, 0), 2u);
    EXPECT_EQ(word(*Snapshot, 1), 1u);
    EXPECT_EQ(word(*Snapshot, 5), 1u);
    const auto Begin =
        std::find(Result->Messages.begin(), Result->Messages.end(),
                  "KMDF power: waiting for D0\n");
    const auto End = std::find(Result->Messages.begin(), Result->Messages.end(),
                               "KMDF power: resumed in D0\n");
    ASSERT_NE(Begin, Result->Messages.end());
    ASSERT_NE(End, Result->Messages.end());
    EXPECT_LT(Begin, End);
  }
}

TEST(DriverKMDFPowerPolicy, SleepingReferenceRequestsPowerOnSystemResume) {
  for (const auto *Image : images()) {
    auto Input = options(KmdfPowerRemainIdleOnSystemWake);
    Input.PnpDevices.front().RequestedDevicePower.front().Action =
        DriverPowerAction::Sleep;
    Input.Requests = {Input.Requests[0],
                      Input.Requests[1],
                      systemPower(SystemPowerState::Sleeping3),
                      file(DriverRequestKind::DeviceControl, KmdfPowerHold),
                      systemPower(SystemPowerState::Working),
                      file(DriverRequestKind::DeviceControl, KmdfPowerRelease),
                      Input.Requests[4],
                      Input.Requests[5],
                      Input.Requests[6],
                      Input.Requests[7]};
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    const auto *Sleeping = scenarioRequest(*Result, 3);
    ASSERT_NE(Sleeping, nullptr);
    EXPECT_EQ(word(*Sleeping, 0), 1u);
    EXPECT_EQ(word(*Sleeping, 5), 0u);
    const auto *Awake = scenarioRequest(*Result, 5);
    ASSERT_NE(Awake, nullptr);
    EXPECT_EQ(word(*Awake, 0), 2u);
    EXPECT_EQ(word(*Awake, 5), 1u);
  }
}

TEST(DriverKMDFPowerPolicy, SystemWakeWaitsForExplicitSystemResume) {
  for (const auto *Image : images()) {
    auto Input = options(KmdfPowerSystemWake);
    Input.PnpDevices.front().RequestedDevicePower.front().Action =
        DriverPowerAction::Sleep;
    Input.Requests.insert(Input.Requests.begin() + 2,
                          systemPower(SystemPowerState::Sleeping3));
    Input.Requests[3].PowerPolicyEvents = {
        {0, DeviceID.str(), DriverPowerPolicyAction::Wake}};
    Input.Requests.insert(Input.Requests.begin() + 4,
                          systemPower(SystemPowerState::Working));
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    const auto *Sleeping = scenarioRequest(*Result, 3);
    ASSERT_NE(Sleeping, nullptr);
    EXPECT_EQ(word(*Sleeping, 0), 1u);
    EXPECT_EQ(word(*Sleeping, 5), 0u);
    const auto *Awake = scenarioRequest(*Result, 5);
    ASSERT_NE(Awake, nullptr);
    EXPECT_EQ(word(*Awake, 0), 2u);
    EXPECT_EQ(word(*Awake, 2), 1u);
    EXPECT_EQ(word(*Awake, 3), 1u);
    EXPECT_EQ(word(*Awake, 4), 1u);
    const auto Wake = std::find_if(
        Result->Requests.begin(), Result->Requests.end(),
        [](const auto &Request) {
          return Request.Origin == DriverRequestOrigin::FrameworkWaitWake;
        });
    ASSERT_NE(Wake, Result->Requests.end());
    ASSERT_TRUE(Wake->Power);
    EXPECT_EQ(Wake->Power->State, uint32_t(SystemPowerState::Sleeping3));
    EXPECT_EQ(Wake->Power->SystemStateAfter, SystemPowerState::Sleeping3);
  }
}

TEST(DriverKMDFPowerPolicy, ArmFailureCancelsWaitWakeWithS0AndSxDisarmRules) {
  for (const char Mode :
       {char(KmdfPowerArmFailure), char(KmdfPowerSystemArmFailure)}) {
    auto Input = options(Mode);
    const bool System = Mode == KmdfPowerSystemArmFailure;
    if (System) {
      Input.PnpDevices.front().RequestedDevicePower.front().Action =
          DriverPowerAction::Sleep;
      Input.Requests.insert(Input.Requests.begin() + 2,
                            systemPower(SystemPowerState::Sleeping3));
      Input.Requests.insert(Input.Requests.begin() + 3,
                            systemPower(SystemPowerState::Working));
    } else {
      Input.PnpDevices.front().RequestedDevicePower.resize(1);
      Input.Requests[2].PowerPolicyEvents = {
          {0, DeviceID.str(), DriverPowerPolicyAction::Idle}};
    }
    auto Result = emulateDriver(NEVERD_KMDF_PNP_FIXTURE, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    const auto *Snapshot = scenarioRequest(*Result, System ? 5 : 3);
    ASSERT_NE(Snapshot, nullptr);
    EXPECT_EQ(word(*Snapshot, 0), System ? 2u : 1u);
    EXPECT_EQ(word(*Snapshot, 2), 1u);
    EXPECT_EQ(word(*Snapshot, 3), 0u);
    EXPECT_EQ(word(*Snapshot, 4), System ? 1u : 0u);
    EXPECT_EQ(word(*Snapshot, 5), 1u);
    const auto Wake = std::find_if(
        Result->Requests.begin(), Result->Requests.end(),
        [](const auto &Request) {
          return Request.Origin == DriverRequestOrigin::FrameworkWaitWake;
        });
    ASSERT_NE(Wake, Result->Requests.end());
    EXPECT_TRUE(Wake->Completed);
    EXPECT_EQ(Wake->IOStatus, framework::RequestCancelled);
  }
}

TEST(DriverKMDFPowerPolicy, RemoveCancelsRetainedWaitWakeBeforeDeviceDeletion) {
  auto Input = options(KmdfPowerIdleWake);
  Input.PnpDevices.front().RequestedDevicePower.resize(1);
  Input.Requests[2].PowerPolicyEvents = {
      {0, DeviceID.str(), DriverPowerPolicyAction::Idle}};
  auto Result = emulateDriver(NEVERD_KMDF_PNP_FIXTURE, Input);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  EXPECT_TRUE(Result->UnloadCompleted);
  const auto Wake = std::find_if(
      Result->Requests.begin(), Result->Requests.end(),
      [](const auto &Request) {
        return Request.Origin == DriverRequestOrigin::FrameworkWaitWake;
      });
  ASSERT_NE(Wake, Result->Requests.end());
  EXPECT_TRUE(Wake->Completed);
  EXPECT_EQ(Wake->IOStatus, framework::RequestCancelled);
}

TEST(DriverKMDFPowerPolicy, ManagedQueueArrivalRequestsD0BeforeDelivery) {
  auto Input = options(KmdfPowerManagedQueue);
  Input.Requests[2].PowerPolicyEvents = {
      {0, DeviceID.str(), DriverPowerPolicyAction::Idle}};
  auto Result = emulateDriver(NEVERD_KMDF_PNP_FIXTURE, Input);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  const auto *Snapshot = scenarioRequest(*Result, 3);
  ASSERT_NE(Snapshot, nullptr);
  EXPECT_EQ(word(*Snapshot, 0), 2u);
  EXPECT_EQ(word(*Snapshot, 1), 1u);
  EXPECT_EQ(word(*Snapshot, 5), 1u);
}

TEST(DriverKMDFPowerPolicy, ManagedQueueCallbackCannotSynchronouslyStopIdle) {
  auto Input = options(KmdfPowerManagedWait);
  Input.Requests[2] = file(DriverRequestKind::DeviceControl, KmdfPowerWaitD0);
  auto Result = emulateDriver(NEVERD_KMDF_PNP_FIXTURE, Input);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
  EXPECT_NE(Result->Diagnostic.find("StopIdle"), std::string::npos);
}

TEST(DriverKMDFPowerPolicy, IdleDevicePowersUpBeforeSystemWakeArming) {
  auto Input = options(KmdfPowerIdleAndSystemWake);
  Input.Requests[2].PowerPolicyEvents = {
      {0, DeviceID.str(), DriverPowerPolicyAction::Idle}};
  auto Down = devicePower(DevicePowerState::D3);
  Down.Action = DriverPowerAction::Sleep;
  Input.PnpDevices.front().RequestedDevicePower.push_back(Down);
  Input.PnpDevices.front().RequestedDevicePower.push_back(
      devicePower(DevicePowerState::D0));
  Input.Requests.insert(Input.Requests.begin() + 3,
                        systemPower(SystemPowerState::Sleeping3));
  Input.Requests[4].PowerPolicyEvents = {
      {0, DeviceID.str(), DriverPowerPolicyAction::Wake}};
  Input.Requests.insert(Input.Requests.begin() + 5,
                        systemPower(SystemPowerState::Working));
  Input.Requests.insert(Input.Requests.begin() + 6,
                        file(DriverRequestKind::DeviceControl));
  auto Result = emulateDriver(NEVERD_KMDF_PNP_FIXTURE, Input);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  const auto *Snapshot = scenarioRequest(*Result, 6);
  ASSERT_NE(Snapshot, nullptr);
  EXPECT_EQ(word(*Snapshot, 0), 3u);
  EXPECT_EQ(word(*Snapshot, 1), 2u);
  EXPECT_EQ(word(*Snapshot, 2), 2u);
  EXPECT_EQ(word(*Snapshot, 3), 1u);
  EXPECT_EQ(word(*Snapshot, 4), 2u);
}

TEST(DriverKMDFPowerPolicy, SystemResumeHonorsRemainIdleUntilExplicitActivity) {
  auto Input = options(KmdfPowerRemainIdleOnSystemWake);
  Input.Requests[2].PowerPolicyEvents = {
      {0, DeviceID.str(), DriverPowerPolicyAction::Idle}};
  auto Down = devicePower(DevicePowerState::D3);
  Down.Action = DriverPowerAction::Sleep;
  Input.PnpDevices.front().RequestedDevicePower.insert(
      Input.PnpDevices.front().RequestedDevicePower.begin() + 1, Down);
  Input.Requests.insert(Input.Requests.begin() + 3,
                        systemPower(SystemPowerState::Sleeping3));
  Input.Requests.insert(Input.Requests.begin() + 4,
                        systemPower(SystemPowerState::Working));
  Input.Requests[5].PowerPolicyEvents = {
      {0, DeviceID.str(), DriverPowerPolicyAction::Active}};
  Input.Requests.insert(Input.Requests.begin() + 6,
                        file(DriverRequestKind::DeviceControl));
  auto Result = emulateDriver(NEVERD_KMDF_PNP_FIXTURE, Input);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  const auto *Idle = scenarioRequest(*Result, 5);
  ASSERT_NE(Idle, nullptr);
  EXPECT_EQ(word(*Idle, 0), 1u);
  EXPECT_EQ(word(*Idle, 5), 0u);
  const auto *Awake = scenarioRequest(*Result, 6);
  ASSERT_NE(Awake, nullptr);
  EXPECT_EQ(word(*Awake, 0), 2u);
  EXPECT_EQ(word(*Awake, 5), 1u);
}

TEST(DriverKMDFPowerPolicy, MissingIdleResponseDoesNotPublishWaitWake) {
  auto Input = options(KmdfPowerIdleWake);
  Input.PnpDevices.front().RequestedDevicePower.clear();
  Input.Requests[2].PowerPolicyEvents = {
      {0, DeviceID.str(), DriverPowerPolicyAction::Idle}};
  auto Result = emulateDriver(NEVERD_KMDF_PNP_FIXTURE, Input);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
  EXPECT_NE(Result->Diagnostic.find("response FIFO"), std::string::npos);
  EXPECT_TRUE(std::none_of(
      Result->Requests.begin(), Result->Requests.end(),
      [](const auto &Request) {
        return Request.Origin == DriverRequestOrigin::FrameworkWaitWake ||
               Request.Origin == DriverRequestOrigin::FrameworkPowerPolicy;
      }));
}
#endif
} // namespace
} // namespace neverd::emulation
