//===- KernelFrameworkChildWakeTests.cpp - Provider child wake ownership -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercise real framework transitions with retained provider wake packets,
/// including recursive propagation, START epochs and parent cancellation.
///
//===----------------------------------------------------------------------===//

#include "KernelFrameworkTestSupport.h"

#include <array>

namespace neverd::emulation {
namespace {
using namespace framework_test;
using namespace framework;
namespace policy = power_policy;

class DriverKernelFrameworkChildWake : public DriverKernelFramework {
protected:
  static constexpr uint64_t FirstPDO = Driver + 0x3000;
  static constexpr uint64_t ProviderStride = 0x100;
  static constexpr uint64_t FDOOffset = 0x80;
  static constexpr uint64_t InitSlot = Driver + 0x4000;
  static constexpr uint64_t DeviceSlot = InitSlot + sizeof(uint64_t);
  static constexpr uint64_t PolicyConfig = Driver + 0x4100;
  static constexpr uint64_t PnpConfig = Driver + 0x4200;
  static constexpr uint64_t WakeConfig = Driver + 0x4300;
  static constexpr uint64_t Entry = 0x180003000;
  static constexpr uint64_t Exit = 0x180003100;
  static constexpr uint64_t Arm = 0x180003200;
  static constexpr uint64_t Disarm = 0x180003300;
  static constexpr uint64_t Triggered = 0x180003400;
  std::array<uint64_t, 4> Device{};
  std::map<uint64_t, std::vector<uint64_t>> Children;
  std::set<uint64_t> Retained;
  std::vector<KernelFramework::GuestCall> Calls;
  std::vector<std::vector<uint64_t>> Batches;
  std::vector<uint64_t> Cancelled;
  uint64_t Packet = Driver + 0x5000;
  uint64_t FailArmDevice = 0;
  uint64_t RejectWakeDevice = 0;
  uint64_t RejectCancelDevice = 0;

  static uint64_t pdo(unsigned Index) {
    return FirstPDO + Index * ProviderStride;
  }
  static uint64_t fdo(unsigned Index) { return pdo(Index) + FDOOffset; }

