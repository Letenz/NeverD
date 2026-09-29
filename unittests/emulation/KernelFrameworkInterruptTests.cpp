//===- KernelFrameworkInterruptTests.cpp - Interrupt object power ownership
//===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "KernelFrameworkTestSupport.h"
#include "os/windows/KernelResources.h"
#include "os/windows/KernelScheduler.h"

namespace neverd::emulation {
namespace {
using namespace framework_test;
using namespace framework;
namespace policy = power_policy;

class DriverKernelFrameworkInterrupt : public DriverKernelFramework {
protected:
  static constexpr uint64_t PDO = Driver + 0x3000;
  static constexpr uint64_t FDO = Driver + 0x3100;
  static constexpr uint64_t InitSlot = Driver + 0x3200;
  static constexpr uint64_t DeviceSlot = Driver + 0x3208;
  static constexpr uint64_t PnpConfig = Driver + 0x3300;
  static constexpr uint64_t InterruptConfig = Driver + 0x3400;
  static constexpr uint64_t InterruptSlot = Driver + 0x3500;
  static constexpr uint64_t InterruptInfo = Driver + 0x3600;
  static constexpr uint64_t ISR = 0x180003000;
  static constexpr uint64_t Enable = 0x180003100;
  static constexpr uint64_t Disable = 0x180003200;
  static constexpr uint64_t DPC = 0x180003300;
  static constexpr uint64_t Before = 0x180004000;
  static constexpr uint64_t After = 0x180004100;
  static constexpr uint64_t Exit = 0x180004200;
  static constexpr uint64_t Release = 0x180004300;
  static constexpr uint64_t WakeArm = 0x180004400;
  static constexpr uint64_t WakeDisarm = 0x180004500;
  static constexpr uint64_t WakeTriggered = 0x180004600;
  uint64_t Device = 0, NextConnection = 0x400000;
  unsigned Assigned = 1;
  bool WithPrepareHardware = false;
  bool PendingWake = false;
  std::set<uint64_t> Connected;
  std::set<uint64_t> Inactive;
  std::map<uint64_t, uint64_t> Deferred;
  std::vector<std::pair<uint64_t, std::vector<uint64_t>>> Calls;

