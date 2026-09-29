//===- KernelFrameworkPowerPolicyTests.cpp - Idle and wake ownership -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "KernelFrameworkTestSupport.h"
#include "os/windows/DriverScenario.h"

namespace neverd::emulation {
namespace {
using namespace framework_test;
using namespace framework;
namespace policy = power_policy;

class DriverKernelFrameworkPowerPolicy : public DriverKernelFramework {
protected:
  static constexpr uint64_t PDO = Driver + 0x3000;
  static constexpr uint64_t FDO = Driver + 0x3100;
  static constexpr uint64_t InitSlot = Driver + 0x3200;
  static constexpr uint64_t DeviceSlot = Driver + 0x3208;
  static constexpr uint64_t IdleConfig = Driver + 0x3300;
  static constexpr uint64_t WakeConfig = Driver + 0x3400;
  static constexpr uint64_t PolicyConfig = Driver + 0x3500;
  static constexpr uint64_t PnpConfig = Driver + 0x3600;
  static constexpr uint64_t Entry = 0x180003000;
  static constexpr uint64_t Exit = 0x180003100;
  static constexpr uint64_t Arm = 0x180003200;
  static constexpr uint64_t Disarm = 0x180003300;
  static constexpr uint64_t Triggered = 0x180003400;
  uint64_t Device = 0, Now = 0, Packet = Driver + 0x4000;
  DevicePowerState State = DevicePowerState::D0;
  std::vector<DevicePowerState> Requests;
  unsigned WaitWakeArms = 0, WaitWakeCompletions = 0;
  bool WaitWakePending = false;
  bool RejectPowerRequest = false;

