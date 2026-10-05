//===- KernelPoFxAPITests.cpp - Guest component power ABI execution ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Verify guest ABI decoding, scheduler ownership and blocking PoFx
/// continuations.
///
//===----------------------------------------------------------------------===//

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

#include <array>

namespace neverd::emulation {
namespace {
using namespace windows;

class KernelPoFxAPI : public ::testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000;
  static constexpr uint64_t Description = Scratch + 0x1000;
  static constexpr uint64_t IdleStates = Scratch + 0x2000;
  static constexpr uint64_t HandleSlot = Scratch + 0x3000;
  static constexpr uint64_t Context = Scratch + 0x4000;
  static constexpr uint64_t CallerThread = Scratch + 0x5000;
  static constexpr uint64_t DispatchPC = 0x180001000;
  static constexpr uint64_t ActivePC = 0x180001100;
  static constexpr uint64_t IdlePC = 0x180001200;
  static constexpr uint64_t StatePC = 0x180001300;
  static constexpr uint64_t RequiredPC = 0x180001400;
  static constexpr uint64_t NotRequiredPC = 0x180001500;
  std::unique_ptr<UnicornBackend> Memory;
  std::unique_ptr<KernelModel> Model;
  DriverResult Result;
  uint64_t PDO = 0, FDO = 0, Handle = 0;
  virtual bool startOnSetup() const { return true; }

  void success(llvm::Error Error) {
    if (Error)
      ADD_FAILURE() << llvm::toString(std::move(Error));
  }
  template <class T> T take(llvm::Expected<T> Value) {
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return {};
    }
    return std::move(*Value);
  }
  template <class T>
  void reject(llvm::Expected<T> Value, llvm::StringRef Text) {
    ASSERT_FALSE(bool(Value));
    EXPECT_NE(llvm::toString(Value.takeError()).find(Text.str()),
              std::string::npos);
  }
  void reject(llvm::Error Error, llvm::StringRef Text) {
    ASSERT_TRUE(bool(Error));
    EXPECT_NE(llvm::toString(std::move(Error)).find(Text.str()),
              std::string::npos);
  }
  uint64_t get(uint64_t Address, unsigned Width = 8) {
    return take(Memory->readInteger(Address, Width));
  }
  void put(uint64_t Address, uint64_t Value, unsigned Width = 8) {
    success(Model->validateGuestAccess(Address, Width, true));
    success(Memory->writeInteger(Address, Value, Width));
  }
  uint64_t call(llvm::StringRef Name,
                std::initializer_list<uint64_t> Arguments) {
    return take(Model->call(Name.str(), Arguments));
  }

  void SetUp() override {
    Memory = take(UnicornBackend::create(8 * 1024 * 1024));
    ASSERT_TRUE(Memory);
    success(Memory->map(Scratch, 0x10000, Read | Write));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = DispatchPC;
    Image.Size = 0x3000;
    DriverOptions Options;
    DriverPnpDevice Device;
    Device.ID = "pofx";
    Device.InitialDevicePower = DevicePowerState::D0;
    Device.InitialSystemPower = SystemPowerState::Working;
    Device.InitialReportedDevicePower = DevicePowerState::D0;
    Options.PnpDevices.push_back(Device);
    success(Model->initialize(Image, Options));
    put(get(Model->driverObject() + DriverExtensionOffset) +
            DriverAddDeviceOffset,
        DispatchPC);
    put(Model->driverObject() + DriverDispatchOffset + 0x1b * 8, DispatchPC);
    success(Model->finishEntry());
    success(Model->preparePnpDevices());
    PDO = take(Model->beginAddDevice("pofx")).Argument1;
    ASSERT_NE(PDO, 0u);
    EXPECT_EQ(call("IoCreateDevice", {Model->driverObject(), 0, 0,
                                      UnknownDeviceType, 0, 0, Scratch}),
              StatusSuccess);
    FDO = get(Scratch);
    put(FDO + DeviceFlagsOffset, DeviceBufferedIO | DevicePowerPageable, 4);
    EXPECT_EQ(call("IoAttachDeviceToDeviceStack", {FDO, PDO}), PDO);
    success(Model->finishAddDevice("pofx", StatusSuccess));
    if (startOnSetup()) {
      const uint64_t IRP = beginStart();
      EXPECT_EQ(call("IofCallDriver", {PDO, IRP}), StatusSuccess);
      success(Model->recordDispatchReturn(IRP, StatusSuccess));
      success(Model->finalizeRequest(IRP));
    }
    Model->enterExecution(CallerThread);
    initializeDescription();
  }

