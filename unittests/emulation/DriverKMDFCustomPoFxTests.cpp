//===- DriverKMDFCustomPoFxTests.cpp - Genuine KMDF custom component ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercise genuine WDK component callbacks, queue gates and handle lifetimes.
///
//===----------------------------------------------------------------------===//
#include "fixtures/driver_kmdf_pofx_test.h"
#include "gtest/gtest.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

#include "neverd/emulation/DriverSession.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
#ifdef NEVERD_KMDF_POFX_FIXTURE
constexpr llvm::StringLiteral DeviceID = "custom-pofx-pdo";
constexpr uint64_t ComponentIdleTime = 3;
constexpr uint64_t IdleTimeout100ns = KmdfPoFxIdleTimeoutMilliseconds * 10000;
constexpr uint64_t PowerDecisionTime = IdleTimeout100ns + 1;
constexpr uint64_t ResumeTime = IdleTimeout100ns + 17;
constexpr uint64_t ProviderPowerDelay = 3;

std::vector<const char *> images() {
  return {
      NEVERD_KMDF_POFX_FIXTURE,
#ifdef NEVERD_KMDF_POFX_CFG_FIXTURE
      NEVERD_KMDF_POFX_CFG_FIXTURE,
#endif
  };
}

DriverRequest pnp(DevicePnpRequest Minor) {
  DriverRequest Request;
  Request.Kind = DriverRequestKind::Pnp;
  Request.DeviceID = DeviceID.str();
  Request.Pnp = DriverPnpOperation{Minor, {windows::StatusSuccess, 0}};
  return Request;
}

DriverRequest file(DriverRequestKind Kind, uint32_t ID = 1) {
  DriverRequest Request;
  Request.Kind = Kind;
  Request.DeviceID = DeviceID.str();
  Request.File = ID;
  if (Kind == DriverRequestKind::DeviceControl) {
    Request.ControlCode = KmdfPoFxSnapshotIoctl;
    Request.OutputSize = KmdfPoFxSnapshotWords * sizeof(uint32_t);
  }
  return Request;
}

DriverPowerOperation power(DevicePowerState State) {
  DriverPowerOperation Operation;
  Operation.Type = DriverPowerType::Device;
  Operation.State = uint32_t(State);
  Operation.BusCompletion = {windows::StatusSuccess, ProviderPowerDelay};
  return Operation;
}