  void SetUp() override {
    DriverKernelFramework::SetUp();
    KernelFramework::DeviceHost Host;
    Host.Create = [](llvm::StringRef, uint32_t,
                     bool) -> llvm::Expected<KernelFramework::DeviceCreation> {
      return failure("only PnP creation is allowed");
    };
    Host.CreatePnp =
        [](uint64_t PDO, llvm::StringRef, uint32_t, uint32_t, bool,
           bool) -> llvm::Expected<KernelFramework::DeviceCreation> {
      return KernelFramework::DeviceCreation{0, PDO + FDOOffset};
    };
    Host.Delete = [](uint64_t) { return llvm::Error::success(); };
    Host.FinishInitializing = [](uint64_t) { return llvm::Error::success(); };
    Model.setDeviceHost(std::move(Host));
    KernelFramework::PowerPolicyHost Policy;
    Policy.Now = [] { return 0; };
    Policy.CanWake = [](uint64_t, bool) -> llvm::Expected<bool> {
      return true;
    };
    Policy.ArmWake = [this](uint64_t Wdm, bool Sleeping) {
      EXPECT_TRUE(Sleeping);
      EXPECT_TRUE(Retained.insert(Wdm).second);
      return llvm::Error::success();
    };
    Policy.FinishWake = [this](uint64_t Wdm, bool Trigger) {
      EXPECT_FALSE(Trigger);
      if (Retained.erase(Wdm))
        Cancelled.push_back(Wdm);
      return llvm::Error::success();
    };
    Policy.CancelWakes =
        [this](llvm::ArrayRef<uint64_t> Devices) -> llvm::Error {
      for (uint64_t Wdm : Devices)
        if (Wdm == RejectCancelDevice)
          return failure("retained cancellation batch rejected");
      for (uint64_t Wdm : Devices)
        if (Retained.erase(Wdm))
          Cancelled.push_back(Wdm);
      return llvm::Error::success();
    };
    Policy.Children =
        [this](uint64_t PDO) -> llvm::Expected<std::vector<uint64_t>> {
      return Children[PDO];
    };
    Policy.CompleteWakes =
        [this](uint64_t Source,
               llvm::ArrayRef<uint64_t> Devices) -> llvm::Error {
      EXPECT_EQ(Source, Devices.front());
      for (uint64_t Wdm : Devices)
        if (Wdm == RejectWakeDevice || !Retained.contains(Wdm))
          return failure("retained wake batch rejected");
      Batches.emplace_back(Devices.begin(), Devices.end());
      for (uint64_t Wdm : Devices)
        Retained.erase(Wdm);
      return llvm::Error::success();
    };
    Model.setPowerPolicyHost(std::move(Policy));
    bind();
    put(Config + DriverConfigAddDevice, Entry);
    put(Config + DriverConfigFlags, 0, 4);
    createDriver();
    for (unsigned I = 0; I < Device.size(); ++I) {
      const auto Add = take(Model.beginPnpAddDevice(pdo(I)));
      put(InitSlot, Add.Init);
      put(PolicyConfig, policy::CallbacksSize, 4);
      put(PolicyConfig + policy::CallbacksFirst + 4 * sizeof(uint64_t), Disarm);
      put(PolicyConfig + policy::CallbacksFirst + 5 * sizeof(uint64_t),
          Triggered);
      put(PolicyConfig + policy::CallbacksFirst + 6 * sizeof(uint64_t), Arm);
      take(invoke(api::WdfDeviceInitSetPowerPolicyEventCallbacks,
                  {Globals, Add.Init, PolicyConfig}));
      put(PnpConfig, PnpPowerCallbacksSize, 4);
      put(PnpConfig + PnpPowerCallbacksFirstOffset, Entry);
      put(PnpConfig + PnpPowerCallbacksFirstOffset + 2 * sizeof(uint64_t),
          Exit);
      take(invoke(api::WdfDeviceInitSetPnpPowerEventCallbacks,
                  {Globals, Add.Init, PnpConfig}));
      EXPECT_EQ(take(invoke(api::WdfDeviceCreate,
                            {Globals, InitSlot, 0, DeviceSlot})),
                0u);
      Device[I] = get(DeviceSlot);
      success(Model.finishPnpAddDevice(pdo(I), Add.Init, 0));
    }
    Children[pdo(0)] = {pdo(1), pdo(3)};
    Children[pdo(1)] = {pdo(2)};
  }
  void configure(unsigned Index, bool Enabled, bool ArmChildren = false,
                 bool Propagate = false) {
    put(WakeConfig, policy::WakeSize, 4);
    put(WakeConfig + policy::WakeDxState, uint32_t(DevicePowerState::D3), 4);
    put(WakeConfig + policy::WakeUserControl, policy::NoUserControl, 4);
    put(WakeConfig + policy::WakeEnabled, Enabled, 4);
    put(WakeConfig + policy::WakeChildren, ArmChildren, 1);
    put(WakeConfig + policy::WakePropagate, Propagate, 1);
    EXPECT_EQ(take(invoke(api::WdfDeviceAssignSxWakeSettings,
                          {Globals, Device[Index], WakeConfig})),
              windows::StatusSuccess);
  }
  void drainTransition() {
    while (auto Call = Model.takeGuestCall()) {
      Calls.push_back(*Call);
      ASSERT_LT(Calls.size(), 128u);
      const bool Fail =
          Call->PC == Arm && Call->Arguments.front() == FailArmDevice;
      take(Model.finishGuestCall(Call->Token,
                                 Fail ? windows::StatusUnsuccessful : 0));
    }
    EXPECT_TRUE(Model.takePnpCompletion());
  }
  void start(unsigned Index) {
    EXPECT_TRUE(take(Model.beginPnpPowerTransition(
        pdo(Index), ++Packet, DevicePnpRequest::Start, 0, 0, 0)));
    drainTransition();
  }
  void sleep(unsigned Index) {
    success(Model.systemPowerPolicy(pdo(Index), true));
    EXPECT_TRUE(take(Model.beginDevicePowerTransition(
        pdo(Index), ++Packet, DevicePowerState::D0, DevicePowerState::D3)));
    drainTransition();
  }
  void resume(unsigned Index) {
    success(Model.systemPowerPolicy(pdo(Index), false));
    EXPECT_TRUE(take(Model.beginDevicePowerTransition(
        pdo(Index), ++Packet, DevicePowerState::D3, DevicePowerState::D0)));
    drainTransition();
  }
  void stop(unsigned Index) {
    if (take(Model.beginPnpPowerTransition(pdo(Index), ++Packet,
                                           DevicePnpRequest::Stop, 0, 0, 0)))
      drainTransition();
  }
  unsigned count(uint64_t PC, unsigned Index) const {
    return std::count_if(Calls.begin(), Calls.end(), [&](const auto &Call) {
      return Call.PC == PC && Call.Arguments.front() == Device[Index];
    });
  }
  std::vector<uint64_t> armReasons(unsigned Index) const {
    for (auto I = Calls.rbegin(); I != Calls.rend(); ++I)
      if (I->PC == Arm && I->Arguments.front() == Device[Index])
        return I->Arguments;
    return {};
  }
};

TEST_F(DriverKernelFrameworkChildWake,
       IndependentFlagsPreserveCallbackReasons) {
  configure(0, false, true, true);
  configure(1, true);
  configure(3, false);
  for (unsigned I : {0, 1, 3})
    start(I);
  sleep(1);
  sleep(3);
  sleep(0);
  EXPECT_EQ(armReasons(0), (std::vector<uint64_t>{Device[0], 0, 1}));
  EXPECT_EQ(armReasons(1), (std::vector<uint64_t>{Device[1], 1, 0}));
  EXPECT_EQ(count(Arm, 3), 0u);
  success(Model.powerPolicyWake(pdo(0)));
  ASSERT_EQ(Batches.size(), 1u);
  EXPECT_EQ(Batches.back(), (std::vector<uint64_t>{fdo(0), fdo(1)}));
  EXPECT_FALSE(Retained.contains(fdo(1)));
  EXPECT_EQ(count(Triggered, 1), 0u);
  resume(0);
  resume(1);
  EXPECT_EQ(count(Triggered, 0), 1u);
  EXPECT_EQ(count(Triggered, 1), 1u);
  EXPECT_EQ(count(Disarm, 0), 1u);
  EXPECT_EQ(count(Disarm, 1), 1u);
}

TEST_F(DriverKernelFrameworkChildWake, OwnAndChildReasonsRemainIndependent) {
  configure(0, true, true, false);
  configure(1, true);
  start(0);
  start(1);
  sleep(1);
  sleep(0);
  EXPECT_EQ(armReasons(0), (std::vector<uint64_t>{Device[0], 1, 1}));
  success(Model.powerPolicyWake(pdo(0)));
  EXPECT_EQ(Batches.back(), (std::vector<uint64_t>{fdo(0)}));
  EXPECT_TRUE(Retained.contains(fdo(1)));
  resume(0);
  resume(1);
  EXPECT_EQ(count(Triggered, 1), 0u);
  EXPECT_EQ(count(Disarm, 1), 1u);
}

TEST_F(DriverKernelFrameworkChildWake, PropagationDoesNotEnableChildArmReason) {
  configure(0, true, false, true);
  configure(1, true);
  start(0);
  start(1);
  sleep(1);
  sleep(0);
  EXPECT_EQ(armReasons(0), (std::vector<uint64_t>{Device[0], 1, 0}));
  success(Model.powerPolicyWake(pdo(0)));
  EXPECT_EQ(Batches.back(), (std::vector<uint64_t>{fdo(0), fdo(1)}));
}

TEST_F(DriverKernelFrameworkChildWake, RecursiveWakePreflightsWholeBatch) {
  configure(0, false, true, true);
  configure(1, false, true, true);
  configure(2, true);
  for (unsigned I : {0, 1, 2})
    start(I);
  for (unsigned I : {2, 1, 0})
    sleep(I);
  RejectWakeDevice = fdo(2);
  expectError(Model.powerPolicyWake(pdo(0)), "batch rejected");
  EXPECT_TRUE(Batches.empty());
  EXPECT_EQ(Retained.size(), 3u);
  RejectWakeDevice = 0;
  success(Model.powerPolicyWake(pdo(0)));
  ASSERT_EQ(Batches.size(), 1u);
  EXPECT_EQ(Batches.back(), (std::vector<uint64_t>{fdo(0), fdo(1), fdo(2)}));
  EXPECT_TRUE(Retained.empty());
  expectError(Model.powerPolicyWake(pdo(1)), "successfully armed");
  for (unsigned I : {0, 1, 2})
    resume(I);
  for (unsigned I : {0, 1, 2}) {
    EXPECT_EQ(count(Triggered, I), 1u);
    EXPECT_EQ(count(Disarm, I), 1u);
  }
}

TEST_F(DriverKernelFrameworkChildWake,
       IntermediatePropagationOptOutStopsFanout) {
  configure(0, true, true, true);
  configure(1, false, true, false);
  configure(2, true);
  for (unsigned I : {0, 1, 2})
    start(I);
  for (unsigned I : {2, 1, 0})
    sleep(I);
  success(Model.powerPolicyWake(pdo(0)));
  EXPECT_EQ(Batches.back(), (std::vector<uint64_t>{fdo(0), fdo(1)}));
  EXPECT_TRUE(Retained.contains(fdo(2)));
}

TEST_F(DriverKernelFrameworkChildWake,
       LastChildCancellationDisarmsAncestorsOnce) {
  configure(0, false, true, true);
  configure(1, false, true, true);
  configure(2, true);
  for (unsigned I : {0, 1, 2})
    start(I);
  for (unsigned I : {2, 1, 0})
    sleep(I);
  stop(2);
  EXPECT_TRUE(Retained.empty());
  EXPECT_EQ(Cancelled, (std::vector<uint64_t>{fdo(2), fdo(1), fdo(0)}));
  for (unsigned I : {0, 1, 2}) {
    EXPECT_EQ(count(Triggered, I), 0u);
    EXPECT_EQ(count(Disarm, I), 1u);
  }
  expectError(Model.powerPolicyWake(pdo(0)), "successfully armed");
  resume(0);
  resume(1);
  EXPECT_EQ(count(Disarm, 0), 1u);
  EXPECT_EQ(count(Disarm, 1), 1u);
}

TEST_F(DriverKernelFrameworkChildWake, CancellationPreservesOtherArmReasons) {
  configure(0, false, true, true);
  configure(1, true);
  configure(3, true);
  for (unsigned I : {0, 1, 3})
    start(I);
  for (unsigned I : {1, 3, 0})
    sleep(I);
  stop(1);
  EXPECT_TRUE(Retained.contains(fdo(0)));
  EXPECT_EQ(count(Disarm, 0), 0u);
  success(Model.powerPolicyWake(pdo(0)));
  EXPECT_EQ(Batches.back(), (std::vector<uint64_t>{fdo(0), fdo(3)}));
}

TEST_F(DriverKernelFrameworkChildWake, OwnWakeSurvivesLastChildCancellation) {
  configure(0, true, true, true);
  configure(1, true);
  start(0);
  start(1);
  sleep(1);
  sleep(0);
  stop(1);
  EXPECT_TRUE(Retained.contains(fdo(0)));
  success(Model.powerPolicyWake(pdo(0)));
  EXPECT_EQ(Batches.back(), (std::vector<uint64_t>{fdo(0)}));
}

TEST_F(DriverKernelFrameworkChildWake, FailedChildArmNeverBecomesParentReason) {
  configure(0, false, true, true);
  configure(1, true);
  start(0);
  start(1);
  FailArmDevice = Device[1];
  sleep(1);
  sleep(0);
  EXPECT_TRUE(Retained.empty());
  EXPECT_EQ(count(Arm, 0), 0u);
  EXPECT_EQ(count(Disarm, 1), 1u);
}

TEST_F(DriverKernelFrameworkChildWake, FailedParentArmLeavesChildUntriggered) {
  configure(0, false, true, true);
  configure(1, true);
  start(0);
  start(1);
  sleep(1);
  FailArmDevice = Device[0];
  sleep(0);
  EXPECT_FALSE(Retained.contains(fdo(0)));
  EXPECT_TRUE(Retained.contains(fdo(1)));
  EXPECT_EQ(count(Disarm, 0), 1u);
  expectError(Model.powerPolicyWake(pdo(0)), "successfully armed");
  success(Model.powerPolicyWake(pdo(1)));
  EXPECT_EQ(Batches.back(), (std::vector<uint64_t>{fdo(1)}));
}

TEST_F(DriverKernelFrameworkChildWake,
       LateEnrollmentFailsBeforeRetainingPacket) {
  configure(0, false, true, true);
  configure(1, true);
  start(0);
  start(1);
  sleep(0);
  success(Model.systemPowerPolicy(pdo(1), true));
  expectError(Model.beginDevicePowerTransition(
                  pdo(1), ++Packet, DevicePowerState::D0, DevicePowerState::D3),
              "late child enrollment");
  EXPECT_TRUE(Retained.empty());
  EXPECT_FALSE(Model.hasPendingGuestCall());
  resume(0);
  sleep(1);
  sleep(0);
  EXPECT_EQ(armReasons(0), (std::vector<uint64_t>{Device[0], 0, 1}));
}

TEST_F(DriverKernelFrameworkChildWake,
       RestartDoesNotInheritCapturedChildEpoch) {
  configure(0, true, true, true);
  configure(1, true);
  start(0);
  start(1);
  sleep(1);
  sleep(0);
  stop(1);
  start(1);
  EXPECT_EQ(take(Model.powerPolicyEpoch(pdo(1))), 2u);
  success(Model.powerPolicyWake(pdo(0)));
  EXPECT_EQ(Batches.back(), (std::vector<uint64_t>{fdo(0)}));
  EXPECT_EQ(count(Triggered, 1), 0u);
}

TEST_F(DriverKernelFrameworkChildWake,
       HeldParentArmRejectsConcurrentEnrollment) {
  configure(0, true, true, true);
  configure(1, true);
  start(0);
  start(1);
  success(Model.systemPowerPolicy(pdo(0), true));
  EXPECT_TRUE(take(Model.beginDevicePowerTransition(
      pdo(0), ++Packet, DevicePowerState::D0, DevicePowerState::D3)));
  const auto Call = callback();
  ASSERT_EQ(Call.PC, Arm);
  EXPECT_EQ(Call.Arguments, (std::vector<uint64_t>{Device[0], 1, 0}));
  success(Model.systemPowerPolicy(pdo(1), true));
  expectError(Model.beginDevicePowerTransition(
                  pdo(1), ++Packet, DevicePowerState::D0, DevicePowerState::D3),
              "another framework callback");
  EXPECT_EQ(Retained, (std::set<uint64_t>{fdo(0)}));
  finish(Call);
  drainTransition();
}

TEST_F(DriverKernelFrameworkChildWake,
       CancellationFailurePreservesParentSnapshot) {
  configure(0, false, true, true);
  configure(1, true);
  start(0);
  start(1);
  sleep(1);
  sleep(0);
  RejectCancelDevice = fdo(0);
  expectError(Model.beginPnpPowerTransition(pdo(1), ++Packet,
                                            DevicePnpRequest::Stop, 0, 0, 0),
              "cancellation batch rejected");
  EXPECT_TRUE(Cancelled.empty());
  EXPECT_EQ(Retained, (std::set<uint64_t>{fdo(0), fdo(1)}));
  // The host rejects before either packet or dependency is retired. A wake
  // can still observe both obligations at this explicit failure boundary.
  success(Model.powerPolicyWake(pdo(0)));
  EXPECT_EQ(Batches.back(), (std::vector<uint64_t>{fdo(0), fdo(1)}));
}

} // namespace
} // namespace neverd::emulation