  uint64_t beginStart(uint32_t Status = StatusSuccess,
                      uint64_t Completion = 0) {
    DriverRequest Start;
    Start.Kind = DriverRequestKind::Pnp;
    Start.DeviceID = "pofx";
    Start.Pnp = DriverPnpOperation{DevicePnpRequest::Start, {Status, 0}};
    const uint64_t IRP = take(Model->beginRequest(Start)).IRP;
    const uint64_t Stack = get(IRP + IRPStackPointerOffset);
    std::array<uint8_t, StackCompletionOffset> Prefix;
    success(Memory->read(Stack, Prefix));
    success(Memory->write(Stack - StackSize, Prefix));
    put(Stack - StackSize + StackControlOffset,
        Completion ? StackInvokeOnSuccess | StackInvokeOnError : 0, 1);
    put(Stack - StackSize + StackCompletionOffset, Completion);
    return IRP;
  }

  void initializeDescription() {
    std::array<uint8_t, pofx::DeviceComponents + pofx::ComponentSize> Bytes{};
    success(Memory->write(Description, Bytes));
    put(Description + pofx::DeviceVersion, pofx::Version1, 4);
    put(Description + pofx::DeviceComponentCount, 1, 4);
    put(Description + pofx::DeviceActiveCondition, ActivePC);
    put(Description + pofx::DeviceIdleCondition, IdlePC);
    put(Description + pofx::DeviceIdleState, StatePC);
    put(Description + pofx::DeviceContext, Context);
    const uint64_t Component = Description + pofx::DeviceComponents;
    put(Component + pofx::ComponentIdleStateCount, 2, 4);
    put(Component + pofx::ComponentDeepestWakeableState, 1, 4);
    put(Component + pofx::ComponentIdleStates, IdleStates);
    put(IdleStates + pofx::IdleStateTransitionLatency, 0);
    put(IdleStates + pofx::IdleStateResidencyRequirement, 0);
    put(IdleStates + pofx::IdleStateNominalPower, 100, 4);
    put(IdleStates + pofx::IdleStateSize + pofx::IdleStateTransitionLatency,
        10);
    put(IdleStates + pofx::IdleStateSize + pofx::IdleStateResidencyRequirement,
        20);
    put(IdleStates + pofx::IdleStateSize + pofx::IdleStateNominalPower, 50, 4);
  }

