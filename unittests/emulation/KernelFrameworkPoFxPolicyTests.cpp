//===- KernelFrameworkPoFxPolicyTests.cpp - Managed idle ownership --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Verify framework-owned component policy and power acknowledgements.
///
//===----------------------------------------------------------------------===//

#include "KernelFrameworkTestSupport.h"

namespace neverd::emulation {
namespace {
using namespace framework_test;
using namespace framework;
namespace policy = power_policy;

class DriverKernelFrameworkPoFxPolicy : public DriverKernelFramework {
protected:
  static constexpr uint64_t PDO = Driver + 0x3000;
  static constexpr uint64_t FDO = Driver + 0x3100;
  static constexpr uint64_t InitSlot = Driver + 0x3200;
  static constexpr uint64_t DeviceSlot = Driver + 0x3208;
  static constexpr uint64_t IdleConfig = Driver + 0x3300;
  static constexpr uint64_t PnpConfig = Driver + 0x3400;
  static constexpr uint64_t Entry = 0x180003000;
  static constexpr uint64_t Exit = 0x180003100;
  uint64_t Device = 0, Now = 0, Packet = Driver + 0x4000;
  DevicePowerState State = DevicePowerState::D0;
  std::vector<std::pair<bool, uint64_t>> ManagedChanges;
  std::vector<DevicePowerState> PowerRequests;
  bool RejectManaged = false;

  void SetUp() override {
    DriverKernelFramework::SetUp();
    KernelFramework::DeviceHost Devices;
    Devices.Create =
        [](llvm::StringRef, uint32_t,
           bool) -> llvm::Expected<KernelFramework::DeviceCreation> {
      return failure("managed policy fixture requires a PnP device");
    };
    Devices.CreatePnp =
        [](uint64_t, llvm::StringRef, uint32_t, uint32_t, bool,
           bool) -> llvm::Expected<KernelFramework::DeviceCreation> {
      return KernelFramework::DeviceCreation{0, FDO};
    };
    Devices.Delete = [](uint64_t) { return llvm::Error::success(); };
    Devices.FinishInitializing = [](uint64_t) {
      return llvm::Error::success();
    };
    Model.setDeviceHost(std::move(Devices));
    bind();
    put(Config + DriverConfigAddDevice, Entry);
    put(Config + DriverConfigFlags, 0, 4);
    createDriver();
    const auto Add = take(Model.beginPnpAddDevice(PDO));
    put(InitSlot, Add.Init);
    put(PnpConfig, PnpPowerCallbacksSize, 4);
    put(PnpConfig + PnpPowerCallbacksFirstOffset, Entry);
    put(PnpConfig + PnpPowerCallbacksFirstOffset + 2 * sizeof(uint64_t), Exit);
    take(invoke(api::WdfDeviceInitSetPnpPowerEventCallbacks,
                {Globals, Add.Init, PnpConfig}));
    EXPECT_EQ(
        take(invoke(api::WdfDeviceCreate, {Globals, InitSlot, 0, DeviceSlot})),
        0u);
    Device = get(DeviceSlot);
    success(Model.finishPnpAddDevice(PDO, Add.Init, 0));

    KernelFramework::PowerPolicyHost Host;
    Host.Now = [this] { return Now; };
    Host.CanWake = [](uint64_t, bool) -> llvm::Expected<bool> { return true; };
    Host.ManagedIdle = [this](uint64_t Wdm, bool Idle,
                              uint64_t Timeout) -> llvm::Error {
      EXPECT_EQ(Wdm, FDO);
      if (RejectManaged)
        return failure("managed policy rejected activity");
      ManagedChanges.emplace_back(Idle, Timeout);
      return llvm::Error::success();
    };
    Host.Request =
        [this](
            uint64_t Wdm, DevicePowerState Target,
            KernelFramework::PowerPolicyHost::RequestMode Mode) -> llvm::Error {
      EXPECT_EQ(Wdm, FDO);
      if (Mode == KernelFramework::PowerPolicyHost::RequestMode::Validate)
        return llvm::Error::success();
      if (auto E = Model.beginPowerPolicyRequest(PDO))
        return E;
      auto Started =
          Model.beginDevicePowerTransition(PDO, ++Packet, State, Target);
      if (!Started)
        return Started.takeError();
      PowerRequests.push_back(Target);
      State = Target;
      return llvm::Error::success();
    };
    Model.setPowerPolicyHost(std::move(Host));
    put(IdleConfig, policy::IdleSize, 4);
    put(IdleConfig + policy::IdleCapabilities, policy::CannotWake, 4);
    put(IdleConfig + policy::IdleDxState, uint32_t(DevicePowerState::D3), 4);
    put(IdleConfig + policy::IdleTimeout, 1, 4);
    put(IdleConfig + policy::IdleUserControl, policy::NoUserControl, 4);
    put(IdleConfig + policy::IdleEnabled, policy::True, 4);
    put(IdleConfig + policy::IdleTimeoutType,
        policy::SystemManagedTimeoutWithHint, 4);
    put(IdleConfig + policy::IdleExcludeD3Cold, policy::True, 4);
  }

