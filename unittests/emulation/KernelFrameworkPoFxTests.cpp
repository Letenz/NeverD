//===- KernelFrameworkPoFxTests.cpp - KMDF component lifecycle ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercise real framework settings, callback ordering and component queue
/// gates.
///
//===----------------------------------------------------------------------===//
#include "KernelFrameworkTestSupport.h"

namespace neverd::emulation {
namespace {
using namespace framework;
using namespace framework_test;
namespace policy = power_policy;

class DriverKernelFrameworkPoFx : public DriverKernelFramework {
protected:
  static constexpr uint64_t PDO = Driver + 0x3000;
  static constexpr uint64_t FDO = Driver + 0x3100;
  static constexpr uint64_t InitSlot = Driver + 0x3200;
  static constexpr uint64_t DeviceSlot = Driver + 0x3208;
  static constexpr uint64_t IdleConfig = Driver + 0x3300;
  static constexpr uint64_t PnpConfig = Driver + 0x3400;
  static constexpr uint64_t Settings = Driver + 0x3500;
  static constexpr uint64_t ComponentDescription = Driver + 0x3600;
  static constexpr uint64_t Entry = 0x180003000;
  static constexpr uint64_t Exit = 0x180003100;
  static constexpr uint64_t SelfManaged = 0x180003200;
  static constexpr uint64_t Post = 0x180003300;
  static constexpr uint64_t Pre = 0x180003400;
  static constexpr uint64_t State = 0x180003500;
  static constexpr uint64_t Context = 0x180003600;
  static constexpr uint64_t Suspend = 0x180003700;
  static constexpr uint64_t FirstPoFxHandle = Driver + 0x4000;
  uint64_t Device = 0, Packet = 0, Handle = 0;
  uint64_t NextHandle = FirstPoFxHandle;
  bool Ready = true, Drained = true, Started = false;
  bool RejectSettings = false;
  bool RejectQuiesce = false;
  bool UseSuspend = false;
  std::vector<uint64_t> Registered, Unregistered;
  std::optional<KernelFrameworkPoFxSettings> Description;