  void registerDevice() {
    EXPECT_EQ(call("PoFxRegisterDevice", {PDO, Description, HandleSlot}),
              StatusSuccess);
    Handle = get(HandleSlot);
    ASSERT_NE(Handle, 0u);
  }
  std::vector<uint64_t> callbackArguments(uint64_t PC,
                                          std::optional<uint32_t> State) {
    std::vector<uint64_t> Arguments{Context};
    if (PC != RequiredPC && PC != NotRequiredPC)
      Arguments.push_back(0);
    if (State)
      Arguments.push_back(*State);
    return Arguments;
  }
  KernelScheduler::Invocation scheduled(uint64_t PC,
                                        std::optional<uint32_t> State = {}) {
    const auto Call = take(Model->nextScheduled(false));
    EXPECT_TRUE(Call);
    if (!Call)
      return {};
    EXPECT_EQ(Call->Kind, KernelScheduler::CallbackKind::PoFx);
    EXPECT_EQ(Call->PC, PC);
    EXPECT_EQ(Call->IRQL, scheduler::PassiveLevel);
    EXPECT_EQ(Call->Arguments, callbackArguments(PC, State));
    Model->enterExecution(Call->ID, Call->ID);
    return *Call;
  }
  void finish(const KernelScheduler::Invocation &Call) {
    EXPECT_FALSE(take(Model->continueScheduled(Call.ID, UINT64_MAX)));
    success(Model->finishScheduled(Call.ID));
    Model->enterForeground();
    Model->enterExecution(CallerThread);
  }
  void startIdle() {
    registerDevice();
    call("PoFxStartDevicePowerManagement", {Handle});
    const auto Call = scheduled(IdlePC);
    call("PoFxCompleteIdleCondition", {Handle, 0});
    finish(Call);
  }
  KernelGuestCall inlineCall(uint64_t PC, std::optional<uint32_t> State = {}) {
    auto Call = Model->takeGuestCall();
    EXPECT_TRUE(Call);
    if (!Call)
      return {};
    EXPECT_EQ(Call->Token.Owner, GuestCallOwner::PoFx);
    EXPECT_EQ(Call->PC, PC);
    EXPECT_EQ(Call->Arguments, callbackArguments(PC, State));
    success(Model->beginGuestCall(Call->Token));
    return *Call;
  }
  void armDecision(DriverPowerPolicyAction Action,
                   std::optional<uint32_t> State = {}) {
    put(Model->driverObject() + DriverDispatchOffset, DispatchPC);
    put(Model->driverObject() + DriverDispatchOffset + 0x0e * 8, DispatchPC);
    DriverRequest Request;
    Request.DeviceID = "pofx";
    auto Complete = [&](const DriverRequest &Input) {
      const auto Invocation = take(Model->beginRequest(Input));
      put(Invocation.IRP + IRPStatusOffset, StatusSuccess, 4);
      put(Invocation.IRP + IRPInformationOffset, 0);
      call("IofCompleteRequest", {Invocation.IRP, 0});
      success(Model->recordDispatchReturn(Invocation.IRP, StatusSuccess));
      success(Model->finalizeRequest(Invocation.IRP));
    };
    Request.Kind = DriverRequestKind::Create;
    Complete(Request);
    Request.Kind = DriverRequestKind::DeviceControl;
    DriverPowerPolicyEvent Event{0, "pofx", Action};
    if (State) {
      Event.Component = 0;
      Event.State = State;
    }
    Request.PowerPolicyEvents.push_back(Event);
    Complete(Request);
    Model->enterExecution(CallerThread);
  }
};

class KernelPoFxRegistrationAPI : public KernelPoFxAPI {
protected:
  bool startOnSetup() const override { return false; }
};

class KernelPoFxPoweredDownAPI : public KernelPoFxAPI {};

TEST_F(KernelPoFxRegistrationAPI, RegistrationWaitsForSuccessfulLowerStart) {
  put(HandleSlot, UINT64_MAX);
  reject(Model->call("PoFxRegisterDevice", {PDO, Description, HandleSlot}),
         "successful START");
  EXPECT_EQ(get(HandleSlot), UINT64_MAX);
  const uint64_t IRP = beginStart(StatusSuccess, ActivePC);
  reject(Model->call("PoFxRegisterDevice", {PDO, Description, HandleSlot}),
         "successful START");
  EXPECT_EQ(get(HandleSlot), UINT64_MAX);
  call("IofCallDriver", {PDO, IRP});
  auto Completion = Model->takeGuestCall();
  ASSERT_TRUE(Completion);
  ASSERT_EQ(Completion->Token.Owner, GuestCallOwner::WDM);
  success(Model->beginGuestCall(Completion->Token));
  registerDevice();
  call("PoFxUnregisterDevice", {Handle});
  take(Model->finishGuestCall(Completion->Token, StatusMoreProcessingRequired));
  call("IofCompleteRequest", {IRP, 0});
  success(Model->recordDispatchReturn(IRP, StatusSuccess));
  success(Model->finalizeRequest(IRP));
}

TEST_F(KernelPoFxRegistrationAPI, FailedLowerStartCannotAuthorizeRegistration) {
  const uint64_t IRP = beginStart(StatusUnsuccessful, ActivePC);
  call("IofCallDriver", {PDO, IRP});
  auto Completion = Model->takeGuestCall();
  ASSERT_TRUE(Completion);
  success(Model->beginGuestCall(Completion->Token));
  put(HandleSlot, UINT64_MAX);
  reject(Model->call("PoFxRegisterDevice", {PDO, Description, HandleSlot}),
         "successful START");
  EXPECT_EQ(get(HandleSlot), UINT64_MAX);
  take(Model->finishGuestCall(Completion->Token, StatusSuccess));
  success(Model->recordDispatchReturn(IRP, StatusUnsuccessful));
  success(Model->finalizeRequest(IRP));
}

