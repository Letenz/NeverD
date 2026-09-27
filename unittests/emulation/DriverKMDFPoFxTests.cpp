//===- DriverKMDFPoFxTests.cpp - Genuine framework-owned idle authority --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Execute framework-managed component power policy in genuine drivers.
///
//===----------------------------------------------------------------------===//

#include "fixtures/driver_kmdf_power_policy_test.h"
#include "gtest/gtest.h"

#include "neverd/emulation/DriverSession.h"

namespace neverd::emulation {
namespace {
#ifdef NEVERD_KMDF_PNP_FIXTURE
constexpr llvm::StringLiteral DeviceID = "managed-pdo";
constexpr uint64_t DecisionTime = KmdfPowerTimeout100ns + 1;
constexpr uint64_t ResumeTime = KmdfPowerTimeout100ns + 17;
std::vector<const char *> images() {
  return {
      NEVERD_KMDF_PNP_FIXTURE,
#ifdef NEVERD_KMDF_PNP_CFG_FIXTURE
      NEVERD_KMDF_PNP_CFG_FIXTURE,
#endif
  };
}
DriverRequest pnp(DevicePnpRequest Minor) {
  DriverRequest Request;
  Request.Kind = DriverRequestKind::Pnp;
  Request.DeviceID = DeviceID.str();
  Request.Pnp = DriverPnpOperation{Minor, {0, 0}};
  return Request;
}
DriverRequest file(DriverRequestKind Kind, uint32_t ID = 1) {
  DriverRequest Request;
  Request.Kind = Kind;
  Request.DeviceID = DeviceID.str();
  Request.File = ID;
  if (Kind == DriverRequestKind::DeviceControl) {
    Request.ControlCode = 0x222000;
    Request.Input = {KmdfPowerSnapshot};
    Request.OutputSize = KmdfPowerSnapshotWords * sizeof(uint32_t);
  }
  return Request;
}
DriverPowerOperation power(DevicePowerState State) {
  DriverPowerOperation Operation;
  Operation.Type = DriverPowerType::Device;
  Operation.State = uint32_t(State);
  Operation.BusCompletion = {0, 3};
  return Operation;
}
DriverOptions options(char Mode) {
  DriverOptions Input;
  Input.ServiceName = std::string("NeverDKmdfPower") + Mode + "-";
  Input.Unload = true;
  DriverPnpDevice Device;
  Device.ID = DeviceID.str();
  Device.InitialDevicePower = DevicePowerState::D0;
  Device.InitialSystemPower = SystemPowerState::Working;
  Device.WakeCapabilities = DriverWakeCapabilities{false, false};
  Input.PnpDevices.push_back(std::move(Device));
  Input.Requests = {pnp(DevicePnpRequest::Start),
                    file(DriverRequestKind::Create),
                    file(DriverRequestKind::DeviceControl),
                    file(DriverRequestKind::DeviceControl),
                    file(DriverRequestKind::Cleanup),
                    file(DriverRequestKind::Close),
                    pnp(DevicePnpRequest::QueryRemove),
                    pnp(DevicePnpRequest::Remove)};
  return Input;
}
void decision(DriverRequest &Request) {
  Request.PowerPolicyEvents = {
      {0, DeviceID.str(), DriverPowerPolicyAction::Idle},
      {DecisionTime, DeviceID.str(), DriverPowerPolicyAction::PowerNotRequired},
      {ResumeTime, DeviceID.str(), DriverPowerPolicyAction::Active}};
}
void snapshot(const DriverResult &Result, size_t ExpectedIndex,
              uint32_t Entries, uint32_t Exits) {
  size_t Index = 0;
  const DriverRequestResult *Snapshot = nullptr;
  for (const auto &Request : Result.Requests) {
    EXPECT_TRUE(Request.Completed);
    EXPECT_EQ(Request.IOStatus, 0u);
    if (Request.Origin == DriverRequestOrigin::Scenario &&
        Index++ == ExpectedIndex)
      Snapshot = &Request;
  }
  ASSERT_NE(Snapshot, nullptr);
  ASSERT_EQ(Snapshot->Output.size(), KmdfPowerSnapshotWords * sizeof(uint32_t));
  const auto Word = [&](size_t Offset) {
    uint32_t Value = 0;
    for (size_t I = 0; I < sizeof(Value); ++I)
      Value |= uint32_t(Snapshot->Output[Offset * sizeof(Value) + I])
               << (8 * I);
    return Value;
  };
  EXPECT_EQ(Word(0), Entries);
  EXPECT_EQ(Word(1), Exits);
  EXPECT_EQ(Word(2), 0u);
  EXPECT_EQ(Word(3), 0u);
  EXPECT_EQ(Word(4), 0u);
  EXPECT_EQ(Word(5), 1u);
}
#endif

TEST(DriverKMDFPoFx, IdleAloneDoesNotInventAnOperatingSystemPowerDecision) {
#ifdef NEVERD_KMDF_PNP_FIXTURE
  for (const auto *Image : images())
    for (char Mode :
         {char(KmdfPowerSystemManaged), char(KmdfPowerSystemManagedHint)}) {
      auto Input = options(Mode);
      Input.Requests[2].PowerPolicyEvents = {
          {0, DeviceID.str(), DriverPowerPolicyAction::Idle},
          {3 * KmdfPowerTimeout100ns, DeviceID.str(),
           DriverPowerPolicyAction::Active}};
      auto Result = emulateDriver(Image, Input);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
      snapshot(*Result, 3, 1, 0);
      EXPECT_EQ(Result->Requests.size(), Input.Requests.size());
      EXPECT_TRUE(Result->UnloadCompleted);
    }
#else
  GTEST_SKIP() << "NEVERD_KMDF_PNP_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverKMDFPoFx, ExplicitDecisionAndActivityCompleteRealDevicePowerIRPs) {
#ifdef NEVERD_KMDF_PNP_FIXTURE
  for (const auto *Image : images())
    for (char Mode :
         {char(KmdfPowerSystemManaged), char(KmdfPowerSystemManagedHint)})
      for (uint64_t Base : {uint64_t(0), uint64_t(0x190000000)}) {
        auto Input = options(Mode);
        Input.LoadAddress = Base;
        Input.PnpDevices[0].RequestedDevicePower = {
            power(DevicePowerState::D3), power(DevicePowerState::D0)};
        decision(Input.Requests[2]);
        auto Result = emulateDriver(Image, Input);
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        ASSERT_EQ(Result->Stop, DriverStopReason::Returned)
            << Result->Diagnostic;
        snapshot(*Result, 3, 2, 1);
        EXPECT_EQ(Result->Requests.size(), Input.Requests.size() + 2);
        EXPECT_TRUE(Result->UnloadCompleted);
      }
#else
  GTEST_SKIP() << "NEVERD_KMDF_PNP_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverKMDFPoFx, StopReleasesRegistrationAndRestartGetsAFreshPowerEpoch) {
#ifdef NEVERD_KMDF_PNP_FIXTURE
  for (const auto *Image : images())
    for (char Mode :
         {char(KmdfPowerSystemManaged), char(KmdfPowerSystemManagedHint)}) {
      auto Input = options(Mode);
      Input.PnpDevices[0].RequestedDevicePower = {
          power(DevicePowerState::D3), power(DevicePowerState::D0),
          power(DevicePowerState::D3), power(DevicePowerState::D0)};
      decision(Input.Requests[2]);
      Input.Requests.resize(6);
      Input.Requests.push_back(pnp(DevicePnpRequest::QueryStop));
      Input.Requests.push_back(pnp(DevicePnpRequest::Stop));
      Input.Requests.push_back(pnp(DevicePnpRequest::Start));
      Input.Requests.push_back(file(DriverRequestKind::Create, 2));
      auto Activity = file(DriverRequestKind::DeviceControl, 2);
      decision(Activity);
      Input.Requests.push_back(std::move(Activity));
      Input.Requests.push_back(file(DriverRequestKind::DeviceControl, 2));
      Input.Requests.push_back(file(DriverRequestKind::Cleanup, 2));
      Input.Requests.push_back(file(DriverRequestKind::Close, 2));
      Input.Requests.push_back(pnp(DevicePnpRequest::QueryRemove));
      Input.Requests.push_back(pnp(DevicePnpRequest::Remove));
      auto Result = emulateDriver(Image, Input);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
      snapshot(*Result, 11, 4, 3);
      ASSERT_EQ(Result->PowerPolicyEvents.size(), 6u);
      EXPECT_EQ(Result->PowerPolicyEvents.front().DeviceEpoch, 1u);
      EXPECT_EQ(Result->PowerPolicyEvents.back().DeviceEpoch, 2u);
      EXPECT_TRUE(Result->UnloadCompleted);
    }
#else
  GTEST_SKIP() << "NEVERD_KMDF_PNP_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverKMDFPoFx, RemovalReleasesAnIdleRegistrationWithoutPoweringDown) {
#ifdef NEVERD_KMDF_PNP_FIXTURE
  auto Input = options(KmdfPowerSystemManagedHint);
  Input.Requests[2].PowerPolicyEvents = {
      {0, DeviceID.str(), DriverPowerPolicyAction::Idle}};
  auto Result = emulateDriver(NEVERD_KMDF_PNP_FIXTURE, Input);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  snapshot(*Result, 3, 1, 0);
  EXPECT_EQ(Result->Requests.size(), Input.Requests.size());
  EXPECT_TRUE(Result->UnloadCompleted);
#else
  GTEST_SKIP() << "NEVERD_KMDF_PNP_FIXTURE requires a genuine WDK fixture";
#endif
}
} // namespace
} // namespace neverd::emulation