  void SetUp() override {
    DriverKernelFramework::SetUp();
    KernelFramework::DeviceHost Devices;
    Devices.Create =
        [](llvm::StringRef, uint32_t,
           bool) -> llvm::Expected<KernelFramework::DeviceCreation> {
      return failure("this fixture only creates a PnP FDO");
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
    put(Config + DriverConfigAddDevice, Before);
    put(Config + DriverConfigFlags, 0, 4);
    createDriver();
    const auto Add = take(Model.beginPnpAddDevice(PDO));
    put(InitSlot, Add.Init);
    put(PnpConfig, PnpPowerCallbacksSize, 4);
    put(PnpConfig + PnpPowerCallbacksFirstOffset, Before);
    put(PnpConfig + PnpPowerCallbacksFirstOffset + 8, After);
    put(PnpConfig + PnpPowerCallbacksFirstOffset + 16, Exit);
    put(PnpConfig + PnpPowerCallbacksFirstOffset + 40, Release);
    if (WithPrepareHardware) {
      put(PnpConfig + PnpPowerCallbacksFirstOffset + 32, ChildCleanup);
      constexpr uint64_t PowerCallbacks = Driver + 0x7300;
      put(PowerCallbacks, policy::CallbacksSize, 4);
      put(PowerCallbacks + policy::CallbacksFirst + 3 * sizeof(uint64_t),
          WakeArm);
      put(PowerCallbacks + policy::CallbacksFirst + 4 * sizeof(uint64_t),
          WakeDisarm);
      put(PowerCallbacks + policy::CallbacksFirst + 5 * sizeof(uint64_t),
          WakeTriggered);
      take(invoke(api::WdfDeviceInitSetPowerPolicyEventCallbacks,
                  {Globals, Add.Init, PowerCallbacks}));
    }
    take(invoke(api::WdfDeviceInitSetPnpPowerEventCallbacks,
                {Globals, Add.Init, PnpConfig}));
    EXPECT_EQ(
        take(invoke(api::WdfDeviceCreate, {Globals, InitSlot, 0, DeviceSlot})),
        0u);
    Device = get(DeviceSlot);
    success(Model.finishPnpAddDevice(PDO, Add.Init, 0));
    KernelFramework::InterruptHost Host;
    Host.Describe = [this](const KernelFramework::InterruptSelection &S)
        -> llvm::Expected<std::optional<KernelFramework::InterruptConnection>> {
      if (S.Ordinal >= Assigned)
        return std::optional<KernelFramework::InterruptConnection>{};
      KernelFramework::InterruptConnection Info;
      Info.Vector = 65;
      Info.Affinity = 1;
      Info.IRQL = S.Passive ? 0 : 5;
      return std::optional<KernelFramework::InterruptConnection>{Info};
    };
    Host.Connect = [this, Describe = Host.Describe](const auto &Selection,
                                                    uint64_t, uint64_t) mutable
        -> llvm::Expected<std::optional<KernelFramework::InterruptConnection>> {
      auto Result = Describe(Selection);
      if (!Result)
        return Result.takeError();
      if (*Result) {
        (*Result)->Token = NextConnection++;
        Connected.insert((*Result)->Token);
      }
      return Result;
    };
    Host.Disconnect = [this](uint64_t Object) {
      EXPECT_EQ(Connected.erase(Object), 1u);
      Inactive.erase(Object);
      return llvm::Error::success();
    };
    Host.HasPendingWake = [this](uint64_t Owner) {
      EXPECT_EQ(Owner, PDO);
      return PendingWake;
    };
    Host.SetActive = [this](uint64_t Object, bool Active) {
      EXPECT_TRUE(Connected.contains(Object));
      if (Active)
        Inactive.erase(Object);
      else
        Inactive.insert(Object);
      return llvm::Error::success();
    };
    Host.PrepareCall =
        [this](uint64_t Object, uint64_t PC, llvm::ArrayRef<uint64_t> Arguments,
               uint64_t Token) -> llvm::Expected<GuestCallToken> {
      EXPECT_TRUE(Connected.count(Object));
      Calls.emplace_back(PC, Arguments.vec());
      return GuestCallToken{GuestCallOwner::Interrupt, Token};
    };
    Host.QueueDeferred = [this](uint64_t Handle, uint64_t, uint64_t,
                                llvm::ArrayRef<uint64_t>, bool, uint64_t Token,
                                uint64_t) -> llvm::Expected<bool> {
      return Deferred.emplace(Handle, Token).second;
    };
    Host.HasDeferred = [this](uint64_t Handle) {
      return Deferred.count(Handle);
    };
    Model.setInterruptHost(std::move(Host));
    configureInterrupt();
  }

  void configureInterrupt(bool PowerCallbacks = true) {
    success(Memory.write(InterruptConfig,
                         std::vector<uint8_t>(InterruptConfigSize)));
    put(InterruptConfig, InterruptConfigSize, 4);
    put(InterruptConfig + InterruptShareVector, InterruptTriDefault, 4);
    put(InterruptConfig + InterruptReportInactive, InterruptTriDefault, 4);
    put(InterruptConfig + InterruptISR, ISR);
    put(InterruptConfig + InterruptDPC, DPC);
    if (PowerCallbacks) {
      put(InterruptConfig + InterruptEnable, Enable);
      put(InterruptConfig + InterruptDisable, Disable);
    }
  }
  llvm::Expected<uint64_t> create() {
    return invoke(api::WdfInterruptCreate,
                  {Globals, Device, InterruptConfig, 0, InterruptSlot});
  }
  uint64_t interrupt() {
    EXPECT_EQ(take(create()), 0u);
    return get(InterruptSlot);
  }
  void start() {
    EXPECT_TRUE(take(Model.beginPnpPowerTransition(
        PDO, InitSlot, DevicePnpRequest::Start, 0, 0, 0)));
  }
  void expectCallback(uint64_t PC, uint64_t Status = 0) {
    const auto Call = callback();
    EXPECT_EQ(Call.PC, PC);
    take(Model.finishGuestCall(Call.Token, Status));
  }
};

class DriverKernelFrameworkWakeInterrupt
    : public DriverKernelFrameworkInterrupt {
protected:
  void SetUp() override {
    WithPrepareHardware = true;
    DriverKernelFrameworkInterrupt::SetUp();
    KernelFramework::PowerPolicyHost Host;
    Host.CanWake = [](uint64_t, bool) -> llvm::Expected<bool> { return true; };
    Host.ArmWake = [](uint64_t, bool) { return llvm::Error::success(); };
    Host.FinishWake = [](uint64_t, bool) { return llvm::Error::success(); };
    Model.setPowerPolicyHost(std::move(Host));
    constexpr uint64_t IdleConfig = Driver + 0x7000;
    put(IdleConfig, policy::IdleSize, 4);
    put(IdleConfig + policy::IdleCapabilities, policy::CanWake, 4);
    put(IdleConfig + policy::IdleDxState, uint32_t(DevicePowerState::D3), 4);
    put(IdleConfig + policy::IdleTimeout, 1, 4);
    put(IdleConfig + policy::IdleUserControl, policy::NoUserControl, 4);
    put(IdleConfig + policy::IdleEnabled, policy::True, 4);
    put(IdleConfig + policy::IdleExcludeD3Cold, policy::True, 4);
    EXPECT_EQ(take(invoke(api::WdfDeviceAssignS0IdleSettings,
                          {Globals, Device, IdleConfig})),
              0u);
    put(InterruptConfig + InterruptCanWake, 1, 1);
    put(InterruptConfig + InterruptPassiveHandling, 1, 1);
    put(InterruptConfig + InterruptDPC, 0);
  }

  uint64_t prepare() {
    constexpr uint64_t Raw = Driver + 0x7100;
    constexpr uint64_t Translated = Driver + 0x7200;
    constexpr uint64_t ListSize =
        resources::ResourceHeaderSize + resources::ResourceDescriptorSize;
    for (const auto Address : {Raw, Translated}) {
      put(Address + resources::ResourceCountOffset,
          resources::SupportedFullDescriptorCount,
          resources::ResourceCountFieldSize);
      put(Address + resources::ResourcePartialCountOffset, 1,
          resources::ResourceCountFieldSize);
      put(Address + resources::ResourceHeaderSize, resources::InterruptType, 1);
    }
    EXPECT_TRUE(take(Model.beginPnpPowerTransition(
        PDO, InitSlot, DevicePnpRequest::Start, Raw, Translated, ListSize)));
    auto Call = callback();
    EXPECT_EQ(Call.PC, ChildCleanup);
    EXPECT_EQ(Call.Arguments.size(), 3u);
    put(InterruptConfig + InterruptRaw,
        take(invoke(api::WdfCmResourceListGetDescriptor,
                    {Globals, Call.Arguments[1], 0})));
    put(InterruptConfig + InterruptTranslated,
        take(invoke(api::WdfCmResourceListGetDescriptor,
                    {Globals, Call.Arguments[2], 0})));
    const auto Handle = interrupt();
    take(Model.finishGuestCall(Call.Token, 0));
    expectCallback(Before);
    expectCallback(Enable);
    expectCallback(After);
    EXPECT_TRUE(Model.takePnpCompletion());
    return Handle;
  }
};

TEST_F(DriverKernelFrameworkWakeInterrupt,
       WakeCreationRequiresPassiveHandlingAndAssignedPreparation) {
  put(InterruptConfig + InterruptPassiveHandling, 0, 1);
  EXPECT_EQ(take(create()), InvalidParameter);
  put(InterruptConfig + InterruptPassiveHandling, 1, 1);
  expectError(create(), "prepare-hardware");
}

TEST_F(DriverKernelFrameworkWakeInterrupt,
       D0EntryPrecedesWakeISRAndInterruptEnableIsNotRepeated) {
  const auto Handle = prepare();
  const auto Connection = *Connected.begin();
  EXPECT_TRUE(take(Model.beginDevicePowerTransition(
      PDO, InitSlot, DevicePowerState::D0, DevicePowerState::D3)));
  expectCallback(Exit);
  ASSERT_TRUE(Model.takePnpCompletion());
  EXPECT_EQ(Connected, (std::set<uint64_t>{Connection}));
  EXPECT_TRUE(Inactive.empty());
  PendingWake = true;
  EXPECT_TRUE(take(Model.beginDevicePowerTransition(
      PDO, InitSlot, DevicePowerState::D3, DevicePowerState::D0)));
  EXPECT_FALSE(Model.canDeliverWakeInterrupt(PDO));
  expectCallback(Before);
  EXPECT_TRUE(Model.canDeliverWakeInterrupt(PDO));
  EXPECT_FALSE(Model.hasPendingGuestCall());
  EXPECT_FALSE(Model.takePnpCompletion());
  success(Model.resumeInterruptDrain());
  EXPECT_FALSE(Model.hasPendingGuestCall());
  PendingWake = false;
  success(Model.resumeInterruptDrain());
  EXPECT_FALSE(Model.canDeliverWakeInterrupt(PDO));
  expectCallback(After);
  ASSERT_TRUE(Model.takePnpCompletion());
  EXPECT_EQ(take(invoke(api::WdfInterruptWdmGetInterrupt, {Globals, Handle})),
            Connection);
}

TEST_F(DriverKernelFrameworkWakeInterrupt,
       WakeISRPrecedesDisarmAndTriggerBeforePostInterruptPowerCallback) {
  prepare();
  constexpr uint64_t WakeConfig = Driver + 0x7400;
  put(WakeConfig, policy::WakeSize, 4);
  put(WakeConfig + policy::WakeDxState, uint32_t(DevicePowerState::D3), 4);
  put(WakeConfig + policy::WakeUserControl, policy::NoUserControl, 4);
  put(WakeConfig + policy::WakeEnabled, policy::True, 4);
  EXPECT_EQ(take(invoke(api::WdfDeviceAssignSxWakeSettings,
                        {Globals, Device, WakeConfig})),
            0u);
  success(Model.systemPowerPolicy(PDO, true));
  EXPECT_TRUE(take(Model.beginDevicePowerTransition(
      PDO, InitSlot, DevicePowerState::D0, DevicePowerState::D3)));
  expectCallback(WakeArm);
  expectCallback(Exit);
  ASSERT_TRUE(Model.takePnpCompletion());
  PendingWake = true;
  success(Model.powerPolicyWake(PDO));
  success(Model.systemPowerPolicy(PDO, false));
  EXPECT_TRUE(take(Model.beginDevicePowerTransition(
      PDO, InitSlot, DevicePowerState::D3, DevicePowerState::D0)));
  expectCallback(Before);
  EXPECT_TRUE(Model.canDeliverWakeInterrupt(PDO));
  EXPECT_FALSE(Model.hasPendingGuestCall());
  PendingWake = false;
  success(Model.resumeInterruptDrain());
  expectCallback(WakeDisarm);
  expectCallback(WakeTriggered);
  expectCallback(After);
  ASSERT_TRUE(Model.takePnpCompletion());
}

TEST_F(DriverKernelFrameworkWakeInterrupt,
       FailedD0EntryDisablesWakeInterruptBeforeHardwareRelease) {
  prepare();
  EXPECT_TRUE(take(Model.beginDevicePowerTransition(
      PDO, InitSlot, DevicePowerState::D0, DevicePowerState::D3)));
  expectCallback(Exit);
  ASSERT_TRUE(Model.takePnpCompletion());
  PendingWake = true;
  EXPECT_TRUE(take(Model.beginDevicePowerTransition(
      PDO, InitSlot, DevicePowerState::D3, DevicePowerState::D0)));
  expectCallback(Before, windows::StatusUnsuccessful);
  EXPECT_FALSE(Model.canDeliverWakeInterrupt(PDO));
  expectCallback(Disable);
  EXPECT_TRUE(Connected.empty());
  expectCallback(Release);
  const auto Completion = Model.takePnpCompletion();
  ASSERT_TRUE(Completion);
  EXPECT_EQ(Completion->Status, windows::StatusUnsuccessful);
}

TEST_F(DriverKernelFrameworkInterrupt,
       SynchronizeReturnDrainsRetiredCallbackLocksAndPreservesBoolean) {
  const auto Handle = interrupt();
  start();
  expectCallback(Before);
  expectCallback(Enable);
  expectCallback(After);
  ASSERT_TRUE(Model.takePnpCompletion());

  constexpr uint64_t QueueConfig = Driver + 0x4000;
  constexpr uint64_t QueueSlot = Driver + 0x4100;
  put(QueueConfig, QueueConfigSize, 4);
  put(QueueConfig + QueueConfigDispatch, QueueDispatchManual, 4);
  attributes(0, 0, 0, ChildDestroy);
  EXPECT_EQ(take(invoke(api::WdfIoQueueCreate,
                        {Globals, Device, QueueConfig, Attrs, QueueSlot})),
            0u);
  const uint64_t Queue = get(QueueSlot);
  success(Model.retainSynchronizationObject(Queue));
  take(invoke(api::WdfObjectDelete, {Globals, Queue}));
  EXPECT_FALSE(Model.takeGuestCall());
  take(invoke(api::WdfInterruptSynchronize, {Globals, Handle, Before, 0}));
  const auto Synchronized = callback();
  success(Model.releaseSynchronizationObject(Queue));
  EXPECT_FALSE(take(Model.finishGuestCall(Synchronized.Token, 1)));
  const auto Destroy = callback();
  EXPECT_EQ(Destroy.PC, ChildDestroy);
  EXPECT_EQ(Destroy.SynchronizationObject, 0u);
  EXPECT_EQ(take(Model.finishGuestCall(Destroy.Token, 0)),
            std::optional<uint64_t>{1});
  EXPECT_TRUE(Released.count(Queue));
}

TEST_F(DriverKernelFrameworkInterrupt,
       AutomaticPowerCallbacksBracketD0AndResources) {
  const auto Handle = interrupt();
  start();
  expectCallback(Before);
  const auto Call = callback();
  EXPECT_EQ(Call.PC, Enable);
  EXPECT_EQ(Call.Arguments, (std::vector<uint64_t>{Handle, Device}));
  ASSERT_TRUE(Call.ExecutionToken);
  EXPECT_EQ(Call.ExecutionToken->Owner, GuestCallOwner::Interrupt);
  take(Model.finishGuestCall(Call.Token, 0));
  expectCallback(After);
  ASSERT_TRUE(Model.takePnpCompletion());
  ASSERT_EQ(Connected.size(), 1u);
  EXPECT_TRUE(take(Model.beginPnpPowerTransition(
      PDO, InitSlot, DevicePnpRequest::Stop, 0, 0, 0)));
  expectCallback(Disable);
  EXPECT_TRUE(Connected.empty());
  expectCallback(Exit);
  expectCallback(Release);
  ASSERT_TRUE(Model.takePnpCompletion());
  EXPECT_EQ(take(invoke(api::WdfInterruptWdmGetInterrupt, {Globals, Handle})),
            0u);
}

TEST_F(DriverKernelFrameworkInterrupt,
       EnableFailureUnwindsPriorInterruptsAndPreservesStatus) {
  Assigned = 2;
  interrupt();
  interrupt();
  start();
  expectCallback(Before);
  expectCallback(Enable);
  expectCallback(Enable, windows::StatusUnsuccessful);
  expectCallback(Disable);
  EXPECT_TRUE(Connected.empty());
  expectCallback(Exit);
  expectCallback(Release);
  const auto Done = Model.takePnpCompletion();
  ASSERT_TRUE(Done);
  EXPECT_EQ(Done->Status, windows::StatusUnsuccessful);
  EXPECT_FALSE(Model.hasPendingGuestCall());
}

TEST_F(DriverKernelFrameworkInterrupt,
       InterruptWithoutPowerCallbacksStillConnectsAndDisconnects) {
  configureInterrupt(false);
  interrupt();
  start();
  expectCallback(Before);
  EXPECT_EQ(Connected.size(), 1u);
  expectCallback(After);
  ASSERT_TRUE(Model.takePnpCompletion());
  EXPECT_TRUE(take(Model.beginPnpPowerTransition(
      PDO, InitSlot, DevicePnpRequest::Stop, 0, 0, 0)));
  EXPECT_TRUE(Connected.empty());
  expectCallback(Exit);
  expectCallback(Release);
  ASSERT_TRUE(Model.takePnpCompletion());
}

TEST_F(DriverKernelFrameworkInterrupt,
       InactivePowerDownRetainsIdentityAndReactivatesBeforeEnable) {
  put(InterruptConfig + InterruptReportInactive, InterruptTriTrue, 4);
  const auto Handle = interrupt();
  start();
  drain();
  ASSERT_TRUE(Model.takePnpCompletion());
  const auto Connection =
      take(invoke(api::WdfInterruptWdmGetInterrupt, {Globals, Handle}));
  ASSERT_NE(Connection, 0u);
  EXPECT_TRUE(take(Model.beginDevicePowerTransition(
      PDO, InitSlot, DevicePowerState::D0, DevicePowerState::D3)));
  expectCallback(Disable);
  EXPECT_EQ(Connected, (std::set<uint64_t>{Connection}));
  EXPECT_EQ(Inactive, Connected);
  expectCallback(Exit);
  ASSERT_TRUE(Model.takePnpCompletion());
  EXPECT_EQ(take(invoke(api::WdfInterruptWdmGetInterrupt, {Globals, Handle})),
            Connection);
  EXPECT_TRUE(take(Model.beginDevicePowerTransition(
      PDO, InitSlot, DevicePowerState::D3, DevicePowerState::D0)));
  expectCallback(Before);
  EXPECT_TRUE(Inactive.empty());
  expectCallback(Enable);
  expectCallback(After);
  ASSERT_TRUE(Model.takePnpCompletion());
  EXPECT_EQ(take(invoke(api::WdfInterruptWdmGetInterrupt, {Globals, Handle})),
            Connection);
  EXPECT_TRUE(take(Model.beginPnpPowerTransition(
      PDO, InitSlot, DevicePnpRequest::Stop, 0, 0, 0)));
  expectCallback(Disable);
  EXPECT_TRUE(Connected.empty());
  expectCallback(Exit);
  expectCallback(Release);
  ASSERT_TRUE(Model.takePnpCompletion());
}

TEST_F(DriverKernelFrameworkInterrupt,
       StopFromInactivePowerStateRetiresConnectionBeforeReleaseHardware) {
  configureInterrupt(false);
  put(InterruptConfig + InterruptReportInactive, InterruptTriTrue, 4);
  interrupt();
  start();
  drain();
  ASSERT_TRUE(Model.takePnpCompletion());
  EXPECT_TRUE(take(Model.beginDevicePowerTransition(
      PDO, InitSlot, DevicePowerState::D0, DevicePowerState::D3)));
  drain();
  ASSERT_TRUE(Model.takePnpCompletion());
  EXPECT_EQ(Inactive, Connected);
  EXPECT_TRUE(take(Model.beginPnpPowerTransition(
      PDO, InitSlot, DevicePnpRequest::Stop, 0, 0, 0)));
  EXPECT_TRUE(Connected.empty());
  expectCallback(Release);
  ASSERT_TRUE(Model.takePnpCompletion());
}

TEST_F(DriverKernelFrameworkInterrupt,
       ActivityReportsRequireLiveConnectionAndDispatchOrLowerIRQL) {
  const auto Handle = interrupt();
  expectError(invoke(api::WdfInterruptReportInactive, {Globals, Handle}),
              "live connection");
  start();
  drain();
  ASSERT_TRUE(Model.takePnpCompletion());
  const auto Connection = *Connected.begin();
  take(invoke(api::WdfInterruptReportInactive, {Globals, Handle}, 2));
  EXPECT_EQ(Inactive, Connected);
  expectError(invoke(api::WdfInterruptReportActive, {Globals, Handle}, 3),
              "IRQL");
  EXPECT_EQ(Inactive, Connected);
  take(invoke(api::WdfInterruptReportActive, {Globals, Handle}, 2));
  EXPECT_TRUE(Inactive.empty());
  EXPECT_EQ(take(invoke(api::WdfInterruptWdmGetInterrupt, {Globals, Handle})),
            Connection);
  take(invoke(api::WdfInterruptDisable, {Globals, Handle}));
  expectError(invoke(api::WdfInterruptReportInactive, {Globals, Handle}),
              "state callback");
  expectCallback(Disable);
}

TEST_F(DriverKernelFrameworkInterrupt, ExcessObjectsRemainUnassigned) {
  interrupt();
  const auto Extra = interrupt();
  start();
  expectCallback(Before);
  expectCallback(Enable);
  expectCallback(After);
  ASSERT_TRUE(Model.takePnpCompletion());
  EXPECT_EQ(Connected.size(), 1u);
  EXPECT_EQ(take(invoke(api::WdfInterruptWdmGetInterrupt, {Globals, Extra})),
            0u);
}

TEST_F(DriverKernelFrameworkInterrupt,
       DeferredCallbackCoalescesAndDrainsBeforeD0Exit) {
  const auto Handle = interrupt();
  start();
  drain();
  ASSERT_TRUE(Model.takePnpCompletion());
  EXPECT_EQ(take(invoke(api::WdfInterruptQueueDpcForIsr, {Globals, Handle}, 5)),
            1u);
  EXPECT_EQ(take(invoke(api::WdfInterruptQueueDpcForIsr, {Globals, Handle}, 5)),
            0u);
  const auto Token = Deferred.at(Handle);
  EXPECT_TRUE(take(Model.beginPnpPowerTransition(
      PDO, InitSlot, DevicePnpRequest::Stop, 0, 0, 0)));
  expectCallback(Disable);
  EXPECT_FALSE(Model.hasPendingGuestCall());
  EXPECT_FALSE(Model.takePnpCompletion());
  // The framework retains its reference until return; the scheduler then
  // retires its independent active callback before resuming the power IRP.
  take(Model.finishGuestCall(Token, Sentinel));
  EXPECT_FALSE(Model.hasPendingGuestCall());
  Deferred.erase(Handle);
  success(Model.resumeInterruptDrain());
  expectCallback(Exit);
  expectCallback(Release);
  ASSERT_TRUE(Model.takePnpCompletion());
}

TEST_F(DriverKernelFrameworkInterrupt,
       InvalidConfigurationDoesNotPublishAnObject) {
  put(InterruptConfig + InterruptISR, 0);
  put(InterruptSlot, Sentinel);
  const auto Live = liveAllocations();
  EXPECT_EQ(take(create()), InvalidParameter);
  EXPECT_EQ(get(InterruptSlot), Sentinel);
  EXPECT_EQ(liveAllocations(), Live);
  configureInterrupt();
  put(InterruptConfig + InterruptAutomaticSerialization, 1, 1);
  put(InterruptConfig + InterruptDPC, 0);
  put(InterruptConfig + InterruptWorkItem, DPC);
  EXPECT_EQ(take(create()), IncompatibleExecutionLevel);
  EXPECT_EQ(liveAllocations(), Live);
}

TEST_F(DriverKernelFrameworkInterrupt,
       CreationAfterStartIsRejectedBeforeMutation) {
  start();
  drain();
  ASSERT_TRUE(Model.takePnpCompletion());
  const auto Live = liveAllocations();
  EXPECT_EQ(take(create()), InvalidDeviceState);
  EXPECT_EQ(liveAllocations(), Live);
}

TEST_F(DriverKernelFrameworkInterrupt, GetInfoEnforcesIRQLAndResourceLifetime) {
  const auto Handle = interrupt();
  put(InterruptInfo, InterruptInfoSize, 4);
  expectError(
      invoke(api::WdfInterruptGetInfo, {Globals, Handle, InterruptInfo}),
      "assigned hardware");
  start();
  drain();
  ASSERT_TRUE(Model.takePnpCompletion());
  take(invoke(api::WdfInterruptGetInfo, {Globals, Handle, InterruptInfo}, 2));
  EXPECT_EQ(get(InterruptInfo + InterruptInfoVector, 4), 65u);
  EXPECT_EQ(get(InterruptInfo + InterruptInfoIRQL, 1), 5u);
  expectError(
      invoke(api::WdfInterruptGetInfo, {Globals, Handle, InterruptInfo}, 5),
      "IRQL");
  EXPECT_EQ(take(invoke(api::WdfInterruptGetDevice, {Globals, Handle}, 5)),
            Device);
}

TEST_F(DriverKernelFrameworkInterrupt,
       SynchronizeReturnsOnlyActualBooleanByte) {
  const auto Handle = interrupt();
  start();
  drain();
  ASSERT_TRUE(Model.takePnpCompletion());
  take(invoke(api::WdfInterruptSynchronize, {Globals, Handle, ISR, Sentinel}));
  const auto Call = callback();
  EXPECT_EQ(Call.Arguments, (std::vector<uint64_t>{Handle, Sentinel}));
  const auto Result = take(Model.finishGuestCall(Call.Token, 0x123456ab));
  ASSERT_TRUE(Result);
  EXPECT_EQ(*Result, 0xabu);
}

TEST_F(DriverKernelFrameworkInterrupt, ConnectedObjectCannotBeDeleted) {
  const auto Handle = interrupt();
  start();
  drain();
  ASSERT_TRUE(Model.takePnpCompletion());
  expectError(invoke(api::WdfObjectDelete, {Globals, Handle}),
              "disconnected and drained");
  EXPECT_EQ(take(invoke(api::WdfInterruptGetDevice, {Globals, Handle})),
            Device);
}

TEST_F(DriverKernelFrameworkInterrupt,
       ExternalSpinLockCannotUseOrdinarySpinAcquisition) {
  constexpr uint64_t Storage = Driver + 0x6000;
  KernelFramework::LockHost Host;
  Host.Create = [Storage](bool Wait) -> llvm::Expected<uint64_t> {
    EXPECT_FALSE(Wait);
    return Storage;
  };
  Host.Destroy = [](uint64_t, bool) { return llvm::Error::success(); };
  Host.Acquire = [](uint64_t, bool,
                    std::optional<int64_t>) -> llvm::Expected<uint32_t> {
    ADD_FAILURE() << "external spin acquisition bypassed the interrupt";
    return 0;
  };
  Model.setLockHost(std::move(Host));
  EXPECT_EQ(take(invoke(api::WdfSpinLockCreate, {Globals, 0, InterruptSlot})),
            0u);
  const auto Lock = get(InterruptSlot);
  put(InterruptConfig + InterruptSpinLock, Lock);
  const auto Handle = interrupt();
  expectError(invoke(api::WdfSpinLockAcquire, {Globals, Lock}),
              "requires WdfInterrupt");
  expectError(invoke(api::WdfSpinLockRelease, {Globals, Lock}),
              "requires WdfInterrupt");
  EXPECT_EQ(take(invoke(api::WdfInterruptGetDevice, {Globals, Handle})),
            Device);
  put(InterruptConfig + InterruptSpinLock, 0);
  put(InterruptConfig + InterruptPassiveHandling, 1, 1);
  put(InterruptConfig + InterruptWaitLock, Lock);
  expectError(create(), "matching framework lock");
}

TEST_F(DriverKernelFrameworkInterrupt,
       ExternalLockDeletionRetainsStorageUntilLastInterruptIsDestroyed) {
  constexpr uint64_t Storage = Driver + 0x6000;
  bool Destroyed = false;
  KernelFramework::LockHost Host;
  Host.Create = [Storage](bool Wait) -> llvm::Expected<uint64_t> {
    EXPECT_TRUE(Wait);
    return Storage;
  };
  Host.CanDelete = [Storage](uint64_t Address, bool Wait) {
    EXPECT_EQ(Address, Storage);
    EXPECT_TRUE(Wait);
    return llvm::Error::success();
  };
  Host.Destroy = [&](uint64_t Address, bool Wait) {
    EXPECT_EQ(Address, Storage);
    EXPECT_TRUE(Wait);
    EXPECT_FALSE(Destroyed);
    Destroyed = true;
    return llvm::Error::success();
  };
  Model.setLockHost(std::move(Host));
  EXPECT_EQ(take(invoke(api::WdfWaitLockCreate, {Globals, 0, InterruptSlot})),
            0u);
  const auto Lock = get(InterruptSlot);
  put(InterruptConfig + InterruptPassiveHandling, 1, 1);
  put(InterruptConfig + InterruptWaitLock, Lock);
  const auto First = interrupt();
  const auto Second = interrupt();
  take(invoke(api::WdfObjectDelete, {Globals, Lock}));
  EXPECT_FALSE(Destroyed);
  expectError(invoke(api::WdfWaitLockAcquire, {Globals, Lock, 0}),
              "live matching");
  take(invoke(api::WdfObjectDelete, {Globals, First}));
  EXPECT_FALSE(Destroyed);
  take(invoke(api::WdfObjectDelete, {Globals, Second}));
  EXPECT_TRUE(Destroyed);
  expectError(Model.validateGuestAccess(Lock, HandleSize, false), "freed");
}

TEST_F(DriverKernelFrameworkInterrupt,
       DispatchDereferenceDefersPassiveDestroyAndRetainsItsContext) {
  type();
  attributes(Device, Type, 0, ChildDestroy);
  const auto Child = object(Attrs);
  const auto Context =
      take(invoke(api::WdfObjectGetTypedContextWorker, {Globals, Child, Type}));
  put(Context, Sentinel);
  take(invoke(api::WdfObjectReferenceActual, {Globals, Child, 0, 0, 0}));
  take(invoke(api::WdfObjectDelete, {Globals, Child}));
  EXPECT_FALSE(Model.hasPendingGuestCall());
  std::optional<KernelFramework::GuestCall> DeferredCall;
  KernelFramework::DeviceHost Host;
  Host.DeferCall = [&](const KernelFramework::GuestCall &Call,
                       uint64_t WdmDevice, uint8_t IRQL) {
    EXPECT_EQ(WdmDevice, FDO);
    EXPECT_EQ(IRQL, scheduler::PassiveLevel);
    DeferredCall = Call;
    return llvm::Error::success();
  };
  Model.setDeviceHost(std::move(Host));
  take(invoke(api::WdfObjectDereferenceActual, {Globals, Child, 0, 0, 0},
              scheduler::DispatchLevel));
  ASSERT_TRUE(DeferredCall);
  EXPECT_EQ(DeferredCall->PC, ChildDestroy);
  EXPECT_FALSE(Model.hasPendingGuestCall());
  EXPECT_EQ(get(Context), Sentinel);
  success(Model.validateGuestAccess(Context, 8, false));
  take(Model.finishGuestCall(DeferredCall->Token, 0));
  expectError(Model.validateGuestAccess(Context, 8, false), "freed");
}

} // namespace
} // namespace neverd::emulation
