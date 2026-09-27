//===- KernelFrameworkUsbIdleTests.cpp - USB idle policy ownership -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DriverScenario.h"
#include "KernelFrameworkTestSupport.h"

#include "neverd/emulation/DriverProfile.h"

namespace neverd::emulation {
namespace {
using namespace framework_test;
using namespace framework;
namespace policy = power_policy;

class DriverKernelFrameworkUsbIdle : public DriverKernelFramework {
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
  bool RejectCancel = false;
  bool FailUsbPowerAllocation = false;
  unsigned AllocationCleanups = 0;
  bool RemoteWake = false;
  bool HasDeviceWake = true;
  bool Entered = false;
  unsigned Submissions = 0, Cancellations = 0, Aborts = 0;
  uint64_t CallbackToken = 17;
  uint64_t SourceQueue = 0, ManagedQueue = 0;
  std::optional<UsbIdleKey> Registration;
  using Mode = KernelFramework::PowerPolicyHost::RequestMode;
  KernelFramework::PowerPolicyHost Policy;

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
    ioQueues();
    success(Model.finishPnpAddDevice(PDO, Add.Init, 0));

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
      if (auto E = Model.beginPowerPolicyRequest(PDO))
        return E;
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
    Policy.ResolveUsbIdle = [this](uint64_t Wdm, uint32_t Dx)
        -> llvm::Expected<KernelFramework::PowerPolicyHost::UsbIdleSettings> {
      EXPECT_EQ(Wdm, FDO);
      if (Dx == policy::DevicePowerMaximum && !HasDeviceWake)
        return failure("Maximum requires explicit DeviceWake");
      if (Dx != policy::DevicePowerMaximum &&
          Dx != uint32_t(DevicePowerState::D2))
        return failure("USB idle requires D2");
      return KernelFramework::PowerPolicyHost::UsbIdleSettings{
          DevicePowerState::D2, RemoteWake};
    };
    Policy.SubmitUsbIdle =
        [this](uint64_t Wdm, uint64_t Epoch) -> llvm::Expected<UsbIdleKey> {
      EXPECT_EQ(Wdm, FDO);
      EXPECT_EQ(Epoch, take(Model.powerPolicyEpoch(PDO)));
      EXPECT_FALSE(Registration);
      Registration = UsbIdleKey{PDO, ++Packet, Epoch};
      ++Submissions;
      return *Registration;
    };
    Policy.CancelUsbIdle = [this](UsbIdleKey Key, Mode Action) -> llvm::Error {
      if (!Registration || *Registration != Key)
        return failure("cancellation lost exact key");
      if (RejectCancel)
        return failure("cancellation preflight rejected");
      if (Action == Mode::Validate)
        return llvm::Error::success();
      ++Cancellations;
      if (!Entered) {
        Registration.reset();
        return Model.retireUsbIdleRegistration(Key);
      }
      return llvm::Error::success();
    };
    Policy.HasUsbIdle = [this](UsbIdleKey Key) -> llvm::Expected<bool> {
      return Registration && *Registration == Key;
    };
    Policy.RequestUsbIdlePower = [this](uint64_t Wdm, UsbIdleKey Key,
                                        uint64_t Token,
                                        Mode Action) -> llvm::Error {
      EXPECT_EQ(Token, CallbackToken);
      EXPECT_EQ(Registration, Key);
      if (Action == Mode::Issue) {
        EXPECT_TRUE(Entered);
        if (FailUsbPowerAllocation)
          return Model.finishUsbIdlePowerAdmissionFailure(
              Key, Token, windows::StatusInsufficientResources);
      }
      return Policy.Request(Wdm, DevicePowerState::D2, Action);
    };
    Policy.AbortUsbIdlePower = [this](UsbIdleKey Key, uint64_t Token,
                                      uint32_t Status) -> llvm::Error {
      EXPECT_EQ(Token, CallbackToken);
      EXPECT_EQ(Registration, Key);
      EXPECT_TRUE(Entered);
      EXPECT_NE(Status & profile::NTStatusFailureMask, 0u);
      ++Aborts;
      Entered = false;
      Registration.reset();
      return Model.retireUsbIdleRegistration(Key);
    };
    Policy.FinishUsbIdleCallback = [this](UsbIdleKey Key, uint64_t Token) {
      EXPECT_EQ(Token, CallbackToken);
      EXPECT_EQ(Registration, Key);
      EXPECT_TRUE(Entered);
      EXPECT_FALSE(WaitWakePending);
      ++AllocationCleanups;
      Entered = false;
      Registration.reset();
      return Model.retireUsbIdleRegistration(Key);
    };
    Model.setPowerPolicyHost(Policy);
    idleSettings();
  }
  void idleSettings(bool = false) {
    put(IdleConfig, policy::IdleSize, 4);
    put(IdleConfig + policy::IdleCapabilities, policy::UsbSelectiveSuspend, 4);
    put(IdleConfig + policy::IdleDxState, uint32_t(DevicePowerState::D2), 4);
    put(IdleConfig + policy::IdleTimeout, 1, 4);
    put(IdleConfig + policy::IdleUserControl, policy::NoUserControl, 4);
    put(IdleConfig + policy::IdleEnabled, policy::True, 4);
    put(IdleConfig + policy::IdlePowerUpOnSystemWake, policy::UseDefault, 4);
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
  void submit() {
    success(Model.powerPolicyIdle(PDO));
    Now += policy::TicksPerMillisecond;
    success(Model.processPowerPolicy());
    ASSERT_TRUE(Registration);
  }
  void permission() {
    ASSERT_TRUE(Registration);
    const auto Key = *Registration;
    const auto Epoch = take(Model.powerPolicyEpoch(PDO));
    success(Model.canBeginUsbIdlePermission(Key, Epoch, CallbackToken));
    Entered = true;
    success(Model.beginUsbIdlePermission(Key, Epoch, CallbackToken));
  }
  void acknowledgePower() {
    ASSERT_TRUE(Model.takePnpCompletion());
    success(Model.finishPowerPolicyRequest(PDO, windows::StatusSuccess));
  }
  std::pair<uint64_t, uint64_t> ioQueues() {
    if (SourceQueue)
      return {SourceQueue, ManagedQueue};
    KernelFramework::RequestHost Host;
    Host.View =
        [](uint64_t IRP) -> llvm::Expected<KernelFramework::RequestView> {
      return KernelFramework::RequestView{IRP, 0, RequestMajorRead, 0, 0, 4};
    };
    Host.MarkPending = [](uint64_t) { return llvm::Error::success(); };
    Host.IsCanceled = [](uint64_t) -> llvm::Expected<bool> { return false; };
    Host.Complete = [](uint64_t, uint32_t, uint64_t) {
      return llvm::Error::success();
    };
    Model.setRequestHost(std::move(Host));
    constexpr uint64_t QueueConfig = Driver + 0x6000;
    constexpr uint64_t QueueSlot = QueueConfig + 0x100;
    std::pair<uint64_t, uint64_t> Queues;
    for (bool Managed : {false, true}) {
      success(Memory.write(QueueConfig, std::vector<uint8_t>(QueueConfigSize)));
      put(QueueConfig, QueueConfigSize, 4);
      put(QueueConfig + QueueConfigDispatch, QueueDispatchParallel, 4);
      put(QueueConfig + QueueConfigPresentedRequests, UINT32_MAX, 4);
      put(QueueConfig + QueueConfigPowerManaged,
          Managed ? QueuePowerEnabled : QueuePowerDisabled, 4);
      put(QueueConfig + QueueConfigIsDefault, !Managed, 1);
      put(QueueConfig + QueueConfigRead, Managed ? Exit : Entry);
      EXPECT_EQ(take(invoke(api::WdfIoQueueCreate,
                            {Globals, Device, QueueConfig, 0, QueueSlot})),
                0u);
      (Managed ? Queues.second : Queues.first) = get(QueueSlot);
    }
    SourceQueue = Queues.first;
    ManagedQueue = Queues.second;
    return Queues;
  }
  uint64_t routedRead() {
    const auto Routed = take(Model.routeRequest(FDO, ++Packet));
    EXPECT_TRUE(Routed);
    if (!Routed || Routed->Arguments.size() < 2)
      return 0;
    EXPECT_EQ(Routed->PC, Entry);
    return Routed->Arguments[1];
  }
  void retire() {
    ASSERT_TRUE(Registration);
    const auto Key = *Registration;
    Entered = false;
    Registration.reset();
    success(Model.retireUsbIdleRegistration(Key));
  }
};

TEST_F(DriverKernelFrameworkUsbIdle, TimerSubmitsOnePacketWithoutD2OrWakeArm) {
  RemoteWake = true;
  configure();
  start();
  submit();
  EXPECT_TRUE(Requests.empty());
  EXPECT_EQ(WaitWakeArms, 0u);
  EXPECT_FALSE(Model.hasPendingGuestCall());
  EXPECT_FALSE(Model.nextPowerPolicyTime());
  Now += policy::TicksPerMillisecond;
  success(Model.processPowerPolicy());
  success(Model.powerPolicyIdle(PDO));
  success(Model.processPowerPolicy());
  EXPECT_EQ(Submissions, 1u);
  EXPECT_TRUE(Requests.empty());
}

TEST_F(DriverKernelFrameworkUsbIdle,
       PermissionCreatesOnlyItsActualD2Transition) {
  configure();
  start();
  submit();
  const auto Key = *Registration;
  permission();
  EXPECT_EQ(Requests, (std::vector{DevicePowerState::D2}));
  expectCall(Exit, 0, DevicePowerState::D2);
  acknowledgePower();
  EXPECT_EQ(Registration, Key);
  expectError(Model.beginUsbIdlePermission(
                  Key, take(Model.powerPolicyEpoch(PDO)), CallbackToken),
              "current idle D0");
}

TEST_F(DriverKernelFrameworkUsbIdle, ActivityCancelsBeforeD0Readiness) {
  configure();
  start();
  submit();
  const auto Key = *Registration;
  success(Model.powerPolicyActive(PDO));
  EXPECT_FALSE(Registration);
  EXPECT_EQ(Cancellations, 1u);
  EXPECT_TRUE(Requests.empty());
  EXPECT_FALSE(Model.nextPowerPolicyTime());
  expectError(Model.canBeginUsbIdlePermission(
                  Key, take(Model.powerPolicyEpoch(PDO)), CallbackToken),
              "current idle D0");
}

TEST_F(DriverKernelFrameworkUsbIdle,
       StopIdleCancelsRetainedPacketBeforeSuccess) {
  configure();
  start();
  submit();
  EXPECT_EQ(take(invoke(api::WdfDeviceStopIdleNoTrack, {Globals, Device, 1})),
            windows::StatusSuccess);
  EXPECT_EQ(Cancellations, 1u);
  EXPECT_FALSE(Registration);
  EXPECT_EQ(take(Model.powerPolicyWait(Device)), windows::StatusSuccess);
  take(invoke(api::WdfDeviceResumeIdleNoTrack, {Globals, Device}));
  EXPECT_EQ(Model.nextPowerPolicyTime(), Now + policy::TicksPerMillisecond);
  Now += policy::TicksPerMillisecond;
  success(Model.processPowerPolicy());
  EXPECT_EQ(Submissions, 2u);
}

TEST_F(DriverKernelFrameworkUsbIdle, FailedCancelDoesNotTakePowerReference) {
  configure();
  start();
  submit();
  RejectCancel = true;
  expectError(invoke(api::WdfDeviceStopIdleNoTrack, {Globals, Device, 1}),
              "preflight rejected");
  EXPECT_EQ(Cancellations, 0u);
  EXPECT_TRUE(Registration);
  expectError(invoke(api::WdfDeviceResumeIdleNoTrack, {Globals, Device}),
              "no matching");
  RejectCancel = false;
  success(Model.powerPolicyActive(PDO));
}

TEST_F(DriverKernelFrameworkUsbIdle,
       EnteredCancellationWaitsForD2ThenSeparateD0) {
  configure();
  start();
  submit();
  permission();
  EXPECT_EQ(take(invoke(api::WdfDeviceStopIdleNoTrack, {Globals, Device, 0})),
            windows::StatusPending);
  EXPECT_TRUE(Registration);
  EXPECT_FALSE(take(Model.powerPolicyWait(Device)));
  expectCall(Exit, 0, DevicePowerState::D2);
  acknowledgePower();
  retire();
  EXPECT_FALSE(take(Model.powerPolicyWait(Device)));
  success(Model.processPowerPolicy());
  EXPECT_EQ(Requests,
            (std::vector{DevicePowerState::D2, DevicePowerState::D0}));
  expectCall(Entry, 0, DevicePowerState::D2);
  EXPECT_FALSE(take(Model.powerPolicyWait(Device)));
  acknowledgePower();
  EXPECT_EQ(take(Model.powerPolicyWait(Device)), windows::StatusSuccess);
  take(invoke(api::WdfDeviceResumeIdleNoTrack, {Globals, Device}));
}

TEST_F(DriverKernelFrameworkUsbIdle, FailedArmRetiresUsbWithoutInventingD2) {
  RemoteWake = true;
  configure();
  start();
  submit();
  permission();
  EXPECT_TRUE(Requests.empty());
  EXPECT_TRUE(WaitWakePending);
  expectCall(Arm, windows::StatusInsufficientResources);
  EXPECT_EQ(Aborts, 1u);
  EXPECT_FALSE(Registration);
  EXPECT_FALSE(WaitWakePending);
  EXPECT_TRUE(Requests.empty());
  EXPECT_EQ(take(invoke(api::WdfDeviceStopIdleNoTrack, {Globals, Device, 1})),
            windows::StatusSuccess);
  take(invoke(api::WdfDeviceResumeIdleNoTrack, {Globals, Device}));
}

TEST_F(DriverKernelFrameworkUsbIdle,
       PermissionRejectsMissingPowerBudgetBeforeEntry) {
  configure();
  start();
  submit();
  const auto Key = *Registration;
  RejectPowerRequest = true;
  expectError(Model.canBeginUsbIdlePermission(
                  Key, take(Model.powerPolicyEpoch(PDO)), CallbackToken),
              "unavailable");
  EXPECT_EQ(Registration, Key);
  EXPECT_FALSE(Entered);
  EXPECT_TRUE(Requests.empty());
  EXPECT_FALSE(Model.hasPendingGuestCall());
  RejectPowerRequest = false;
  permission();
  expectCall(Exit, 0, DevicePowerState::D2);
  acknowledgePower();
}

TEST_F(DriverKernelFrameworkUsbIdle,
       DefaultTimeoutAndMaximumRequireCapability) {
  put(IdleConfig + policy::IdleTimeout, 0, 4);
  put(IdleConfig + policy::IdleDxState, policy::DevicePowerMaximum, 4);
  HasDeviceWake = false;
  expectError(
      invoke(api::WdfDeviceAssignS0IdleSettings, {Globals, Device, IdleConfig}),
      "explicit DeviceWake");
  HasDeviceWake = true;
  configure();
  start();
  success(Model.powerPolicyIdle(PDO));
  EXPECT_EQ(Model.nextPowerPolicyTime(),
            policy::DefaultIdleTimeoutMilliseconds *
                policy::TicksPerMillisecond);
}

TEST_F(DriverKernelFrameworkUsbIdle,
       ReconfigurationCancelsOnlyAfterValidation) {
  configure();
  start();
  submit();
  const auto Key = *Registration;
  put(IdleConfig + policy::IdleDxState, uint32_t(DevicePowerState::D3), 4);
  expectError(
      invoke(api::WdfDeviceAssignS0IdleSettings, {Globals, Device, IdleConfig}),
      "requires D2");
  EXPECT_EQ(Registration, Key);
  EXPECT_EQ(Cancellations, 0u);
  put(IdleConfig + policy::IdleCapabilities, policy::CannotWake, 4);
  configure();
  EXPECT_FALSE(Registration);
  EXPECT_EQ(Cancellations, 1u);
  Now += policy::TicksPerMillisecond;
  success(Model.processPowerPolicy());
  EXPECT_EQ(Requests, (std::vector{DevicePowerState::D3}));
  EXPECT_EQ(Submissions, 1u);
}

TEST_F(DriverKernelFrameworkUsbIdle, SystemSleepCancelsBeforeChangingPolicy) {
  configure();
  start();
  submit();
  RejectCancel = true;
  expectError(Model.systemPowerPolicy(PDO, true), "preflight rejected");
  EXPECT_TRUE(Registration);
  RejectCancel = false;
  success(Model.systemPowerPolicy(PDO, true));
  EXPECT_FALSE(Registration);
  EXPECT_FALSE(Model.nextPowerPolicyTime());
}

TEST_F(DriverKernelFrameworkUsbIdle,
       DisabledPolicyCancelsWithoutRestartingTimer) {
  configure();
  start();
  submit();
  put(IdleConfig + policy::IdleEnabled, policy::False, 4);
  configure();
  EXPECT_FALSE(Registration);
  EXPECT_EQ(Cancellations, 1u);
  EXPECT_FALSE(Model.nextPowerPolicyTime());
  Now += policy::TicksPerMillisecond;
  success(Model.processPowerPolicy());
  EXPECT_EQ(Submissions, 1u);
  EXPECT_TRUE(Requests.empty());
}

TEST_F(DriverKernelFrameworkUsbIdle,
       StopDrainsRegistrationAndRestartRejectsOldEpoch) {
  configure();
  start();
  submit();
  const auto OldKey = *Registration;
  const auto OldEpoch = take(Model.powerPolicyEpoch(PDO));
  EXPECT_TRUE(take(Model.beginPnpPowerTransition(
      PDO, ++Packet, DevicePnpRequest::Stop, 0, 0, 0)));
  EXPECT_FALSE(Registration);
  expectCall(Exit);
  ASSERT_TRUE(Model.takePnpCompletion());
  start();
  submit();
  ASSERT_TRUE(Registration);
  EXPECT_NE(*Registration, OldKey);
  EXPECT_GT(take(Model.powerPolicyEpoch(PDO)), OldEpoch);
  expectError(
      Model.canBeginUsbIdlePermission(*Registration, OldEpoch, CallbackToken),
      "current idle D0");
  expectError(Model.retireUsbIdleRegistration(OldKey), "exact registration");
  EXPECT_EQ(Submissions, 2u);
  EXPECT_TRUE(Registration);
}

TEST_F(DriverKernelFrameworkUsbIdle,
       UnsupportedPoFxDoesNotBypassUsbPermission) {
  configure();
  start();
  submit();
  const auto Key = *Registration;
  put(IdleConfig + policy::IdleTimeoutType, policy::SystemManagedTimeout, 4);
  expectError(
      invoke(api::WdfDeviceAssignS0IdleSettings, {Globals, Device, IdleConfig}),
      "driver-managed timeout");
  EXPECT_EQ(Registration, Key);
  EXPECT_EQ(Cancellations, 0u);
  EXPECT_TRUE(Requests.empty());
}

TEST_F(DriverKernelFrameworkUsbIdle,
       MissingWakeHostRejectsBeforeCallbackEntry) {
  RemoteWake = true;
  configure();
  start();
  submit();
  const auto Key = *Registration;
  const auto Epoch = take(Model.powerPolicyEpoch(PDO));
  const auto ArmWake = Policy.ArmWake;
  Policy.ArmWake = {};
  Model.setPowerPolicyHost(Policy);
  expectError(Model.canBeginUsbIdlePermission(Key, Epoch, CallbackToken),
              "WAIT_WAKE bridge");
  expectError(Model.beginUsbIdlePermission(Key, Epoch, CallbackToken),
              "WAIT_WAKE bridge");
  EXPECT_EQ(Registration, Key);
  EXPECT_TRUE(Requests.empty());
  EXPECT_FALSE(Model.hasPendingGuestCall());
  Policy.ArmWake = ArmWake;
  Model.setPowerPolicyHost(Policy);
  permission();
  expectCall(Arm);
  EXPECT_EQ(Requests, (std::vector{DevicePowerState::D2}));
  expectCall(Exit, 0, DevicePowerState::D2);
  acknowledgePower();
}

TEST_F(DriverKernelFrameworkUsbIdle,
       ForwardToManagedQueueCancelsRetainedUsbBeforeDelivery) {
  configure();
  start();
  const auto [Source, Managed] = ioQueues();
  submit();
  const auto Request = routedRead();
  ASSERT_NE(Request, 0u);
  EXPECT_EQ(Cancellations, 0u);
  EXPECT_EQ(take(invoke(api::WdfRequestForwardToIoQueue,
                        {Globals, Request, Managed})),
            windows::StatusSuccess);
  EXPECT_EQ(Cancellations, 1u);
  EXPECT_FALSE(Registration);
  const auto Presented = callback();
  EXPECT_EQ(Presented.PC, Exit);
  EXPECT_EQ(Presented.Arguments, (std::vector<uint64_t>{Managed, Request, 4}));
  finish(Presented);
  EXPECT_TRUE(Requests.empty());
  EXPECT_EQ(take(invoke(api::WdfRequestGetIoQueue, {Globals, Request})),
            Managed);
}

TEST_F(DriverKernelFrameworkUsbIdle,
       FailedForwardActivationPreservesSourceOwnership) {
  configure();
  start();
  const auto [Source, Managed] = ioQueues();
  submit();
  const auto Request = routedRead();
  ASSERT_NE(Request, 0u);
  RejectCancel = true;
  expectError(
      invoke(api::WdfRequestForwardToIoQueue, {Globals, Request, Managed}),
      "preflight rejected");
  EXPECT_EQ(take(invoke(api::WdfRequestGetIoQueue, {Globals, Request})),
            Source);
  EXPECT_TRUE(Registration);
  EXPECT_FALSE(Model.hasPendingGuestCall());
  RejectCancel = false;
  EXPECT_EQ(take(invoke(api::WdfRequestForwardToIoQueue,
                        {Globals, Request, Managed})),
            windows::StatusSuccess);
  EXPECT_EQ(Cancellations, 1u);
  finish(callback());
  EXPECT_EQ(take(invoke(api::WdfRequestGetIoQueue, {Globals, Request})),
            Managed);
}

TEST_F(DriverKernelFrameworkUsbIdle,
       ForwardToManagedQueueRequestsD0AndWaitsForEntry) {
  configure();
  start();
  const auto [Source, Managed] = ioQueues();
  submit();
  permission();
  expectCall(Exit, 0, DevicePowerState::D2);
  acknowledgePower();
  Entered = false;
  const auto Request = routedRead();
  ASSERT_NE(Request, 0u);
  EXPECT_EQ(take(invoke(api::WdfRequestForwardToIoQueue,
                        {Globals, Request, Managed})),
            windows::StatusSuccess);
  EXPECT_FALSE(Model.hasPendingGuestCall());
  success(Model.processPowerPolicy());
  EXPECT_EQ(Requests,
            (std::vector{DevicePowerState::D2, DevicePowerState::D0}));
  expectCall(Entry, 0, DevicePowerState::D2);
  const auto Presented = callback();
  EXPECT_EQ(Presented.PC, Exit);
  EXPECT_EQ(Presented.Arguments, (std::vector<uint64_t>{Managed, Request, 4}));
  finish(Presented);
  acknowledgePower();
}

TEST_F(DriverKernelFrameworkUsbIdle,
       FailedPowerAllocationDisarmsBeforeNativeReturn) {
  RemoteWake = true;
  configure();
  start();
  submit();
  permission();
  FailUsbPowerAllocation = true;
  expectCall(Arm);
  EXPECT_TRUE(Registration);
  EXPECT_EQ(AllocationCleanups, 0u);
  EXPECT_TRUE(Requests.empty());
  expectCall(Disarm);
  const auto Done = Model.takePnpCompletion();
  ASSERT_TRUE(Done);
  EXPECT_EQ(Done->IdlePolicyDevice, Device);
  success(Model.finishIdlePowerDown(Device, Done->Status));
  EXPECT_EQ(AllocationCleanups, 1u);
  EXPECT_EQ(Aborts, 0u);
  EXPECT_FALSE(Registration);
  EXPECT_EQ(WaitWakeArms, 1u);
  EXPECT_EQ(WaitWakeCompletions, 1u);
  EXPECT_TRUE(Requests.empty());
  EXPECT_EQ(take(invoke(api::WdfDeviceStopIdleNoTrack, {Globals, Device, 1})),
            windows::StatusSuccess);
}

TEST_F(DriverKernelFrameworkUsbIdle,
       UnarmedAllocationFailureRestoresD0WithoutCallbacks) {
  configure();
  start();
  submit();
  FailUsbPowerAllocation = true;
  permission();
  EXPECT_EQ(AllocationCleanups, 1u);
  EXPECT_FALSE(Registration);
  EXPECT_TRUE(Requests.empty());
  EXPECT_FALSE(Model.hasPendingGuestCall());
  EXPECT_EQ(take(invoke(api::WdfDeviceStopIdleNoTrack, {Globals, Device, 1})),
            windows::StatusSuccess);
}

} // namespace
} // namespace neverd::emulation