  void SetUp() override {
    DriverKernelFramework::SetUp();
    KernelFramework::DeviceHost Devices;
    Devices.Create =
        [](llvm::StringRef, uint32_t,
           bool) -> llvm::Expected<KernelFramework::DeviceCreation> {
      return failure("fixture requires a PnP device");
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
    KernelFramework::PowerPolicyHost Policy;
    Policy.Now = [] { return uint64_t(0); };
    Policy.ManagedIdle = [](uint64_t, bool, uint64_t) {
      return llvm::Error::success();
    };
    Model.setPowerPolicyHost(std::move(Policy));
    KernelFramework::PoFxHost Host;
    Host.ReadComponent =
        [](uint64_t Address) -> llvm::Expected<KernelPoFx::Component> {
      if (Address != ComponentDescription)
        return failure("invalid component address");
      KernelPoFx::Component C;
      C.DeepestWakeableState = 1;
      C.IdleStates = {{0, 0, 100}, {10, 20, 25}};
      return C;
    };
    Host.Validate = [this](uint64_t Wdm,
                           const KernelFrameworkPoFxSettings &) -> llvm::Error {
      EXPECT_EQ(Wdm, FDO);
      if (RejectSettings)
        return failure("invalid component contract");
      return llvm::Error::success();
    };
    Host.Register = [this](uint64_t Wdm,
                           const KernelFrameworkPoFxSettings &Settings)
        -> llvm::Expected<uint64_t> {
      EXPECT_EQ(Wdm, FDO);
      EXPECT_EQ(Handle, 0u);
      Description = Settings;
      Handle = NextHandle++;
      Registered.push_back(Handle);
      return Handle;
    };
    Host.Start = [this](uint64_t Value) {
      EXPECT_EQ(Value, Handle);
      EXPECT_FALSE(Started);
      Started = true;
      return llvm::Error::success();
    };
    Host.Quiesce = [this](uint64_t Value) -> llvm::Error {
      EXPECT_EQ(Value, Handle);
      if (RejectQuiesce)
        return failure("outstanding device power transaction");
      return llvm::Error::success();
    };
    Host.CanUnregister = [this](uint64_t Value) -> llvm::Expected<bool> {
      EXPECT_EQ(Value, Handle);
      return Drained;
    };
    Host.Unregister = [this](uint64_t Value) {
      EXPECT_EQ(Value, Handle);
      EXPECT_TRUE(Drained);
      Unregistered.push_back(Value);
      Handle = 0;
      Started = false;
      return llvm::Error::success();
    };
    Host.ComponentReady = [this](uint64_t Wdm) -> llvm::Expected<bool> {
      EXPECT_EQ(Wdm, FDO);
      return Ready;
    };
    Model.setPoFxHost(std::move(Host));
    bind();
    put(Config + DriverConfigAddDevice, Entry);
    put(Config + DriverConfigFlags, 0, 4);
    createDriver();
    const auto Add = take(Model.beginPnpAddDevice(PDO));
    put(InitSlot, Add.Init);
    put(PnpConfig, PnpPowerCallbacksSize, 4);
    put(PnpConfig + PnpPowerCallbacksFirstOffset, Entry);
    put(PnpConfig + PnpPowerCallbacksFirstOffset + 2 * sizeof(uint64_t), Exit);
    put(PnpConfig + PnpPowerCallbacksFirstOffset + 8 * sizeof(uint64_t),
        SelfManaged);
    if (UseSuspend)
      put(PnpConfig + PnpPowerCallbacksFirstOffset + 9 * sizeof(uint64_t),
          Suspend);
    take(invoke(api::WdfDeviceInitSetPnpPowerEventCallbacks,
                {Globals, Add.Init, PnpConfig}));
    EXPECT_EQ(
        take(invoke(api::WdfDeviceCreate, {Globals, InitSlot, 0, DeviceSlot})),
        0u);
    Device = get(DeviceSlot);
    success(Model.finishPnpAddDevice(PDO, Add.Init, 0));
    put(IdleConfig, policy::IdleSize, 4);
    put(IdleConfig + policy::IdleCapabilities, policy::CannotWake, 4);
    put(IdleConfig + policy::IdleDxState, uint32_t(DevicePowerState::D3), 4);
    put(IdleConfig + policy::IdleUserControl, policy::NoUserControl, 4);
    put(IdleConfig + policy::IdleEnabled, policy::True, 4);
    put(IdleConfig + policy::IdleTimeoutType, policy::SystemManagedTimeout, 4);
    put(IdleConfig + policy::IdleExcludeD3Cold, policy::True, 4);
    put(Settings, PoFxSettingsSize, 4);
    put(Settings + PoFxSettingsPostRegister, Post);
    put(Settings + PoFxSettingsPreUnregister, Pre);
    put(Settings + PoFxSettingsComponent, ComponentDescription);
    put(Settings + PoFxSettingsIdleState, State);
    put(Settings + PoFxSettingsContext, Context);
    put(Settings + PoFxSettingsDirected, policy::False, 4);
  }

  void configureIdle() {
    EXPECT_EQ(take(invoke(api::WdfDeviceAssignS0IdleSettings,
                          {Globals, Device, IdleConfig})),
              0u);
  }
  llvm::Expected<uint64_t> assign(uint8_t IRQL = 0) {
    return invoke(api::WdfDeviceWdmAssignPowerFrameworkSettings,
                  {Globals, Device, Settings}, IRQL);
  }
  void finish(uint64_t PC, uint32_t Status = 0) {
    const auto Call = callback();
    EXPECT_EQ(Call.PC, PC);
    if (PC == Post || PC == Pre)
      EXPECT_EQ(Call.Arguments, (std::vector<uint64_t>{Device, Handle}));
    take(Model.finishGuestCall(Call.Token, Status));
  }
  void beginStart() {
    EXPECT_TRUE(take(Model.beginPnpPowerTransition(
        PDO, ++Packet, DevicePnpRequest::Start, 0, 0, 0)));
    finish(Entry);
  }
  void start() {
    beginStart();
    finish(SelfManaged);
    finish(Post);
    ASSERT_TRUE(Model.takePnpCompletion());
  }
};

TEST_F(DriverKernelFrameworkPoFx,
       SettingsRequireManagedPolicyAndOneAssignment) {
  expectError(assign(), "previously assigned");
  configureIdle();
  expectError(assign(1), "PASSIVE_LEVEL");
  EXPECT_EQ(take(assign()), 0u);
  expectError(assign(), "only once");
  EXPECT_TRUE(Registered.empty());
}

TEST_F(DriverKernelFrameworkPoFx, RejectedSettingsDoNotConsumeAssignment) {
  configureIdle();
  put(Settings, PoFxSettingsSize - sizeof(uint64_t), 4);
  EXPECT_EQ(take(assign()), InfoLengthMismatch);
  put(Settings, PoFxSettingsSize, 4);
  RejectSettings = true;
  expectError(assign(), "invalid component");
  RejectSettings = false;
  EXPECT_EQ(take(assign()), 0u);
}

TEST_F(DriverKernelFrameworkPoFx,
       DirectedPowerAndPEPRemainExplicitlyUnsupported) {
  configureIdle();
  for (const auto Directed : {policy::True, policy::UseDefault}) {
    put(Settings + PoFxSettingsDirected, Directed, 4);
    expectError(assign(), "directed power");
  }
  put(Settings + PoFxSettingsDirected, policy::False, 4);
  put(Settings + PoFxSettingsFlags, 1);
  expectError(assign(), "power-relation");
  put(Settings + PoFxSettingsFlags, 0);
  put(Settings + PoFxSettingsPowerControl, State);
  expectError(assign(), "PEP provider");
  put(Settings + PoFxSettingsPowerControl, 0);
  EXPECT_EQ(take(assign()), 0u);
}

TEST_F(DriverKernelFrameworkPoFx,
       CopiedSettingsReachPostBeforeManagementStarts) {
  configureIdle();
  EXPECT_EQ(take(assign()), 0u);
  put(Settings + PoFxSettingsContext, 0);
  put(Settings + PoFxSettingsIdleState, 0);
  beginStart();
  EXPECT_TRUE(Registered.empty());
  finish(SelfManaged);
  ASSERT_TRUE(Description);
  EXPECT_EQ(Description->Context, Context);
  EXPECT_EQ(Description->Routines.IdleState, State);
  EXPECT_EQ(Description->Component.IdleStates.size(), 2u);
  EXPECT_EQ(Description->Component.DeepestWakeableState, 1u);
  EXPECT_FALSE(Started);
  finish(Post);
  EXPECT_TRUE(Started);
  EXPECT_EQ(Model.takePnpCompletion()->Status, 0u);
}

TEST_F(DriverKernelFrameworkPoFx, AssignmentDuringSelfManagedInitIsAccepted) {
  configureIdle();
  beginStart();
  const auto Call = callback();
  ASSERT_EQ(Call.PC, SelfManaged);
  EXPECT_EQ(take(assign()), 0u);
  take(Model.finishGuestCall(Call.Token, 0));
  finish(Post);
  EXPECT_TRUE(Model.takePnpCompletion());
}

TEST_F(DriverKernelFrameworkPoFx, FirstAssignmentAfterStartIsRejected) {
  configureIdle();
  beginStart();
  finish(SelfManaged);
  EXPECT_TRUE(Model.takePnpCompletion());
  ASSERT_TRUE(Description);
  EXPECT_EQ(Description->Component.IdleStates.size(), 1u);
  expectError(assign(), "first START");
}

TEST_F(DriverKernelFrameworkPoFx,
       StopDrainsBeforeD0ExitAndPreThenRestartUsesFreshHandle) {
  configureIdle();
  EXPECT_EQ(take(assign()), 0u);
  start();
  const uint64_t Original = Handle;
  Drained = false;
  EXPECT_TRUE(take(Model.beginPnpPowerTransition(
      PDO, ++Packet, DevicePnpRequest::Stop, 0, 0, 0)));
  EXPECT_FALSE(Model.takeGuestCall());
  EXPECT_FALSE(Model.takePnpCompletion());
  EXPECT_TRUE(take(Model.powerPolicyDeviceReady(PDO, true)));
  success(Model.resumePoFxTransitions());
  EXPECT_TRUE(Unregistered.empty());
  Drained = true;
  success(Model.resumePoFxTransitions());
  EXPECT_EQ(Handle, Original);
  finish(Exit);
  finish(Pre);
  EXPECT_TRUE(Model.takePnpCompletion());
  EXPECT_EQ(Unregistered, (std::vector<uint64_t>{Original}));
  EXPECT_TRUE(take(Model.beginPnpPowerTransition(
      PDO, ++Packet, DevicePnpRequest::Start, 0, 0, 0)));
  finish(Entry);
  EXPECT_NE(Handle, Original);
  finish(Post);
  EXPECT_TRUE(Model.takePnpCompletion());
}

TEST_F(DriverKernelFrameworkPoFx, FailedPostUnwindsBeforeStartCanSucceed) {
  configureIdle();
  EXPECT_EQ(take(assign()), 0u);
  beginStart();
  finish(SelfManaged);
  finish(Post, windows::StatusUnsuccessful);
  EXPECT_FALSE(Started);
  finish(Exit);
  finish(Pre);
  ASSERT_TRUE(Model.takePnpCompletion());
  EXPECT_EQ(Registered.size(), 1u);
  EXPECT_EQ(Registered, Unregistered);
}

class DriverKernelFrameworkPoFxSuspend : public DriverKernelFrameworkPoFx {
protected:
  void SetUp() override {
    UseSuspend = true;
    DriverKernelFrameworkPoFx::SetUp();
  }
};

TEST_F(DriverKernelFrameworkPoFxSuspend,
       FailedPostSuspendsSelfManagedIoBeforeD0Exit) {
  configureIdle();
  EXPECT_EQ(take(assign()), 0u);
  beginStart();
  finish(SelfManaged);
  finish(Post, windows::StatusUnsuccessful);
  finish(Suspend);
  finish(Exit);
  finish(Pre);
  auto Completion = Model.takePnpCompletion();
  ASSERT_TRUE(Completion);
  EXPECT_EQ(Completion->Status, windows::StatusUnsuccessful);
  EXPECT_EQ(Registered, Unregistered);
}

TEST_F(DriverKernelFrameworkPoFxSuspend,
       RejectedTeardownPreservesD0AndQueueAdmissionUntilRetry) {
  configureIdle();
  EXPECT_EQ(take(assign()), 0u);
  start();
  const uint64_t Original = Handle;
  RejectQuiesce = true;
  expectError(Model.beginPnpPowerTransition(PDO, ++Packet,
                                            DevicePnpRequest::Stop, 0, 0, 0),
              "outstanding device power");
  EXPECT_FALSE(Model.takeGuestCall());
  EXPECT_FALSE(Model.takePnpCompletion());
  EXPECT_EQ(Handle, Original);
  EXPECT_TRUE(Unregistered.empty());
  EXPECT_EQ(take(invoke(api::WdfDeviceStopIdleNoTrack, {Globals, Device, 0})),
            windows::StatusSuccess);
  take(invoke(api::WdfDeviceResumeIdleNoTrack, {Globals, Device}));
  RejectQuiesce = false;
  EXPECT_TRUE(take(Model.beginPnpPowerTransition(
      PDO, ++Packet, DevicePnpRequest::Stop, 0, 0, 0)));
  finish(Suspend);
  finish(Exit);
  finish(Pre);
  ASSERT_TRUE(Model.takePnpCompletion());
  EXPECT_EQ(Unregistered, (std::vector<uint64_t>{Original}));
}

TEST_F(DriverKernelFrameworkPoFx,
       PhysicalD0CompletionDoesNotWaitForComponentF0) {
  configureIdle();
  EXPECT_EQ(take(assign()), 0u);
  start();
  Ready = false;
  EXPECT_EQ(take(invoke(api::WdfDeviceStopIdleNoTrack, {Globals, Device, 0})),
            windows::StatusPending);
  EXPECT_FALSE(take(Model.powerPolicyWait(Device)));
  EXPECT_TRUE(take(Model.powerPolicyDeviceReady(PDO, true)));
  success(Model.resumePoFxTransitions());
  EXPECT_FALSE(take(Model.powerPolicyWait(Device)));
  Ready = true;
  success(Model.resumePoFxTransitions());
  EXPECT_EQ(*take(Model.powerPolicyWait(Device)), windows::StatusSuccess);
  take(invoke(api::WdfDeviceResumeIdleNoTrack, {Globals, Device}));
}
} // namespace
} // namespace neverd::emulation