  llvm::Expected<uint64_t> configure() {
    return invoke(api::WdfDeviceAssignS0IdleSettings,
                  {Globals, Device, IdleConfig});
  }
  void start() {
    EXPECT_TRUE(take(Model.beginPnpPowerTransition(
        PDO, ++Packet, DevicePnpRequest::Start, 0, 0, 0)));
    const auto Call = callback();
    EXPECT_EQ(Call.PC, Entry);
    take(Model.finishGuestCall(Call.Token, 0));
    ASSERT_TRUE(Model.takePnpCompletion());
  }
  void finishPower(uint64_t PC,
                   std::optional<DevicePowerState> PowerState = {}) {
    const auto Call = callback();
    EXPECT_EQ(Call.PC, PC);
    if (PowerState)
      EXPECT_EQ(Call.Arguments,
                (std::vector<uint64_t>{Device, uint32_t(*PowerState)}));
    take(Model.finishGuestCall(Call.Token, 0));
    const auto Completed = Model.takePnpCompletion();
    ASSERT_TRUE(Completed);
    success(Model.finishPowerPolicyRequest(PDO, Completed->Status, true));
  }
  void idlePowerDown() {
    success(Model.powerPolicyIdle(PDO));
    success(Model.powerPolicyPermission(PDO, true));
    success(Model.processPowerPolicy());
    ASSERT_EQ(PowerRequests, (std::vector{DevicePowerState::D3}));
    finishPower(Exit);
    ASSERT_TRUE(take(Model.powerPolicyDeviceReady(PDO, false)));
  }
};

TEST_F(DriverKernelFrameworkPoFxPolicy,
       InitialStartKeepsAnActiveComponentUntilExplicitIdleObservation) {
  EXPECT_EQ(take(configure()), 0u);
  EXPECT_TRUE(ManagedChanges.empty());
  start();
  ASSERT_EQ(ManagedChanges.size(), 1u);
  EXPECT_EQ(ManagedChanges.back(),
            (std::pair{false, policy::TicksPerMillisecond}));
  EXPECT_FALSE(Model.nextPowerPolicyTime());
  success(Model.powerPolicyIdle(PDO));
  EXPECT_EQ(ManagedChanges.back(),
            (std::pair{true, policy::TicksPerMillisecond}));
  Now = policy::TicksPerMillisecond * 2;
  success(Model.processPowerPolicy());
  EXPECT_TRUE(PowerRequests.empty());
  EXPECT_FALSE(Model.nextPowerPolicyTime());
}

TEST_F(DriverKernelFrameworkPoFxPolicy,
       PoweredDownDeviceDoesNotInventActivityOnSchedulerPolling) {
  EXPECT_EQ(take(configure()), 0u);
  start();
  idlePowerDown();
  ManagedChanges.clear();
  Now = policy::TicksPerMillisecond * 2;
  success(Model.processPowerPolicy());
  success(Model.processPowerPolicy());
  EXPECT_TRUE(ManagedChanges.empty());
  EXPECT_EQ(PowerRequests, (std::vector{DevicePowerState::D3}));
  EXPECT_FALSE(Model.nextPowerPolicyTime());
}

TEST_F(DriverKernelFrameworkPoFxPolicy,
       SleepingDeviceDoesNotReactivateItsIdleComponent) {
  EXPECT_EQ(take(configure()), 0u);
  start();
  success(Model.powerPolicyIdle(PDO));
  success(Model.systemPowerPolicy(PDO, true));
  ManagedChanges.clear();
  success(Model.processPowerPolicy());
  EXPECT_TRUE(ManagedChanges.empty());
  EXPECT_TRUE(PowerRequests.empty());
}

TEST_F(DriverKernelFrameworkPoFxPolicy,
       ActualActivityReactivatesPoFxAndWaitsForRealD0Completion) {
  EXPECT_EQ(take(configure()), 0u);
  start();
  idlePowerDown();
  ManagedChanges.clear();
  success(Model.powerPolicyActive(PDO));
  ASSERT_EQ(ManagedChanges.size(), 1u);
  EXPECT_FALSE(ManagedChanges.back().first);
  success(Model.processPowerPolicy());
  EXPECT_EQ(PowerRequests,
            (std::vector{DevicePowerState::D3, DevicePowerState::D0}));
  EXPECT_FALSE(take(Model.powerPolicyDeviceReady(PDO, true)));
  finishPower(Entry);
  EXPECT_TRUE(take(Model.powerPolicyDeviceReady(PDO, true)));
}

TEST_F(DriverKernelFrameworkPoFxPolicy,
       DisabledIdlePolicyReactivatesPoweredDownDevice) {
  EXPECT_EQ(take(configure()), 0u);
  start();
  idlePowerDown();
  ManagedChanges.clear();
  put(IdleConfig + policy::IdleEnabled, policy::False, 4);
  EXPECT_EQ(take(configure()), 0u);
  ASSERT_EQ(ManagedChanges.size(), 1u);
  EXPECT_FALSE(ManagedChanges.back().first);
  success(Model.processPowerPolicy());
  finishPower(Entry);
  EXPECT_EQ(PowerRequests,
            (std::vector{DevicePowerState::D3, DevicePowerState::D0}));
}

TEST_F(DriverKernelFrameworkPoFxPolicy,
       ExplicitPermissionUsesD2PolicyAndRestoresFromD2) {
  put(IdleConfig + policy::IdleDxState, uint32_t(DevicePowerState::D2), 4);
  EXPECT_EQ(take(configure()), 0u);
  start();
  success(Model.powerPolicyIdle(PDO));
  success(Model.processPowerPolicy());
  EXPECT_TRUE(PowerRequests.empty());
  success(Model.powerPolicyPermission(PDO, true));
  success(Model.processPowerPolicy());
  ASSERT_EQ(PowerRequests, (std::vector{DevicePowerState::D2}));
  EXPECT_FALSE(take(Model.powerPolicyDeviceReady(PDO, false)));
  finishPower(Exit, DevicePowerState::D2);
  EXPECT_TRUE(take(Model.powerPolicyDeviceReady(PDO, false)));
  success(Model.powerPolicyActive(PDO));
  success(Model.processPowerPolicy());
  EXPECT_EQ(PowerRequests,
            (std::vector{DevicePowerState::D2, DevicePowerState::D0}));
  EXPECT_FALSE(take(Model.powerPolicyDeviceReady(PDO, true)));
  finishPower(Entry, DevicePowerState::D2);
  EXPECT_TRUE(take(Model.powerPolicyDeviceReady(PDO, true)));
}

TEST_F(DriverKernelFrameworkPoFxPolicy,
       DisablingBeforePowerDownRevokesEarlierPoFxPermission) {
  EXPECT_EQ(take(configure()), 0u);
  start();
  success(Model.powerPolicyIdle(PDO));
  success(Model.powerPolicyPermission(PDO, true));
  put(IdleConfig + policy::IdleEnabled, policy::False, 4);
  EXPECT_EQ(take(configure()), 0u);
  success(Model.processPowerPolicy());
  EXPECT_TRUE(PowerRequests.empty());
  EXPECT_FALSE(Model.nextPowerPolicyTime());
  expectError(Model.powerPolicyPermission(PDO, true),
              "raced with device activity");
}

TEST_F(DriverKernelFrameworkPoFxPolicy,
       TimeoutTypeCannotChangeBetweenSystemManagedVariants) {
  EXPECT_EQ(take(configure()), 0u);
  put(IdleConfig + policy::IdleTimeoutType, policy::SystemManagedTimeout, 4);
  expectError(configure(), "timeout type cannot change");
  put(IdleConfig + policy::IdleTimeoutType, policy::DriverManagedTimeout, 4);
  expectError(configure(), "timeout type cannot change");
  start();
  EXPECT_EQ(ManagedChanges.back().second, policy::TicksPerMillisecond);
}

TEST_F(DriverKernelFrameworkPoFxPolicy,
       SystemManagedAssignmentMustPrecedeInitialD0Completion) {
  start();
  expectError(configure(), "first D0 entry");
  EXPECT_TRUE(ManagedChanges.empty());
}

TEST_F(DriverKernelFrameworkPoFxPolicy,
       ReassignmentUpdatesHintButPreservesInitialSystemWakeChoice) {
  EXPECT_EQ(take(configure()), 0u);
  start();
  EXPECT_FALSE(take(Model.systemPowerNeedsD0(PDO)));
  put(IdleConfig + policy::IdlePowerUpOnSystemWake, policy::True, 4);
  put(IdleConfig + policy::IdleTimeout, 3, 4);
  EXPECT_EQ(take(configure()), 0u);
  EXPECT_FALSE(take(Model.systemPowerNeedsD0(PDO)));
  success(Model.powerPolicyIdle(PDO));
  EXPECT_EQ(ManagedChanges.back().second, policy::TicksPerMillisecond * 3);
}

TEST_F(DriverKernelFrameworkPoFxPolicy,
       RejectedActivationPreservesIdlePermissionAndReferenceBalance) {
  EXPECT_EQ(take(configure()), 0u);
  start();
  success(Model.powerPolicyIdle(PDO));
  success(Model.powerPolicyPermission(PDO, true));
  RejectManaged = true;
  expectError(Model.powerPolicyActive(PDO), "rejected activity");
  expectError(invoke(api::WdfDeviceStopIdleNoTrack, {Globals, Device, 0}),
              "rejected activity");
  expectError(invoke(api::WdfDeviceResumeIdleNoTrack, {Globals, Device}),
              "matching successful StopIdle");
  RejectManaged = false;
  success(Model.processPowerPolicy());
  EXPECT_EQ(PowerRequests, (std::vector{DevicePowerState::D3}));
  finishPower(Exit);
}
} // namespace
} // namespace neverd::emulation
