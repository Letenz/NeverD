//===- DriverKMDFD3ColdTests.cpp - Genuine cold bus power recovery -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercise genuine WDK cold-power policy, register restoration and wake
/// limits.
///
//===----------------------------------------------------------------------===//

#include "fixtures/driver_kmdf_d3cold_scenario.h"
#include "gtest/gtest.h"

#include "neverd/emulation/DriverSession.h"

#include <array>

namespace neverd::emulation {
namespace {
#ifdef NEVERD_KMDF_PNP_FIXTURE
std::vector<const char *> images() {
  return {
      NEVERD_KMDF_PNP_FIXTURE,
#ifdef NEVERD_KMDF_PNP_CFG_FIXTURE
      NEVERD_KMDF_PNP_CFG_FIXTURE,
#endif
  };
}
uint32_t word(const DriverRequestResult &Request, size_t Index) {
  uint32_t Value = 0;
  for (size_t I = 0; I < sizeof(Value); ++I)
    Value |= uint32_t(Request.Output.at(Index * sizeof(Value) + I)) << (I * 8);
  return Value;
}
void execute(const char *Image, char Mode, bool Default, bool ColdWake,
             bool ExpectedReset, uint64_t LoadAddress, bool Supported = true) {
  SCOPED_TRACE(Image);
  SCOPED_TRACE(Mode);
  SCOPED_TRACE(LoadAddress);
  auto Input = driverOptionsFromScenarioJSON(
      test::kmdfD3ColdScenario(Mode, Default, ColdWake, Supported));
  ASSERT_TRUE(bool(Input)) << llvm::toString(Input.takeError());
  Input->LoadAddress = LoadAddress;
  auto Result = emulateDriver(Image, *Input);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  ASSERT_TRUE(Result->UnloadCompleted);
  const DriverRequestResult *Snapshot = nullptr;
  size_t ScenarioIndex = 0;
  for (const auto &Request : Result->Requests) {
    EXPECT_TRUE(Request.Completed);
    EXPECT_EQ(Request.IOStatus, 0u);
    if (Request.Origin == DriverRequestOrigin::Scenario && ScenarioIndex++ == 3)
      Snapshot = &Request;
  }
  ASSERT_NE(Snapshot, nullptr);
  ASSERT_EQ(Snapshot->Output.size(),
            KmdfPowerColdSnapshotWords * sizeof(uint32_t));
  EXPECT_EQ(word(*Snapshot, 0), 2u);
  EXPECT_EQ(word(*Snapshot, 1), 1u);
  EXPECT_EQ(word(*Snapshot, 5), 1u);
  EXPECT_EQ(word(*Snapshot, 6),
            ExpectedReset ? 0x12345678u : uint32_t(KmdfPowerRegisterMarker));
  EXPECT_EQ(word(*Snapshot, 7), ExpectedReset ? 1u : 0u);
  ASSERT_EQ(Result->PowerPolicyEvents.size(), 2u);
  EXPECT_EQ(Result->PowerPolicyEvents[0].DeviceEpoch, 1u);
  EXPECT_EQ(Result->PowerPolicyEvents[1].DeviceEpoch, 1u);
}
#endif

TEST(DriverKMDFD3Cold, ExplicitAndProviderDefaultRestorePowerOnRegisters) {
#ifdef NEVERD_KMDF_PNP_FIXTURE
  for (const auto *Image : images())
    for (uint64_t Base : {uint64_t(0), uint64_t(0x190000000)}) {
      execute(Image, KmdfPowerColdExplicit, false, true, true, Base);
      execute(Image, KmdfPowerColdDefault, true, true, true, Base);
    }
#else
  GTEST_SKIP() << "NEVERD_KMDF_PNP_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverKMDFD3Cold, ExcludedAndDefaultDisabledKeepHotRegisterContext) {
#ifdef NEVERD_KMDF_PNP_FIXTURE
  for (const auto *Image : images()) {
    execute(Image, KmdfPowerColdExcluded, true, true, false, 0);
    execute(Image, KmdfPowerColdDefaultHot, false, true, false, 0);
  }
#else
  GTEST_SKIP() << "NEVERD_KMDF_PNP_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverKMDFD3Cold, MissingColdWakeOrPlatformSupportKeepsWakeDeviceInD3Hot) {
#ifdef NEVERD_KMDF_PNP_FIXTURE
  for (const auto *Image : images()) {
    execute(Image, KmdfPowerColdWakeUnavailable, false, false, false, 0);
    execute(Image, KmdfPowerColdWakeUnavailable, false, false, false, 0, false);
  }
#else
  GTEST_SKIP() << "NEVERD_KMDF_PNP_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverKMDFD3Cold,
     SystemWakeUsesSleepingCapabilityBeforeSystemIRPCompletes) {
#ifdef NEVERD_KMDF_PNP_FIXTURE
  for (const auto *Image : images()) {
    auto Input = driverOptionsFromScenarioJSON(
        test::kmdfD3ColdScenario(KmdfPowerColdSystemWake, false, false));
    ASSERT_TRUE(bool(Input)) << llvm::toString(Input.takeError());
    auto &Device = Input->PnpDevices[0];
    Device.D3Cold->WakeSx = true;
    Device.RequestedDevicePower[0].Action = DriverPowerAction::Sleep;
    const auto System = [](SystemPowerState State) {
      DriverRequest Request;
      Request.Kind = DriverRequestKind::Power;
      Request.DeviceID = "cold-pdo";
      DriverPowerOperation Operation;
      Operation.Type = DriverPowerType::System;
      Operation.State = uint32_t(State);
      Operation.Action = State == SystemPowerState::Working
                             ? DriverPowerAction::None
                             : DriverPowerAction::Sleep;
      Operation.BusCompletion = {0, 3};
      Request.Power = Operation;
      return Request;
    };
    Input->Requests.insert(Input->Requests.begin() + 2,
                           System(SystemPowerState::Sleeping3));
    Input->Requests[3].PowerPolicyEvents = {
        {0, "cold-pdo", DriverPowerPolicyAction::Wake}};
    Input->Requests.insert(Input->Requests.begin() + 4,
                           System(SystemPowerState::Working));
    auto Result = emulateDriver(Image, *Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    size_t Index = 0;
    const DriverRequestResult *Snapshot = nullptr;
    for (const auto &Request : Result->Requests)
      if (Request.Origin == DriverRequestOrigin::Scenario && Index++ == 5)
        Snapshot = &Request;
    ASSERT_NE(Snapshot, nullptr);
    ASSERT_EQ(Snapshot->Output.size(),
              KmdfPowerColdSnapshotWords * sizeof(uint32_t));
    EXPECT_EQ(word(*Snapshot, 6), 0x12345678u);
    EXPECT_EQ(word(*Snapshot, 7), 1u);
    EXPECT_TRUE(Result->UnloadCompleted);
  }
#else
  GTEST_SKIP() << "NEVERD_KMDF_PNP_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverKMDFD3Cold, PresentDeviceCannotRejectPowerDown) {
#ifdef NEVERD_KMDF_PNP_FIXTURE
  for (const auto *Image : images()) {
    auto Input = driverOptionsFromScenarioJSON(
        test::kmdfD3ColdScenario(KmdfPowerColdExplicit, false, true));
    ASSERT_TRUE(bool(Input)) << llvm::toString(Input.takeError());
    auto &Responses = Input->PnpDevices[0].RequestedDevicePower;
    Responses.resize(1);
    Responses[0].BusCompletion.Status = 0xc0000001;
    Input->Requests[2].PowerPolicyEvents.resize(1);
    Input->Requests[3].Input = {KmdfPowerReadRegister};
    auto Result = emulateDriver(Image, *Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
    EXPECT_NE(Result->Diagnostic.find(
                  "device set-power must not fail on a present device"),
              std::string::npos)
        << Result->Diagnostic;
  }
#else
  GTEST_SKIP() << "NEVERD_KMDF_PNP_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverKMDFD3Cold, SoftwarePowerUpRestoresColdDeviceWithoutWakeCapability) {
#ifdef NEVERD_KMDF_PNP_FIXTURE
  for (const auto *Image : images())
    execute(Image, KmdfPowerColdNoWake, false, false, true, 0);
#else
  GTEST_SKIP() << "NEVERD_KMDF_PNP_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverKMDFD3Cold, SystemSleepPreservesAnAlreadyColdDevice) {
#ifdef NEVERD_KMDF_PNP_FIXTURE
  for (const auto *Image : images())
    for (uint64_t Base : {uint64_t(0), uint64_t(0x190000000)}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Base);
      auto Input = driverOptionsFromScenarioJSON(
          test::kmdfD3ColdScenario(KmdfPowerColdNoWake, false, false));
      ASSERT_TRUE(bool(Input)) << llvm::toString(Input.takeError());
      Input->LoadAddress = Base;
      auto &Responses = Input->PnpDevices.front().RequestedDevicePower;
      auto SleepD3 = Responses.front();
      SleepD3.Action = DriverPowerAction::Sleep;
      Responses.insert(Responses.begin() + 1, SleepD3);
      Input->Requests[2].PowerPolicyEvents.resize(1);
      const auto System = [](SystemPowerState State) {
        DriverRequest Request;
        Request.Kind = DriverRequestKind::Power;
        Request.DeviceID = "cold-pdo";
        DriverPowerOperation Power;
        Power.Type = DriverPowerType::System;
        Power.State = uint32_t(State);
        Power.Action = State == SystemPowerState::Working
                           ? DriverPowerAction::None
                           : DriverPowerAction::Sleep;
        Power.BusCompletion = {0, 3};
        Request.Power = Power;
        return Request;
      };
      const auto Original = Input->Requests;
      Input->Requests = {Original[0], Original[1],
                         Original[2], System(SystemPowerState::Sleeping3),
                         Original[3], System(SystemPowerState::Working),
                         Original[3], Original[4],
                         Original[5], Original[6],
                         Original[7]};
      // Activity during S3 clears the old idle observation; it cannot issue
      // device power until the explicit system resume arrives.
      Input->Requests[4].PowerPolicyEvents = {
          {0, "cold-pdo", DriverPowerPolicyAction::Active}};
      auto Result = emulateDriver(Image, *Input);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
      EXPECT_TRUE(Result->UnloadCompleted);
      ASSERT_EQ(Result->Requests.size(), Input->Requests.size() + 3);
      std::vector<const DriverRequestResult *> Children, Scenarios;
      for (const auto &Request : Result->Requests) {
        EXPECT_TRUE(Request.Completed);
        EXPECT_EQ(Request.IOStatus, 0u);
        if (Request.Origin == DriverRequestOrigin::Scenario)
          Scenarios.push_back(&Request);
        else if (Request.Origin == DriverRequestOrigin::FrameworkPowerPolicy)
          Children.push_back(&Request);
        else
          ADD_FAILURE() << "unexpected wake request without wake policy";
      }
      ASSERT_EQ(Children.size(), 3u);
      const std::array Before{DevicePowerState::D0, DevicePowerState::D3,
                              DevicePowerState::D3};
      const std::array After{DevicePowerState::D3, DevicePowerState::D3,
                             DevicePowerState::D0};
      for (size_t I = 0; I < Children.size(); ++I) {
        const auto &Child = *Children[I];
        EXPECT_EQ(Child.ResponseIndex, I);
        ASSERT_TRUE(Child.Power);
        EXPECT_EQ(Child.Power->State, uint32_t(After[I]));
        EXPECT_EQ(Child.Power->DeviceStateBefore, Before[I]);
        EXPECT_EQ(Child.Power->DeviceStateAfter, After[I]);
        ASSERT_TRUE(Child.Power->BusReceivedAt100ns);
        ASSERT_TRUE(Child.Power->BusCompletedAt100ns);
        EXPECT_EQ(*Child.Power->BusCompletedAt100ns -
                      *Child.Power->BusReceivedAt100ns,
                  Responses[I].BusCompletion.Delay100ns);
        if (I)
          EXPECT_LT(*Children[I - 1]->Power->BusCompletedAt100ns,
                    *Child.Power->BusReceivedAt100ns);
      }
      ASSERT_EQ(Scenarios.size(), Input->Requests.size());
      for (const size_t Index : {size_t(4), size_t(6)}) {
        const auto &Snapshot = *Scenarios[Index];
        ASSERT_EQ(Snapshot.Output.size(),
                  KmdfPowerColdSnapshotWords * sizeof(uint32_t));
        EXPECT_EQ(word(Snapshot, 0), Index == 4 ? 1u : 2u);
        EXPECT_EQ(word(Snapshot, 1), 1u);
        EXPECT_EQ(word(Snapshot, 2), 0u);
        EXPECT_EQ(word(Snapshot, 5), Index == 4 ? 0u : 1u);
        EXPECT_EQ(word(Snapshot, 6), 0x12345678u);
        EXPECT_EQ(word(Snapshot, 7), Index == 4 ? 0u : 1u);
      }
      ASSERT_TRUE(Scenarios[3]->Power);
      EXPECT_EQ(Scenarios[3]->Power->DeviceStateBefore, DevicePowerState::D3);
      EXPECT_EQ(Scenarios[3]->Power->DeviceStateAfter, DevicePowerState::D3);
      EXPECT_EQ(Scenarios[3]->Power->SystemStateAfter,
                SystemPowerState::Sleeping3);
      ASSERT_EQ(Result->PowerPolicyEvents.size(), 2u);
      EXPECT_EQ(Result->PowerPolicyEvents.front().Action,
                DriverPowerPolicyAction::Idle);
      EXPECT_EQ(Result->PowerPolicyEvents.back().Action,
                DriverPowerPolicyAction::Active);
    }
#else
  GTEST_SKIP() << "NEVERD_KMDF_PNP_FIXTURE requires a genuine WDK fixture";
#endif
}
} // namespace
} // namespace neverd::emulation