  void SetUp() override {
    DriverKernelFramework::SetUp();
    KernelFramework::DeviceHost Host;
    Host.Create = [](llvm::StringRef, uint32_t,
                     bool) -> llvm::Expected<KernelFramework::DeviceCreation> {
      return failure("only PnP creation is allowed");
    };
    Host.CreatePnp =
        [](uint64_t, llvm::StringRef, uint32_t, uint32_t, bool,
           bool) -> llvm::Expected<KernelFramework::DeviceCreation> {
      return KernelFramework::DeviceCreation{0, FDO};
    };
    Host.Delete = [](uint64_t) { return llvm::Error::success(); };
    Host.FinishInitializing = [](uint64_t) { return llvm::Error::success(); };
    Model.setDeviceHost(std::move(Host));
    bind();
    put(Config + DriverConfigAddDevice, Entry);
    put(Config + DriverConfigFlags, 0, 4);
    createDriver();
    const auto Add = take(Model.beginPnpAddDevice(PDO));
    put(InitSlot, Add.Init);
    put(PolicyConfig, policy::CallbacksSize, 4);
    put(PolicyConfig + policy::CallbacksFirst, Arm);
    put(PolicyConfig + policy::CallbacksFirst + sizeof(uint64_t), Disarm);
    put(PolicyConfig + policy::CallbacksFirst + 2 * sizeof(uint64_t),
        Triggered);
    put(PolicyConfig + policy::CallbacksFirst + 3 * sizeof(uint64_t), Arm);
    put(PolicyConfig + policy::CallbacksFirst + 4 * sizeof(uint64_t), Disarm);
    put(PolicyConfig + policy::CallbacksFirst + 5 * sizeof(uint64_t),
        Triggered);
    take(invoke(api::WdfDeviceInitSetPowerPolicyEventCallbacks,
                {Globals, Add.Init, PolicyConfig}));
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
    KernelFramework::PowerPolicyHost Policy;
    Policy.Now = [this] { return Now; };
    Policy.CanWake = [](uint64_t, bool) -> llvm::Expected<bool> {
      return true;
    };
    Policy.Request =
        [this](
            uint64_t Wdm, DevicePowerState Target,
            KernelFramework::PowerPolicyHost::RequestMode Mode) -> llvm::Error {
      EXPECT_EQ(Wdm, FDO);
      if (RejectPowerRequest)
        return failure("power response unavailable");
      if (Mode == KernelFramework::PowerPolicyHost::RequestMode::Validate)
        return llvm::Error::success();
      auto Started =
          Model.beginDevicePowerTransition(PDO, ++Packet, State, Target);
      if (!Started)
        return Started.takeError();
      Requests.push_back(Target);
      State = Target;
      return llvm::Error::success();
    };
    Policy.ArmWake = [this](uint64_t Wdm, bool) -> llvm::Error {
      EXPECT_EQ(Wdm, FDO);
      EXPECT_FALSE(WaitWakePending);
      WaitWakePending = true;
      ++WaitWakeArms;
      return llvm::Error::success();
    };
    Policy.FinishWake = [this](uint64_t Wdm, bool) -> llvm::Error {
      EXPECT_EQ(Wdm, FDO);
      if (WaitWakePending) {
        WaitWakePending = false;
        ++WaitWakeCompletions;
      }
      return llvm::Error::success();
    };
    Model.setPowerPolicyHost(std::move(Policy));
    idleSettings();
  }
  void idleSettings(bool CanWake = true) {
    put(IdleConfig, policy::IdleSize, 4);
    put(IdleConfig + policy::IdleCapabilities,
        CanWake ? policy::CanWake : policy::CannotWake, 4);
    put(IdleConfig + policy::IdleDxState, uint32_t(DevicePowerState::D3), 4);
    put(IdleConfig + policy::IdleTimeout, 1, 4);
    put(IdleConfig + policy::IdleUserControl, policy::NoUserControl, 4);
    put(IdleConfig + policy::IdleEnabled, policy::True, 4);
    put(IdleConfig + policy::IdlePowerUpOnSystemWake, policy::True, 4);
    put(IdleConfig + policy::IdleTimeoutType, policy::DriverManagedTimeout, 4);
    put(IdleConfig + policy::IdleExcludeD3Cold, policy::True, 4);
  }
  void configure() {
    EXPECT_EQ(take(invoke(api::WdfDeviceAssignS0IdleSettings,
                          {Globals, Device, IdleConfig})),
              0u);
  }
  void expectCall(uint64_t PC, uint32_t Status = 0,
                  std::optional<DevicePowerState> PowerState = {}) {
    const auto Call = callback();
    EXPECT_EQ(Call.PC, PC);
    EXPECT_EQ(Call.Arguments.front(), Device);
    if (PowerState)
      EXPECT_EQ(Call.Arguments,
                (std::vector<uint64_t>{Device, uint32_t(*PowerState)}));
    take(Model.finishGuestCall(Call.Token, Status));
    if (PC == Arm) {
      auto Completed = Model.takePnpCompletion();
      ASSERT_TRUE(Completed);
      ASSERT_EQ(Completed->IdlePolicyDevice, Device);
      success(Model.finishIdlePowerDown(Device, Completed->Status));
    }
  }
  void start() {
    EXPECT_TRUE(take(Model.beginPnpPowerTransition(
        PDO, ++Packet, DevicePnpRequest::Start, 0, 0, 0)));
    expectCall(Entry);
    ASSERT_TRUE(Model.takePnpCompletion());
  }
  void wakeSettings(DevicePowerState State, bool Enabled = true) {
    put(WakeConfig, policy::WakeSize, 4);
    put(WakeConfig + policy::WakeDxState, uint32_t(State), 4);
    put(WakeConfig + policy::WakeUserControl, policy::NoUserControl, 4);
    put(WakeConfig + policy::WakeEnabled, Enabled, 4);
    put(WakeConfig + policy::WakeChildren, 0, 1);
    put(WakeConfig + policy::WakePropagate, 0, 1);
    EXPECT_EQ(take(invoke(api::WdfDeviceAssignSxWakeSettings,
                          {Globals, Device, WakeConfig})),
              windows::StatusSuccess);
  }
};

TEST_F(DriverKernelFrameworkPowerPolicy, WakeFollowsRealArmAndD0Entry) {
  configure();
  start();
  expectError(Model.powerPolicyWake(PDO), "successfully armed");
  success(Model.powerPolicyIdle(PDO));
  EXPECT_EQ(Model.nextPowerPolicyTime(), policy::TicksPerMillisecond);
  Now = policy::TicksPerMillisecond - 1;
  success(Model.processPowerPolicy());
  EXPECT_TRUE(Requests.empty());
  ++Now;
  success(Model.processPowerPolicy());
  EXPECT_TRUE(Requests.empty());
  EXPECT_TRUE(WaitWakePending);
  expectCall(Arm);
  ASSERT_EQ(Requests, (std::vector{DevicePowerState::D3}));
  expectCall(Exit);
  ASSERT_TRUE(Model.takePnpCompletion());
  success(Model.powerPolicyWake(PDO));
  EXPECT_FALSE(WaitWakePending);
  expectError(Model.powerPolicyWake(PDO), "successfully armed");
  success(Model.processPowerPolicy());
  expectCall(Entry);
  expectCall(Triggered);
  expectCall(Disarm);
  ASSERT_TRUE(Model.takePnpCompletion());
  EXPECT_EQ(WaitWakeArms, 1u);
  EXPECT_EQ(WaitWakeCompletions, 1u);
}

TEST_F(DriverKernelFrameworkPowerPolicy, NestedReferencesRestartFullTimeout) {
  configure();
  start();
  success(Model.powerPolicyIdle(PDO));
  for (unsigned I = 0; I < 2; ++I)
    EXPECT_EQ(
        take(invoke(api::WdfDeviceStopIdleNoTrack, {Globals, Device, 0}, 2)),
        windows::StatusSuccess);
  EXPECT_FALSE(Model.nextPowerPolicyTime());
  Now = 70;
  take(invoke(api::WdfDeviceResumeIdleNoTrack, {Globals, Device}, 2));
  EXPECT_FALSE(Model.nextPowerPolicyTime());
  take(invoke(api::WdfDeviceResumeIdleNoTrack, {Globals, Device}, 2));
  EXPECT_EQ(Model.nextPowerPolicyTime(), Now + policy::TicksPerMillisecond);
  expectError(invoke(api::WdfDeviceResumeIdleNoTrack, {Globals, Device}),
              "matching");
}

TEST_F(DriverKernelFrameworkPowerPolicy,
       RejectedIRQLAndSettingsDoNotTakeReference) {
  configure();
  start();
  success(Model.powerPolicyIdle(PDO));
  expectError(invoke(api::WdfDeviceStopIdleNoTrack, {Globals, Device, 1}, 2),
              "IRQL");
  expectError(invoke(api::WdfDeviceStopIdleNoTrack, {Globals, Device, 0}, 3),
              "IRQL");
  expectError(invoke(api::WdfDeviceResumeIdleNoTrack, {Globals, Device}),
              "matching");
  put(IdleConfig + policy::IdleTimeoutType, policy::SystemManagedTimeout, 4);
  expectError(
      invoke(api::WdfDeviceAssignS0IdleSettings, {Globals, Device, IdleConfig}),
      "PoFx authority");
  EXPECT_EQ(Model.nextPowerPolicyTime(), policy::TicksPerMillisecond);
}

TEST_F(DriverKernelFrameworkPowerPolicy, SynchronousStopWaitsForD0AndDisarm) {
  configure();
  start();
  success(Model.powerPolicyIdle(PDO));
  Now = policy::TicksPerMillisecond;
  success(Model.processPowerPolicy());
  expectCall(Arm);
  expectCall(Exit);
  ASSERT_TRUE(Model.takePnpCompletion());
  EXPECT_EQ(take(invoke(api::WdfDeviceStopIdleNoTrack, {Globals, Device, 1})),
            windows::StatusPending);
  EXPECT_FALSE(take(Model.powerPolicyWait(Device)));
  success(Model.processPowerPolicy());
  expectCall(Entry);
  EXPECT_FALSE(take(Model.powerPolicyWait(Device)));
  expectCall(Disarm);
  ASSERT_TRUE(Model.takePnpCompletion());
  EXPECT_EQ(take(Model.powerPolicyWait(Device)), windows::StatusSuccess);
  EXPECT_EQ(WaitWakeCompletions, 1u);
  take(invoke(api::WdfDeviceResumeIdleNoTrack, {Globals, Device}));
}

TEST_F(DriverKernelFrameworkPowerPolicy,
       DeadlineFailurePreservesSettingsAndReference) {
  configure();
  start();
  success(Model.powerPolicyIdle(PDO));
  Now = uint64_t(INT64_MAX) - 5;
  put(IdleConfig + policy::IdleTimeout, 2, 4);
  expectError(
      invoke(api::WdfDeviceAssignS0IdleSettings, {Globals, Device, IdleConfig}),
      "virtual clock");
  EXPECT_EQ(Model.nextPowerPolicyTime(), policy::TicksPerMillisecond);
  take(invoke(api::WdfDeviceStopIdleNoTrack, {Globals, Device, 0}));
  expectError(invoke(api::WdfDeviceResumeIdleNoTrack, {Globals, Device}),
              "virtual clock");
  Now = 0;
  take(invoke(api::WdfDeviceResumeIdleNoTrack, {Globals, Device}));
  EXPECT_EQ(Model.nextPowerPolicyTime(), policy::TicksPerMillisecond);
}

TEST_F(DriverKernelFrameworkPowerPolicy,
       StartEpochCannotSurviveStopAndRestart) {
  configure();
  start();
  EXPECT_EQ(take(Model.powerPolicyEpoch(PDO)), 1u);
  EXPECT_TRUE(take(Model.beginPnpPowerTransition(
      PDO, ++Packet, DevicePnpRequest::Stop, 0, 0, 0)));
  expectCall(Exit);
  ASSERT_TRUE(Model.takePnpCompletion());
  expectError(Model.powerPolicyEpoch(PDO), "started device epoch");
  start();
  EXPECT_EQ(take(Model.powerPolicyEpoch(PDO)), 2u);
}

TEST_F(DriverKernelFrameworkPowerPolicy,
       FailedSynchronousPowerUpReleasesOnlyItsReference) {
  idleSettings(false);
  configure();
  start();
  success(Model.powerPolicyIdle(PDO));
  Now = policy::TicksPerMillisecond;
  success(Model.processPowerPolicy());
  expectCall(Exit);
  ASSERT_TRUE(Model.takePnpCompletion());
  EXPECT_EQ(take(invoke(api::WdfDeviceStopIdleNoTrack, {Globals, Device, 1})),
            windows::StatusPending);
  success(Model.processPowerPolicy());
  expectCall(Entry, windows::StatusUnsuccessful);
  ASSERT_TRUE(Model.takePnpCompletion());
  EXPECT_FALSE(take(Model.powerPolicyDeviceCompletion(PDO, true)));
  success(Model.finishPowerPolicyRequest(PDO, windows::StatusUnsuccessful));
  EXPECT_EQ(take(Model.powerPolicyDeviceCompletion(PDO, true)),
            windows::StatusUnsuccessful);
  expectError(Model.powerPolicyDeviceReady(PDO, true),
              "device power transaction failed");
  EXPECT_EQ(take(Model.powerPolicyWait(Device)),
            policy::StatusPowerStateInvalid);
  expectError(invoke(api::WdfDeviceResumeIdleNoTrack, {Globals, Device}),
              "matching");
  EXPECT_EQ(take(invoke(api::WdfDeviceStopIdleNoTrack, {Globals, Device, 0})),
            policy::StatusPowerStateInvalid);
}

TEST_F(DriverKernelFrameworkPowerPolicy,
       AssignSettingsAcceptDispatchAndRejectWrongSizes) {
  EXPECT_EQ(take(invoke(api::WdfDeviceAssignS0IdleSettings,
                        {Globals, Device, IdleConfig}, 2)),
            0u);
  put(IdleConfig, policy::IdleSize - sizeof(uint32_t), 4);
  EXPECT_EQ(take(invoke(api::WdfDeviceAssignS0IdleSettings,
                        {Globals, Device, IdleConfig}, 2)),
            InfoLengthMismatch);
  expectError(invoke(api::WdfDeviceAssignS0IdleSettings,
                     {Globals, Device, IdleConfig}, 3),
              "IRQL");
}

TEST_F(DriverKernelFrameworkPowerPolicy,
       MissingResponseDoesNotArmOrConsumeIdleObservation) {
  configure();
  start();
  success(Model.powerPolicyIdle(PDO));
  Now = policy::TicksPerMillisecond;
  RejectPowerRequest = true;
  expectError(Model.processPowerPolicy(), "response unavailable");
  EXPECT_FALSE(WaitWakePending);
  EXPECT_EQ(WaitWakeArms, 0u);
  EXPECT_TRUE(Requests.empty());
  EXPECT_FALSE(Model.hasPendingGuestCall());
  EXPECT_EQ(Model.nextPowerPolicyTime(), Now);
  RejectPowerRequest = false;
  success(Model.processPowerPolicy());
  expectCall(Arm);
  expectCall(Exit);
  ASSERT_TRUE(Model.takePnpCompletion());
}

TEST_F(DriverKernelFrameworkPowerPolicy,
       ActivityDuringWakeArmingSurvivesPowerDown) {
  configure();
  start();
  success(Model.powerPolicyIdle(PDO));
  Now = policy::TicksPerMillisecond;
  success(Model.processPowerPolicy());
  success(Model.powerPolicyActive(PDO));
  expectCall(Arm);
  expectCall(Exit);
  ASSERT_TRUE(Model.takePnpCompletion());
  success(Model.processPowerPolicy());
  expectCall(Entry);
  expectCall(Disarm);
  ASSERT_TRUE(Model.takePnpCompletion());
  EXPECT_EQ(Requests,
            (std::vector{DevicePowerState::D3, DevicePowerState::D0}));
  EXPECT_EQ(WaitWakeCompletions, 1u);
}

TEST_F(DriverKernelFrameworkPowerPolicy,
       SleepingActivityOverridesRemainIdleOnSystemWake) {
  idleSettings(false);
  put(IdleConfig + policy::IdlePowerUpOnSystemWake, policy::False, 4);
  configure();
  start();
  success(Model.systemPowerPolicy(PDO, true));
  EXPECT_FALSE(take(Model.systemPowerNeedsD0(PDO)));
  success(Model.powerPolicyActive(PDO));
  EXPECT_TRUE(take(Model.systemPowerNeedsD0(PDO)));
  success(Model.processPowerPolicy());
  EXPECT_TRUE(Requests.empty());
}

TEST_F(DriverKernelFrameworkPowerPolicy, D2IdleWakePreservesCallbackStates) {
  put(IdleConfig + policy::IdleDxState, uint32_t(DevicePowerState::D2), 4);
  configure();
  start();
  success(Model.powerPolicyIdle(PDO));
  Now = policy::TicksPerMillisecond;
  success(Model.processPowerPolicy());
  expectCall(Arm);
  ASSERT_EQ(Requests, (std::vector{DevicePowerState::D2}));
  expectCall(Exit, 0, DevicePowerState::D2);
  ASSERT_TRUE(Model.takePnpCompletion());
  success(Model.powerPolicyWake(PDO));
  success(Model.processPowerPolicy());
  expectCall(Entry, 0, DevicePowerState::D2);
  expectCall(Triggered);
  expectCall(Disarm);
  ASSERT_TRUE(Model.takePnpCompletion());
  EXPECT_EQ(Requests,
            (std::vector{DevicePowerState::D2, DevicePowerState::D0}));
  EXPECT_EQ(WaitWakeArms, 1u);
  EXPECT_EQ(WaitWakeCompletions, 1u);
}

TEST_F(DriverKernelFrameworkPowerPolicy,
       ReassignedIdleStateUsesD2AndStopIdleRestoresD0) {
  idleSettings(false);
  configure();
  put(IdleConfig + policy::IdleDxState, uint32_t(DevicePowerState::D2), 4);
  configure();
  start();
  success(Model.powerPolicyIdle(PDO));
  Now = policy::TicksPerMillisecond;
  success(Model.processPowerPolicy());
  ASSERT_EQ(Requests, (std::vector{DevicePowerState::D2}));
  expectCall(Exit, 0, DevicePowerState::D2);
  ASSERT_TRUE(Model.takePnpCompletion());
  EXPECT_EQ(take(invoke(api::WdfDeviceStopIdleNoTrack, {Globals, Device, 1})),
            windows::StatusPending);
  success(Model.processPowerPolicy());
  expectCall(Entry, 0, DevicePowerState::D2);
  ASSERT_TRUE(Model.takePnpCompletion());
  EXPECT_EQ(take(Model.powerPolicyWait(Device)), windows::StatusSuccess);
  EXPECT_EQ(Requests,
            (std::vector{DevicePowerState::D2, DevicePowerState::D0}));
  EXPECT_EQ(WaitWakeArms, 0u);
  take(invoke(api::WdfDeviceResumeIdleNoTrack, {Globals, Device}));
}

TEST_F(DriverKernelFrameworkPowerPolicy,
       LowPowerStatesCannotTransitionWithoutD0) {
  start();
  for (const auto State : {DevicePowerState::D2, DevicePowerState::D3}) {
    EXPECT_TRUE(take(Model.beginDevicePowerTransition(
        PDO, ++Packet, DevicePowerState::D0, State)));
    expectCall(Exit, 0, State);
    ASSERT_TRUE(Model.takePnpCompletion());
    const auto Other = State == DevicePowerState::D2 ? DevicePowerState::D3
                                                     : DevicePowerState::D2;
    expectError(Model.beginDevicePowerTransition(PDO, ++Packet, State, Other),
                "require D0");
    EXPECT_FALSE(Model.hasPendingGuestCall());
    EXPECT_FALSE(Model.takePnpCompletion());
    EXPECT_TRUE(take(Model.beginDevicePowerTransition(PDO, ++Packet, State,
                                                      DevicePowerState::D0)));
    expectCall(Entry, 0, State);
    ASSERT_TRUE(Model.takePnpCompletion());
  }
}

TEST_F(DriverKernelFrameworkPowerPolicy,
       UnsupportedStatesCannotBypassValidationAsNoOps) {
  start();
  for (const auto State :
       {DevicePowerState::D1, DevicePowerState(0), DevicePowerState(5)}) {
    expectError(Model.beginDevicePowerTransition(PDO, ++Packet, State, State),
                "supported state");
    expectError(Model.beginDevicePowerTransition(PDO, ++Packet,
                                                 DevicePowerState::D0, State),
                "supported state");
    EXPECT_FALSE(Model.hasPendingGuestCall());
    EXPECT_FALSE(Model.takePnpCompletion());
  }
  EXPECT_FALSE(take(Model.beginDevicePowerTransition(
      PDO, ++Packet, DevicePowerState::D0, DevicePowerState::D0)));
}

TEST_F(DriverKernelFrameworkPowerPolicy,
       InvalidPolicyStatePreservesPreviousIdleAndWakeSettings) {
  idleSettings(false);
  put(IdleConfig + policy::IdleDxState, uint32_t(DevicePowerState::D2), 4);
  configure();
  wakeSettings(DevicePowerState::D2);
  for (const auto State :
       {DevicePowerState::D0, DevicePowerState::D1, DevicePowerState(5)}) {
    put(IdleConfig + policy::IdleDxState, uint32_t(State), 4);
    expectError(invoke(api::WdfDeviceAssignS0IdleSettings,
                       {Globals, Device, IdleConfig}),
                "supported low-power");
    put(WakeConfig + policy::WakeDxState, uint32_t(State), 4);
    expectError(invoke(api::WdfDeviceAssignSxWakeSettings,
                       {Globals, Device, WakeConfig}),
                "supported low-power");
    EXPECT_EQ(take(Model.systemSleepTarget(PDO)), DevicePowerState::D2);
  }
  start();
  success(Model.powerPolicyIdle(PDO));
  Now = policy::TicksPerMillisecond;
  success(Model.processPowerPolicy());
  EXPECT_EQ(Requests, (std::vector{DevicePowerState::D2}));
  expectCall(Exit, 0, DevicePowerState::D2);
  ASSERT_TRUE(Model.takePnpCompletion());
}

TEST_F(DriverKernelFrameworkPowerPolicy,
       SystemWakeUsesItsOwnExplicitLowPowerState) {
  EXPECT_EQ(take(Model.systemSleepTarget(PDO)), DevicePowerState::D3);
  wakeSettings(DevicePowerState::D2, false);
  EXPECT_EQ(take(Model.systemSleepTarget(PDO)), DevicePowerState::D3);
  wakeSettings(DevicePowerState::D3);
  wakeSettings(DevicePowerState::D2);
  EXPECT_EQ(take(Model.systemSleepTarget(PDO)), DevicePowerState::D2);
  start();
  success(Model.systemPowerPolicy(PDO, true));
  EXPECT_TRUE(take(
      Model.beginDevicePowerTransition(PDO, ++Packet, DevicePowerState::D0,
                                       take(Model.systemSleepTarget(PDO)))));
  const auto ArmCall = callback();
  EXPECT_EQ(ArmCall.PC, Arm);
  EXPECT_EQ(ArmCall.Arguments, (std::vector<uint64_t>{Device}));
  take(Model.finishGuestCall(ArmCall.Token, windows::StatusSuccess));
  expectCall(Exit, 0, DevicePowerState::D2);
  ASSERT_TRUE(Model.takePnpCompletion());
  EXPECT_TRUE(take(Model.systemSleepNeedsD0(PDO)));
  success(Model.systemPowerPolicy(PDO, false));
  EXPECT_TRUE(take(Model.beginDevicePowerTransition(
      PDO, ++Packet, DevicePowerState::D2, DevicePowerState::D0)));
  expectCall(Entry, 0, DevicePowerState::D2);
  expectCall(Disarm);
  ASSERT_TRUE(Model.takePnpCompletion());
  EXPECT_EQ(WaitWakeArms, 1u);
  EXPECT_EQ(WaitWakeCompletions, 1u);
}

TEST_F(DriverKernelFrameworkPowerPolicy,
       D2IdlePolicyCannotAuthorizeD3ColdOnSystemSleep) {
  unsigned ColdQueries = 0;
  KernelFramework::PowerPolicyHost Policy;
  Policy.ColdAllowed = [&](uint64_t Wdm, bool Wake, bool Sleeping,
                           uint32_t Exclude) -> llvm::Expected<bool> {
    EXPECT_EQ(Wdm, FDO);
    EXPECT_FALSE(Wake);
    EXPECT_TRUE(Sleeping);
    EXPECT_EQ(Exclude, policy::False);
    ++ColdQueries;
    return true;
  };
  Model.setPowerPolicyHost(std::move(Policy));
  idleSettings(false);
  put(IdleConfig + policy::IdleDxState, uint32_t(DevicePowerState::D2), 4);
  put(IdleConfig + policy::IdleExcludeD3Cold, policy::False, 4);
  configure();
  start();
  success(Model.systemPowerPolicy(PDO, true));
  EXPECT_EQ(take(Model.systemSleepTarget(PDO)), DevicePowerState::D3);
  EXPECT_FALSE(take(Model.allowsD3Cold(PDO)));
  EXPECT_EQ(ColdQueries, 0u);
  put(IdleConfig + policy::IdleDxState, uint32_t(DevicePowerState::D3), 4);
  configure();
  EXPECT_TRUE(take(Model.allowsD3Cold(PDO)));
  EXPECT_EQ(ColdQueries, 1u);
}

TEST(DriverPowerPolicyScenario, EmptyEventFieldStillRequiresAnIoRequest) {
  for (const auto *Kind : {"create", "cleanup", "close", "pnp", "power"}) {
    const std::string Json = std::string("{\"requests\":[{\"kind\":\"") + Kind +
                             "\",\"power_policy_events\":[]}]}";
    expectError(driverOptionsFromScenarioJSON(Json),
                "power_policy_events requires read, write or ioctl");
  }
}
} // namespace
} // namespace neverd::emulation