TEST_F(KernelPoFxPoweredDownAPI, StartedDeviceMustBePhysicallyPoweredOn) {
  put(Model->driverObject() + DriverDispatchOffset + 0x16 * 8, DispatchPC);
  DriverRequest Power;
  Power.Kind = DriverRequestKind::Power;
  Power.DeviceID = "pofx";
  DriverPowerOperation Operation;
  Operation.State = uint32_t(DevicePowerState::D3);
  Operation.BusCompletion = {StatusSuccess, 0};
  Power.Power = Operation;
  const uint64_t IRP = take(Model->beginRequest(Power)).IRP;
  const uint64_t Stack = get(IRP + IRPStackPointerOffset);
  std::array<uint8_t, StackCompletionOffset> Prefix;
  success(Memory->read(Stack, Prefix));
  success(Memory->write(Stack - StackSize, Prefix));
  put(Stack - StackSize + StackControlOffset, 0, 1);
  call("IofCallDriver", {PDO, IRP});
  success(Model->recordDispatchReturn(IRP, StatusSuccess));
  success(Model->finalizeRequest(IRP));
  put(HandleSlot, UINT64_MAX);
  reject(Model->call("PoFxRegisterDevice", {PDO, Description, HandleSlot}),
         "running D0");
  EXPECT_EQ(get(HandleSlot), UINT64_MAX);
}