DriverOptions options(char Mode) {
  DriverOptions Options;
  Options.ServiceName = std::string("NeverDKmdfPoFx") + Mode;
  Options.Unload = true;
  DriverPnpDevice Device;
  Device.ID = DeviceID.str();
  Device.InitialDevicePower = DevicePowerState::D0;
  Device.InitialSystemPower = SystemPowerState::Working;
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

void decisions(DriverRequest &Request, bool ComponentState) {
  Request.PowerPolicyEvents = {
      {0, DeviceID.str(), DriverPowerPolicyAction::Idle}};
  if (ComponentState)
    Request.PowerPolicyEvents.push_back(
        {ComponentIdleTime, DeviceID.str(),
         DriverPowerPolicyAction::ComponentIdleState, 0, 1});
  Request.PowerPolicyEvents.push_back(
      {PowerDecisionTime, DeviceID.str(),
       DriverPowerPolicyAction::PowerNotRequired});
  Request.PowerPolicyEvents.push_back(
      {ResumeTime, DeviceID.str(), DriverPowerPolicyAction::Active});
}

const DriverRequestResult *scenarioRequest(const DriverResult &Result,
                                           size_t Expected) {
  size_t Index = 0;
  for (const auto &Request : Result.Requests)
    if (Request.Origin == DriverRequestOrigin::Scenario && Index++ == Expected)
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

size_t messageIndex(const DriverResult &Result, llvm::StringRef Message) {
  const auto Found =
      std::find(Result.Messages.begin(), Result.Messages.end(), Message.str());
  EXPECT_NE(Found, Result.Messages.end()) << Message.str();
  return size_t(Found - Result.Messages.begin());
}

void expectClean(const DriverResult &Result) {
  EXPECT_EQ(Result.Stop, DriverStopReason::Returned) << Result.Diagnostic;
  EXPECT_TRUE(Result.UnloadCompleted);
  for (const auto &Message : Result.Messages)
    EXPECT_FALSE(llvm::StringRef(Message).starts_with("KMDF PoFx: invalid"))
        << Message;
  EXPECT_LT(messageIndex(Result, "KMDF PoFx: pre unregister 1\n"),
            messageIndex(Result, "KMDF PoFx: device cleanup\n"));
  messageIndex(Result, "KMDF PoFx: unload failures 0\n");
}
#endif

TEST(DriverKMDFCustomPoFx, GuestFxCallbacksGateRequestsUntilF0AndD0Complete) {
#ifdef NEVERD_KMDF_POFX_FIXTURE
  for (const auto *Image : images())
    for (const char Mode :
         {char(KmdfPoFxNormal), char(KmdfPoFxDeferredIdle),
          char(KmdfPoFxDefaultConditions), char(KmdfPoFxSelfManagedSettings)})
      for (const uint64_t Base : {uint64_t(0), uint64_t(0x190000000)}) {
        SCOPED_TRACE(Image);
        SCOPED_TRACE(Mode);
        SCOPED_TRACE(Base);
        auto Input = options(Mode);
        Input.LoadAddress = Base;
        Input.PnpDevices.front().RequestedDevicePower = {
            power(DevicePowerState::D3), power(DevicePowerState::D0)};
        decisions(Input.Requests[2], true);
        auto Result = emulateDriver(Image, Input);
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        ASSERT_EQ(Result->Stop, DriverStopReason::Returned)
            << Result->Diagnostic;
        expectClean(*Result);
        const auto *Snapshot = scenarioRequest(*Result, 3);
        ASSERT_NE(Snapshot, nullptr);
        ASSERT_EQ(Snapshot->Output.size(),
                  KmdfPoFxSnapshotWords * sizeof(uint32_t));
        EXPECT_EQ(Snapshot->IOStatus, windows::StatusSuccess);
        EXPECT_EQ(word(*Snapshot, KmdfPoFxSnapshotFailures), 0u);
        EXPECT_EQ(word(*Snapshot, KmdfPoFxSnapshotPosts), 1u);
        EXPECT_EQ(word(*Snapshot, KmdfPoFxSnapshotPres), 0u);
        EXPECT_EQ(word(*Snapshot, KmdfPoFxSnapshotF0Transitions), 1u);
        EXPECT_EQ(word(*Snapshot, KmdfPoFxSnapshotF1Transitions), 1u);
        EXPECT_EQ(word(*Snapshot, KmdfPoFxSnapshotCurrentState), 0u);
        EXPECT_EQ(word(*Snapshot, KmdfPoFxSnapshotD0Entries), 2u);
        EXPECT_EQ(word(*Snapshot, KmdfPoFxSnapshotD0Exits), 1u);
        const uint32_t Conditions = Mode == KmdfPoFxDefaultConditions ? 0 : 1;
        EXPECT_EQ(word(*Snapshot, KmdfPoFxSnapshotActiveConditions),
                  Conditions);
        EXPECT_EQ(word(*Snapshot, KmdfPoFxSnapshotIdleConditions), Conditions);
        EXPECT_EQ(Result->Requests.size(), Input.Requests.size() + 2);
        EXPECT_LT(messageIndex(*Result, "KMDF PoFx: D0 entry\n"),
                  messageIndex(*Result, "KMDF PoFx: post register 1\n"));
        EXPECT_LT(messageIndex(*Result, "KMDF PoFx: state F1\n"),
                  messageIndex(*Result, "KMDF PoFx: D0 exit\n"));
        if (Conditions) {
          EXPECT_LT(messageIndex(*Result, "KMDF PoFx: idle acknowledged\n"),
                    messageIndex(*Result, "KMDF PoFx: state F1\n"));
          EXPECT_LT(messageIndex(*Result, "KMDF PoFx: state F0\n"),
                    messageIndex(*Result, "KMDF PoFx: active condition\n"));
        }
      }
#else
  GTEST_SKIP() << "NEVERD_KMDF_POFX_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverKMDFCustomPoFx,
     DefaultComponentStillOwnsRealDevicePowerTransitions) {
#ifdef NEVERD_KMDF_POFX_FIXTURE
  for (const auto *Image : images()) {
    auto Input = options(KmdfPoFxDefaultComponent);
    Input.PnpDevices.front().RequestedDevicePower = {
        power(DevicePowerState::D3), power(DevicePowerState::D0)};
    decisions(Input.Requests[2], false);
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    expectClean(*Result);
    const auto *Snapshot = scenarioRequest(*Result, 3);
    ASSERT_NE(Snapshot, nullptr);
    EXPECT_EQ(word(*Snapshot, KmdfPoFxSnapshotFailures), 0u);
    EXPECT_EQ(word(*Snapshot, KmdfPoFxSnapshotF1Transitions), 0u);
    EXPECT_EQ(word(*Snapshot, KmdfPoFxSnapshotD0Entries), 2u);
    EXPECT_EQ(word(*Snapshot, KmdfPoFxSnapshotD0Exits), 1u);
    EXPECT_EQ(Result->Requests.size(), Input.Requests.size() + 2);
  }
#else
  GTEST_SKIP() << "NEVERD_KMDF_POFX_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverKMDFCustomPoFx,
     RequestArrivalRestoresF0BeforeDeliveryWithoutAnotherDevicePowerIrp) {
#ifdef NEVERD_KMDF_POFX_FIXTURE
  for (const auto *Image : images())
    for (const char Mode :
         {char(KmdfPoFxNormal), char(KmdfPoFxDefaultConditions)}) {
      auto Input = options(Mode);
      Input.Requests[2].PowerPolicyEvents = {
          {0, DeviceID.str(), DriverPowerPolicyAction::Idle},
          {ComponentIdleTime, DeviceID.str(),
           DriverPowerPolicyAction::ComponentIdleState, 0, 1}};
      auto Result = emulateDriver(Image, Input);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
      expectClean(*Result);
      const auto *Snapshot = scenarioRequest(*Result, 3);
      ASSERT_NE(Snapshot, nullptr);
      EXPECT_EQ(Snapshot->IOStatus, windows::StatusSuccess);
      EXPECT_EQ(word(*Snapshot, KmdfPoFxSnapshotFailures), 0u);
      EXPECT_EQ(word(*Snapshot, KmdfPoFxSnapshotF0Transitions), 1u);
      EXPECT_EQ(word(*Snapshot, KmdfPoFxSnapshotF1Transitions), 1u);
      EXPECT_EQ(word(*Snapshot, KmdfPoFxSnapshotD0Entries), 1u);
      EXPECT_EQ(word(*Snapshot, KmdfPoFxSnapshotD0Exits), 0u);
      EXPECT_EQ(Result->Requests.size(), Input.Requests.size());
    }
#else
  GTEST_SKIP() << "NEVERD_KMDF_POFX_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverKMDFCustomPoFx, StopAndRestartBalanceIndependentRegistrations) {
#ifdef NEVERD_KMDF_POFX_FIXTURE
  for (const auto *Image : images())
    for (const char Mode :
         {char(KmdfPoFxNormal), char(KmdfPoFxSelfManagedSettings)}) {
      auto Input = options(Mode);
      Input.Requests.resize(6);
      Input.Requests.push_back(pnp(DevicePnpRequest::QueryStop));
      Input.Requests.push_back(pnp(DevicePnpRequest::Stop));
      Input.Requests.push_back(pnp(DevicePnpRequest::Start));
      Input.Requests.push_back(file(DriverRequestKind::Create, 2));
      Input.Requests.push_back(file(DriverRequestKind::DeviceControl, 2));
      Input.Requests.push_back(file(DriverRequestKind::Cleanup, 2));
      Input.Requests.push_back(file(DriverRequestKind::Close, 2));
      Input.Requests.push_back(pnp(DevicePnpRequest::QueryRemove));
      Input.Requests.push_back(pnp(DevicePnpRequest::Remove));
      auto Result = emulateDriver(Image, Input);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
      expectClean(*Result);
      const auto *Snapshot = scenarioRequest(*Result, 10);
      ASSERT_NE(Snapshot, nullptr);
      EXPECT_EQ(word(*Snapshot, KmdfPoFxSnapshotFailures), 0u);
      EXPECT_EQ(word(*Snapshot, KmdfPoFxSnapshotPosts), 2u);
      EXPECT_EQ(word(*Snapshot, KmdfPoFxSnapshotPres), 1u);
      EXPECT_EQ(word(*Snapshot, KmdfPoFxSnapshotD0Entries), 2u);
      EXPECT_EQ(word(*Snapshot, KmdfPoFxSnapshotD0Exits), 1u);
      EXPECT_LT(messageIndex(*Result, "KMDF PoFx: pre unregister 1\n"),
                messageIndex(*Result, "KMDF PoFx: post register 2\n"));
      EXPECT_LT(messageIndex(*Result, "KMDF PoFx: post register 2\n"),
                messageIndex(*Result, "KMDF PoFx: pre unregister 2\n"));
    }
#else
  GTEST_SKIP() << "NEVERD_KMDF_POFX_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverKMDFCustomPoFx,
     FailedPostReturnsStartFailureAndUnregistersBeforeCleanup) {
#ifdef NEVERD_KMDF_POFX_FIXTURE
  for (const auto *Image : images())
    for (const uint64_t Base : {uint64_t(0), uint64_t(0x190000000)}) {
      auto Input = options(KmdfPoFxFailPostRegister);
      Input.LoadAddress = Base;
      Input.Requests = {pnp(DevicePnpRequest::Start),
                        pnp(DevicePnpRequest::Remove)};
      auto Result = emulateDriver(Image, Input);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
      expectClean(*Result);
      ASSERT_EQ(Result->Requests.size(), 2u);
      EXPECT_EQ(Result->Requests[0].IOStatus, windows::StatusUnsuccessful);
      EXPECT_EQ(Result->Requests[1].IOStatus, windows::StatusSuccess);
      EXPECT_LT(messageIndex(*Result, "KMDF PoFx: post register 1\n"),
                messageIndex(*Result, "KMDF PoFx: D0 exit\n"));
      EXPECT_LT(messageIndex(*Result, "KMDF PoFx: D0 exit\n"),
                messageIndex(*Result, "KMDF PoFx: pre unregister 1\n"));
    }
#else
  GTEST_SKIP() << "NEVERD_KMDF_POFX_FIXTURE requires a genuine WDK fixture";
#endif
}
} // namespace
} // namespace neverd::emulation