TEST_F(KernelPoFxAPI,
       CentralDispatchDeclaresTheCompleteSupportedArgumentCounts) {
#define NEVERD_KERNEL_POFX_API(Name, Arity, IRQL, Operation)                   \
  EXPECT_EQ(KernelModel::argumentCount(#Name), Arity);
#include "os/windows/kernel/KernelPoFxAPIs.def"
#undef NEVERD_KERNEL_POFX_API
}

TEST_F(KernelPoFxAPI, CurrentThreadIdentityFollowsTheCallerAcrossNestedFrames) {
  const uint64_t Object = take(Model->currentThreadObject());
  ASSERT_NE(Object, 0u);
  EXPECT_NE(Object, CallerThread);
  EXPECT_EQ(take(Model->currentThreadObject()), Object);
  Model->enterExecution(CallerThread + 0x100, CallerThread);
  EXPECT_EQ(take(Model->currentThreadObject()), Object);
  Model->enterExecution(CallerThread + 0x200, CallerThread + 0x200);
  const uint64_t Other = take(Model->currentThreadObject());
  EXPECT_NE(Other, Object);
  Model->enterExecution(CallerThread);
  EXPECT_EQ(take(Model->currentThreadObject()), Object);
  reject(Model->validateGuestAccess(Object, 1, false), "opaque thread");
  reject(Model->validateGuestAccess(Object, 1, true), "opaque thread");
  reject(Model->validateGuestAccess(profile::ProcessorEnvironmentBase +
                                        windows::GSCurrentThreadOffset,
                                    profile::PointerSize, false),
         "processor");
  reject(Model->validateGuestAccess(Object + profile::ProcessTokenSize - 1, 1,
                                    false),
         "opaque thread");
  reject(Model->call("ObfDereferenceObject", {Object}), "referenced thread");
  Model->enterExecution(0);
  reject(Model->currentThreadObject(), "active execution");
}

TEST_F(KernelPoFxAPI, CurrentSystemThreadUsesItsExistingBorrowedObject) {
  call("PsCreateSystemThread",
       {Scratch, ThreadAllAccess, 0, 0, 0, DispatchPC, 0});
  const uint64_t ThreadHandle = get(Scratch);
  ASSERT_NE(ThreadHandle, 0u);
  call("ObReferenceObjectByHandle",
       {ThreadHandle, 0, 0, KernelMode, Scratch + 8, 0});
  const uint64_t Object = get(Scratch + 8);
  const auto Invocation = take(Model->nextScheduled(false));
  ASSERT_TRUE(Invocation);
  ASSERT_EQ(Invocation->Kind, KernelScheduler::CallbackKind::SystemThread);
  EXPECT_EQ(Invocation->Object, Object);
  Model->enterExecution(CallerThread + 0x100, Invocation->ID);
  EXPECT_EQ(take(Model->currentThreadObject()), Object);
  Model->enterExecution(CallerThread + 0x200, Invocation->ID);
  EXPECT_EQ(take(Model->currentThreadObject()), Object);
  call("ZwClose", {ThreadHandle});
  call("ObfDereferenceObject", {Object});
  EXPECT_EQ(take(Model->currentThreadObject()), Object);
  call("PsTerminateSystemThread", {StatusSuccess});
  EXPECT_EQ(Model->takeThreadTermination(), StatusSuccess);
  success(Model->finishScheduled(Invocation->ID));
  // A borrowed current-thread pointer adds no reference that survives exit.
  reject(Model->validateGuestAccess(Object, 1, false), "freed");
}

TEST_F(KernelPoFxAPI, RegistrationRequiresTheConfiguredPhysicalDeviceObject) {
  put(HandleSlot, UINT64_MAX);
  reject(Model->call("PoFxRegisterDevice", {FDO, Description, HandleSlot}),
         "exact configured PDO");
  EXPECT_EQ(get(HandleSlot), UINT64_MAX);
  registerDevice();
  reject(Model->validateGuestAccess(Handle, 8, false), "opaque PoFx");
  reject(Model->validateGuestAccess(Handle, 8, true), "opaque PoFx");
  call("PoFxSetComponentLatency", {Handle, 0, 10});
  reject(Model->call("PoFxRegisterDevice", {PDO, Description, HandleSlot}),
         "already registered");
  EXPECT_EQ(get(HandleSlot), Handle);
}

TEST_F(KernelPoFxAPI, MalformedRegistrationPreservesOutputAndCanBeRetried) {
  put(HandleSlot, UINT64_MAX);
  put(Description + pofx::DeviceVersion, pofx::Version1 + 1, 4);
  reject(Model->call("PoFxRegisterDevice", {PDO, Description, HandleSlot}),
         "version");
  EXPECT_EQ(get(HandleSlot), UINT64_MAX);
  initializeDescription();
  put(Description + pofx::DeviceComponentCount, 0, 4);
  reject(Model->call("PoFxRegisterDevice", {PDO, Description, HandleSlot}),
         "component count");
  EXPECT_EQ(get(HandleSlot), UINT64_MAX);
  initializeDescription();
  put(IdleStates + pofx::IdleStateTransitionLatency, 1);
  reject(Model->call("PoFxRegisterDevice", {PDO, Description, HandleSlot}),
         "F0");
  EXPECT_EQ(get(HandleSlot), UINT64_MAX);
  initializeDescription();
  registerDevice();
  EXPECT_FALSE(Model->hasPendingHardwareWork());
}

TEST_F(KernelPoFxAPI, UnwritableOutputCannotPublishRegistration) {
  constexpr uint64_t ReadOnlySlot = Scratch + 0x20000;
  success(Memory->map(ReadOnlySlot, 0x1000, Read));
  auto Rejected =
      Model->call("PoFxRegisterDevice", {PDO, Description, ReadOnlySlot});
  ASSERT_FALSE(bool(Rejected));
  llvm::consumeError(Rejected.takeError());
  registerDevice();
  EXPECT_FALSE(Model->hasPendingHardwareWork());
}

TEST_F(KernelPoFxAPI, RegistrationDeepCopySurvivesGuestDescriptionChanges) {
  registerDevice();
  put(Description + pofx::DeviceIdleCondition, UINT64_MAX);
  put(Description + pofx::DeviceComponents + pofx::ComponentIdleStates,
      UINT64_MAX);
  put(Description + pofx::DeviceContext, UINT64_MAX);
  call("PoFxStartDevicePowerManagement", {Handle});
  const auto Call = scheduled(IdlePC);
  call("PoFxCompleteIdleCondition", {Handle, 0});
  finish(Call);
  call("PoFxUnregisterDevice", {Handle});
}

TEST_F(KernelPoFxAPI,
       CompletionCannotPrecedeScheduledEntryOrReleaseRunningOwner) {
  registerDevice();
  call("PoFxStartDevicePowerManagement", {Handle});
  EXPECT_TRUE(Model->hasPendingHardwareWork());
  reject(Model->call("PoFxCompleteIdleCondition", {Handle, 0}), "delivered");
  reject(Model->call("PoFxUnregisterDevice", {Handle}), "retained");
  const auto Call = scheduled(IdlePC);
  call("PoFxCompleteIdleCondition", {Handle, 0});
  reject(Model->call("PoFxCompleteIdleCondition", {Handle, 0}), "matching");
  reject(Model->call("PoFxUnregisterDevice", {Handle}), "retained");
  finish(Call);
  EXPECT_FALSE(Model->hasPendingHardwareWork());
  call("PoFxUnregisterDevice", {Handle});
  auto Access = Model->validateGuestAccess(Handle, sizeof(uint64_t), false);
  ASSERT_TRUE(bool(Access));
  llvm::consumeError(std::move(Access));
  reject(Model->call("PoFxStartDevicePowerManagement", {Handle}), "live");
}

TEST_F(KernelPoFxAPI,
       ReturnedCallbackRetainsRegistrationUntilExplicitCompletion) {
  registerDevice();
  call("PoFxStartDevicePowerManagement", {Handle});
  const auto Call = scheduled(IdlePC);
  finish(Call);
  EXPECT_TRUE(Model->hasPendingHardwareWork());
  EXPECT_FALSE(take(Model->nextScheduled(false)));
  reject(Model->call("PoFxUnregisterDevice", {Handle}), "retained");
  call("PoFxCompleteIdleCondition", {Handle, 0});
  EXPECT_FALSE(Model->hasPendingHardwareWork());
  call("PoFxUnregisterDevice", {Handle});
}

TEST_F(KernelPoFxAPI, NestedReferencesSuppressSpuriousConditionNotifications) {
  registerDevice();
  call("PoFxActivateComponent", {Handle, 0, pofx::FlagAsyncOnly});
  call("PoFxActivateComponent", {Handle, 0, pofx::FlagAsyncOnly});
  call("PoFxStartDevicePowerManagement", {Handle});
  EXPECT_FALSE(take(Model->nextScheduled(false)));
  call("PoFxIdleComponent", {Handle, 0, pofx::FlagAsyncOnly});
  EXPECT_FALSE(take(Model->nextScheduled(false)));
  call("PoFxIdleComponent", {Handle, 0, pofx::FlagAsyncOnly});
  const auto Call = scheduled(IdlePC);
  call("PoFxCompleteIdleCondition", {Handle, 0});
  finish(Call);
  reject(Model->call("PoFxIdleComponent", {Handle, 0, 0}),
         "matching activation");
}

TEST_F(KernelPoFxAPI, BlockingCallbacksStayOnTheCallingThreadAndWaitForReturn) {
  startIdle();
  call("PoFxActivateComponent", {Handle, 0, pofx::FlagBlocking});
  EXPECT_FALSE(Model->takeWait());
  auto Active = inlineCall(ActivePC);
  EXPECT_FALSE(take(Model->nextScheduled(false)));
  auto Returned = take(Model->finishGuestCall(Active.Token, UINT64_MAX));
  ASSERT_TRUE(Returned);
  EXPECT_EQ(*Returned, 0u);
  EXPECT_FALSE(Model->takeWait());
  call("PoFxIdleComponent", {Handle, 0, pofx::FlagBlocking});
  auto Idle = inlineCall(IdlePC);
  call("PoFxCompleteIdleCondition", {Handle, 0});
  // The nested completion acknowledges the child; it must not consume the
  // original API continuation or publish its wait before the child returns.
  EXPECT_FALSE(Model->takeWait());
  Returned = take(Model->finishGuestCall(Idle.Token, UINT64_MAX));
  ASSERT_TRUE(Returned);
  EXPECT_EQ(*Returned, 0u);
  EXPECT_FALSE(Model->takeWait());
  EXPECT_FALSE(Model->takeGuestCall());
  EXPECT_FALSE(take(Model->nextScheduled(false)));
}

TEST_F(KernelPoFxAPI,
       FlagsAndComponentFailuresDoNotConsumeActivationReferences) {
  startIdle();
  reject(Model->call("PoFxActivateComponent", {0, UINT32_MAX, 4}),
         "live PoFx handle");
  reject(Model->call("PoFxActivateComponent",
                     {Handle, 0, pofx::FlagBlocking | pofx::FlagAsyncOnly}),
         "invalid flags");
  reject(Model->call("PoFxActivateComponent", {Handle, 0, 4}), "invalid flags");
  reject(Model->call("PoFxActivateComponent", {Handle, 1, 0}),
         "component index");
  reject(Model->call("PoFxCompleteIdleState", {Handle, 0}), "no pending");
  reject(Model->call("PoFxIdleComponent", {Handle, 0, 0}),
         "matching activation");
  EXPECT_FALSE(Model->hasPendingHardwareWork());
  call("PoFxActivateComponent", {Handle, 0, pofx::FlagAsyncOnly});
  finish(scheduled(ActivePC));
  call("PoFxIdleComponent", {Handle, 0, pofx::FlagAsyncOnly});
  auto Idle = scheduled(IdlePC);
  call("PoFxCompleteIdleCondition", {Handle, 0});
  finish(Idle);
}

TEST_F(KernelPoFxAPI, BlockingIdleRetainsTheCallerUntilLateAcknowledgement) {
  registerDevice();
  call("PoFxActivateComponent", {Handle, 0, 0});
  call("PoFxStartDevicePowerManagement", {Handle});
  call("PoFxIdleComponent", {Handle, 0, pofx::FlagBlocking});
  const auto Idle = inlineCall(IdlePC);
  auto Returned = take(Model->finishGuestCall(Idle.Token, 0));
  ASSERT_TRUE(Returned);
  const auto Wait = Model->takeWait();
  ASSERT_TRUE(Wait);
  EXPECT_EQ(Wait->Type, KernelModel::Wait::Kind::PoFxIdle);
  EXPECT_EQ(Wait->Thread, CallerThread);
  EXPECT_FALSE(take(Model->pollWait(*Wait)));
  Model->enterExecution(CallerThread + 1);
  call("PoFxCompleteIdleCondition", {Handle, 0});
  EXPECT_FALSE(Model->takePoFxThreadCall(CallerThread + 1));
  auto Ready = take(Model->pollWait(*Wait));
  ASSERT_TRUE(Ready);
  EXPECT_EQ(*Ready, StatusSuccess);
  Model->enterExecution(CallerThread);
  call("PoFxUnregisterDevice", {Handle});
}

TEST_F(KernelPoFxAPI, RegistrationStartAndUnregisterEnforcePassiveIRQL) {
  registerDevice();
  EXPECT_EQ(call("KfRaiseIrql", {scheduler::DispatchLevel}),
            scheduler::PassiveLevel);
  reject(Model->call("PoFxStartDevicePowerManagement", {Handle}), "IRQL");
  reject(Model->call("PoFxUnregisterDevice", {Handle}), "IRQL");
  reject(Model->call("PoFxRegisterDevice", {PDO, Description, HandleSlot}),
         "IRQL");
  reject(Model->call("PoFxActivateComponent", {Handle, 0, pofx::FlagBlocking}),
         "below DISPATCH_LEVEL");
  call("PoFxSetComponentLatency", {Handle, 0, 100});
  call("PoFxSetComponentResidency", {Handle, 0, 1000});
  call("PoFxSetComponentWake", {Handle, 0, 1});
  call("KeLowerIrql", {scheduler::PassiveLevel});
  call("PoFxUnregisterDevice", {Handle});
}

TEST_F(KernelPoFxAPI, BlockingIdleWithAnotherReferenceReturnsWithoutAWait) {
  registerDevice();
  call("PoFxActivateComponent", {Handle, 0, 0});
  call("PoFxActivateComponent", {Handle, 0, 0});
  call("PoFxStartDevicePowerManagement", {Handle});
  call("PoFxIdleComponent", {Handle, 0, pofx::FlagBlocking});
  EXPECT_FALSE(Model->takeWait());
  EXPECT_FALSE(Model->takeGuestCall());
  EXPECT_FALSE(take(Model->nextScheduled(false)));
  call("PoFxIdleComponent", {Handle, 0, pofx::FlagBlocking});
  const auto Idle = inlineCall(IdlePC);
  call("PoFxCompleteIdleCondition", {Handle, 0});
  EXPECT_TRUE(take(Model->finishGuestCall(Idle.Token, 0)));
  EXPECT_FALSE(Model->takeWait());
  call("PoFxUnregisterDevice", {Handle});
}

TEST_F(KernelPoFxAPI,
       SupersedingAsyncActivationCannotHideTheCompletedIdleWait) {
  registerDevice();
  call("PoFxActivateComponent", {Handle, 0, 0});
  call("PoFxStartDevicePowerManagement", {Handle});
  call("PoFxIdleComponent", {Handle, 0, pofx::FlagBlocking});
  const auto Idle = inlineCall(IdlePC);
  EXPECT_TRUE(take(Model->finishGuestCall(Idle.Token, 0)));
  const auto Wait = Model->takeWait();
  ASSERT_TRUE(Wait);
  Model->enterExecution(CallerThread + 1);
  call("PoFxActivateComponent", {Handle, 0, pofx::FlagAsyncOnly});
  call("PoFxCompleteIdleCondition", {Handle, 0});
  EXPECT_FALSE(Model->takePoFxThreadCall(CallerThread));
  EXPECT_FALSE(Model->takePoFxThreadCall(CallerThread + 1));
  const auto Ready = take(Model->pollWait(*Wait));
  ASSERT_TRUE(Ready);
  EXPECT_EQ(*Ready, StatusSuccess);
  finish(scheduled(ActivePC));
  call("PoFxUnregisterDevice", {Handle});
}

TEST_F(KernelPoFxAPI,
       AsyncIdleWaitsForTheOriginalBlockingF0AndActiveCallbacks) {
  startIdle();
  armDecision(DriverPowerPolicyAction::ComponentIdleState, 1);
  const auto Enter = scheduled(StatePC, 1);
  call("PoFxCompleteIdleState", {Handle, 0});
  finish(Enter);
  call("PoFxActivateComponent", {Handle, 0, pofx::FlagBlocking});
  const auto F0 = inlineCall(StatePC, 0);
  EXPECT_TRUE(take(Model->finishGuestCall(F0.Token, 0)));
  const auto Wait = Model->takeWait();
  ASSERT_TRUE(Wait);
  Model->enterExecution(CallerThread + 1);
  call("PoFxIdleComponent", {Handle, 0, pofx::FlagAsyncOnly});
  call("PoFxCompleteIdleState", {Handle, 0});
  EXPECT_FALSE(Model->takePoFxThreadCall(CallerThread + 1));
  EXPECT_FALSE(take(Model->pollWait(*Wait)));
  Model->enterExecution(CallerThread);
  const auto Active = inlineCall(ActivePC);
  EXPECT_TRUE(take(Model->finishGuestCall(Active.Token, 0)));
  EXPECT_FALSE(Model->takeWait());
  EXPECT_FALSE(Model->takePoFxThreadCall(CallerThread));
  const auto Idle = scheduled(IdlePC);
  call("PoFxCompleteIdleCondition", {Handle, 0});
  finish(Idle);
  call("PoFxUnregisterDevice", {Handle});
}

TEST_F(KernelPoFxAPI,
       ImplicitActiveCompletionSurvivesAnImmediateOppositeCondition) {
  put(Description + pofx::DeviceActiveCondition, 0);
  put(Description + pofx::DeviceIdleCondition, 0);
  put(Description + pofx::DeviceIdleState, 0);
  put(Description + pofx::DevicePowerRequired, RequiredPC);
  put(Description + pofx::DevicePowerNotRequired, NotRequiredPC);
  put(Description + pofx::DeviceComponents + pofx::ComponentIdleStateCount, 1,
      4);
  put(Description + pofx::DeviceComponents +
          pofx::ComponentDeepestWakeableState,
      0, 4);
  registerDevice();
  call("PoFxStartDevicePowerManagement", {Handle});
  armDecision(DriverPowerPolicyAction::PowerNotRequired);
  const auto Idle = scheduled(NotRequiredPC);
  call("PoFxCompleteDevicePowerNotRequired", {Handle});
  finish(Idle);
  call("PoFxActivateComponent", {Handle, 0, pofx::FlagBlocking});
  const auto Required = inlineCall(RequiredPC);
  EXPECT_TRUE(take(Model->finishGuestCall(Required.Token, 0)));
  const auto Wait = Model->takeWait();
  ASSERT_TRUE(Wait);
  Model->enterExecution(CallerThread + 1);
  call("PoFxIdleComponent", {Handle, 0, pofx::FlagAsyncOnly});
  EXPECT_FALSE(take(Model->pollWait(*Wait)));
  call("PoFxReportDevicePoweredOn", {Handle});
  const auto Completed = take(Model->pollWait(*Wait));
  ASSERT_TRUE(Completed);
  EXPECT_EQ(*Completed, StatusSuccess);
  EXPECT_FALSE(Model->takePoFxThreadCall(CallerThread));
  EXPECT_FALSE(take(Model->nextScheduled(false)));
  Model->enterExecution(CallerThread);
  call("PoFxUnregisterDevice", {Handle});
}
} // namespace
} // namespace neverd::emulation
