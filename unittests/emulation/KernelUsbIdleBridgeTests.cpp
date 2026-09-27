//===- KernelUsbIdleBridgeTests.cpp - Actual USB idle packet ownership
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "unicorn/UnicornBackend.h"
#include "windows/DriverImage.h"
#include "windows/KernelModel.h"
#include "windows/WindowsKernelLayout.h"

#include <algorithm>
#include <array>
#include <map>

namespace neverd::emulation {
namespace {
using namespace windows;

enum class RequestMajor : uint8_t {
#define NEVERD_DRIVER_REQUEST_KIND(Name, Spelling, Value) Name = Value,
#include "neverd/emulation/DriverRequestKinds.def"
#undef NEVERD_DRIVER_REQUEST_KIND
};

class KernelUsbIdleBridge : public ::testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000;
  static constexpr uint64_t DispatchPC = 0x180001000;
  static constexpr uint64_t CompletionPC = DispatchPC + 0x100;
  static constexpr uint64_t IdlePC = DispatchPC + 0x200;
  static constexpr uint64_t WorkerPC = DispatchPC + 0x300;
  static constexpr uint32_t PoolTag = 0x49625355;
  static constexpr uint8_t CompletionFlags =
      StackInvokeOnSuccess | StackInvokeOnError | StackInvokeOnCancel;
  struct Device {
    uint64_t PDO = 0, FDO = 0;
    uint32_t File = 0;
  };
  struct Packet {
    uint64_t IRP = 0, Info = 0;
  };
  std::unique_ptr<UnicornBackend> Memory;
  KernelExportRegistry Exports;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;
  std::map<std::string, Device> Devices;

  void success(llvm::Error E) {
    if (E)
      ADD_FAILURE() << llvm::toString(std::move(E));
  }
  template <class T> T take(llvm::Expected<T> Value) {
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return {};
    }
    return std::move(*Value);
  }
  void rejected(llvm::Error E, llvm::StringRef Text) {
    ASSERT_TRUE(bool(E));
    const auto Message = llvm::toString(std::move(E));
    EXPECT_NE(Message.find(Text.str()), std::string::npos) << Message;
  }
  template <class T>
  void rejected(llvm::Expected<T> Value, llvm::StringRef Text) {
    ASSERT_FALSE(bool(Value));
    rejected(Value.takeError(), Text);
  }
  uint64_t call(const char *Name, std::initializer_list<uint64_t> Args) {
    return take(Model->call(Name, Args));
  }
  uint64_t get(uint64_t Address, unsigned Width = 8) {
    return take(Memory->readInteger(Address, Width));
  }
  void put(uint64_t Address, uint64_t Value, unsigned Width = 8) {
    success(Model->validateGuestAccess(Address, Width, true));
    success(Memory->writeInteger(Address, Value, Width));
  }
  DriverPnpDevice configuration(llvm::StringRef ID, DriverUsbIdleRole Role) {
    DriverPnpDevice Config;
    Config.ID = ID.str();
    Config.InitialDevicePower = DevicePowerState::D0;
    Config.InitialSystemPower = SystemPowerState::Working;
    Config.UsbIdle = DriverUsbIdleConfig{Role, false};
    DriverPowerOperation D0;
    D0.State = uint32_t(DevicePowerState::D0);
    D0.BusCompletion = {StatusSuccess, 5};
    Config.RequestedDevicePower.push_back(D0);
    return Config;
  }
  virtual std::vector<DriverPnpDevice> configuration() {
    return {configuration("port", DriverUsbIdleRole::IndependentFunction)};
  }
  void copyDown(uint64_t IRP) {
    const auto Stack = get(IRP + IRPStackPointerOffset);
    std::array<uint8_t, StackCompletionOffset> Prefix{};
    success(Memory->read(Stack, Prefix));
    success(Memory->write(Stack - StackSize, Prefix));
    put(Stack - StackSize + StackControlOffset, 0, 1);
  }
  void completeForeground(uint64_t IRP) {
    put(IRP + IRPStatusOffset, StatusSuccess, 4);
    put(IRP + IRPInformationOffset, 0);
    EXPECT_EQ(call("IofCompleteRequest", {IRP, 0}), 0u);
    success(Model->recordDispatchReturn(IRP, StatusSuccess));
    success(Model->finalizeRequest(IRP));
  }
  void SetUp() override {
    Memory = take(UnicornBackend::create(8 * 1024 * 1024));
    ASSERT_TRUE(Memory);
    success(Memory->map(Scratch, 0x10000, Read | Write));
    success(Memory->map(DispatchPC, profile::PageSize, Read | Execute));
    DriverOptions Options;
    Options.PnpDevices = configuration();
    success(Exports.initialize(Options));
    Model = std::make_unique<KernelModel>(*Memory, Result, &Exports);
    DriverImage Image;
    Image.Base = DispatchPC - profile::PageSize;
    Image.Entry = DispatchPC;
    Image.Size = 3 * profile::PageSize;
    success(Model->initialize(Image, Options));
    Model->enterForeground();
    Model->enterExecution(1);
    for (auto Major : {RequestMajor::Pnp, RequestMajor::Power,
                       RequestMajor::InternalDeviceControl,
                       RequestMajor::Create, RequestMajor::DeviceControl})
      put(Model->driverObject() + DriverDispatchOffset +
              uint8_t(Major) * profile::PointerSize,
          DispatchPC);
    put(get(Model->driverObject() + DriverExtensionOffset) +
            DriverAddDeviceOffset,
        DispatchPC);
    success(Model->finishEntry());
    success(Model->preparePnpDevices());
    for (const auto &Config : Options.PnpDevices) {
      auto &Device = Devices[Config.ID];
      Device.PDO = take(Model->beginAddDevice(Config.ID)).Argument1;
      ASSERT_NE(Device.PDO, 0u);
      EXPECT_EQ(call("IoCreateDevice", {Model->driverObject(), 0, 0,
                                        UnknownDeviceType, 0, 0, Scratch}),
                StatusSuccess);
      Device.FDO = get(Scratch);
      put(Device.FDO + DeviceFlagsOffset,
          DeviceBufferedIO | DevicePowerPageable, 4);
      EXPECT_EQ(call("IoAttachDeviceToDeviceStack", {Device.FDO, Device.PDO}),
                Device.PDO);
      success(Model->finishAddDevice(Config.ID, StatusSuccess));
      DriverRequest Start;
      Start.Kind = DriverRequestKind::Pnp;
      Start.DeviceID = Config.ID;
      Start.Pnp =
          DriverPnpOperation{DevicePnpRequest::Start, {StatusSuccess, 0}};
      const auto IRP = take(Model->beginRequest(Start)).IRP;
      ASSERT_NE(IRP, 0u);
      copyDown(IRP);
      EXPECT_EQ(call("IofCallDriver", {Device.PDO, IRP}), StatusSuccess);
      success(Model->recordDispatchReturn(IRP, StatusSuccess));
      success(Model->finalizeRequest(IRP));
      DriverRequest Create;
      Create.Kind = DriverRequestKind::Create;
      Create.DeviceID = Config.ID;
      Device.File = Create.File = uint32_t(Devices.size());
      completeForeground(take(Model->beginRequest(Create)).IRP);
    }
  }
  uint64_t nextStack(uint64_t IRP) {
    return get(IRP + IRPStackPointerOffset) - StackSize;
  }
  Packet packet(uint64_t Context = 0) {
    Packet P;
    P.Info =
        call("ExAllocatePoolWithTag", {0, usb_idle::CallbackInfoSize, PoolTag});
    put(P.Info + usb_idle::CallbackOffset, IdlePC);
    put(P.Info + usb_idle::ContextOffset, Context);
    P.IRP = call("IoAllocateIrp", {1, false});
    const auto Stack = nextStack(P.IRP);
    put(Stack, uint8_t(RequestMajor::InternalDeviceControl), 1);
    put(Stack + StackIOControlOffset, usb_idle::SubmitIdleNotification, 4);
    put(Stack + StackInputLengthOffset, usb_idle::CallbackInfoSize, 4);
    put(Stack + StackType3InputOffset, P.Info);
    put(Stack + StackControlOffset, CompletionFlags, 1);
    put(Stack + StackCompletionOffset, CompletionPC);
    put(Stack + StackCompletionContextOffset, P.Info);
    return P;
  }
  void submit(Packet P, llvm::StringRef ID = "port") {
    EXPECT_EQ(call("IoCallDriver", {Devices.at(ID.str()).PDO, P.IRP}),
              StatusPending);
    EXPECT_FALSE(Model->takeGuestCall());
  }
  DriverRequestResult &observation(uint64_t IRP) {
    auto Found = std::find_if(Result.Requests.begin(), Result.Requests.end(),
                              [&](const auto &R) { return R.IRP == IRP; });
    EXPECT_NE(Found, Result.Requests.end());
    if (Found != Result.Requests.end())
      return *Found;
    static DriverRequestResult Missing;
    return Missing;
  }
  std::optional<KernelGuestCall> pending(uint64_t PC) {
    auto Call = Model->takeGuestCall();
    EXPECT_TRUE(Call);
    if (!Call)
      return std::nullopt;
    EXPECT_EQ(Call->PC, PC);
    Model->enterExecution(2, 1, Call->Token);
    return Call;
  }
  void finish(const KernelGuestCall &Call, uint64_t Value,
              uint64_t Expected = 0) {
    Model->enterExecution(2, 1, Call.Token);
    const auto Returned = take(Model->finishGuestCall(Call.Token, Value));
    ASSERT_TRUE(Returned);
    EXPECT_EQ(*Returned, Expected);
  }
  void dispose(Packet P, const KernelGuestCall &Completion,
               uint64_t ExpectedReturn = 0) {
    EXPECT_EQ(Completion.Arguments, (std::vector<uint64_t>{0, P.IRP, P.Info}));
    call("ExFreePoolWithTag", {P.Info, PoolTag});
    call("IoFreeIrp", {P.IRP});
    finish(Completion, StatusMoreProcessingRequired, ExpectedReturn);
    EXPECT_TRUE(observation(P.IRP).Completed);
    rejected(Model->validateGuestAccess(P.IRP, 1, false), "freed");
    rejected(Model->validateGuestAccess(P.Info, 1, false), "freed");
  }
  void cancel(Packet P) {
    const auto Routine = get(P.IRP + IRPCancelRoutineOffset);
    ASSERT_NE(Routine, 0u);
    EXPECT_EQ(call("IoCancelIrp", {P.IRP}), 0u);
    auto Cancel = pending(Routine);
    ASSERT_TRUE(Cancel);
    const auto *Export = Exports.lookup(Cancel->PC);
    ASSERT_NE(Export, nullptr);
    success(Model->beginGuestCall(Cancel->Token));
    const auto Status = take(Model->call(*Export, Cancel->Arguments, nullptr));
    auto Complete = pending(CompletionPC);
    ASSERT_TRUE(Complete);
    dispose(P, *Complete);
    finish(*Cancel, Status, 1);
    Model->enterExecution(1);
    EXPECT_EQ(observation(P.IRP).IOStatus, StatusCancelled);
  }
  void permission(llvm::StringRef Coordinator = "port") {
    DriverRequest Observe;
    Observe.Kind = DriverRequestKind::DeviceControl;
    Observe.DeviceID = Coordinator.str();
    Observe.File = Devices.at(Coordinator.str()).File;
    Observe.PowerPolicyEvents.push_back(
        {0, Coordinator.str(), DriverPowerPolicyAction::UsbIdlePermission});
    const auto Invocation = take(Model->beginRequest(Observe));
    ASSERT_NE(Invocation.IRP, 0u);
    completeForeground(Invocation.IRP);
  }
  uint64_t queueWorker(uint64_t FDO) {
    const auto Work = call("IoAllocateWorkItem", {FDO});
    call("IoQueueWorkItem", {Work, WorkerPC, profile::DelayedWorkQueue, 0});
    return Work;
  }
};

TEST_F(KernelUsbIdleBridge, NoPermissionParksOnlyTheRealRetainedPacket) {
  const auto Context = call("ExAllocatePoolWithTag", {0, 32, PoolTag});
  const auto P = packet(Context);
  const auto Rows = Result.Requests.size();
  submit(P);
  ASSERT_EQ(Result.Requests.size(), Rows + 1);
  const auto &Observed = observation(P.IRP);
  ASSERT_TRUE(Observed.UsbIdle);
  EXPECT_EQ(Observed.DeviceID, "port");
  EXPECT_EQ(Observed.Origin, DriverRequestOrigin::DriverAllocatedIRP);
  EXPECT_EQ(Observed.DispatchStatus, StatusPending);
  EXPECT_EQ(Observed.UsbIdle->BusReceivedAt100ns, 0u);
  EXPECT_FALSE(Observed.UsbIdle->CallbackEnteredAt100ns);
  EXPECT_TRUE(Model->requestPending(P.IRP));
  EXPECT_FALSE(Model->requestPending(
      P.IRP, KernelModel::PendingRequestScope::ExcludePowerParked));
  EXPECT_FALSE(take(Model->nextScheduled(true)));
  rejected(Model->call("ExFreePoolWithTag", {P.Info, PoolTag}), "borrowed");
  rejected(Model->call("IoFreeIrp", {P.IRP}), "completion");
  EXPECT_EQ(get(P.Info + usb_idle::CallbackOffset), IdlePC);
  call("ExFreePoolWithTag", {Context, PoolTag});
  cancel(P);
  EXPECT_FALSE(Model->requestPending());
}

TEST_F(KernelUsbIdleBridge, InvalidSubmissionPreservesPacketCursorAndRows) {
  const auto P = packet();
  const auto Stack = nextStack(P.IRP);
  const auto Rows = Result.Requests.size();
  const auto Deny = [&](llvm::StringRef Text) {
    rejected(Model->call("IoCallDriver", {Devices.at("port").PDO, P.IRP}),
             Text);
    EXPECT_EQ(Result.Requests.size(), Rows);
    EXPECT_EQ(get(P.IRP + IRPLocationOffset, 1), 2u);
    EXPECT_EQ(nextStack(P.IRP), Stack);
    EXPECT_FALSE(Model->takeGuestCall());
  };
  put(Stack + StackInputLengthOffset, usb_idle::CallbackInfoSize - 1, 4);
  Deny("callback info");
  put(Stack + StackInputLengthOffset, usb_idle::CallbackInfoSize, 4);
  put(Stack + StackParametersOffset, 1, 4);
  Deny("callback info");
  put(Stack + StackParametersOffset, 0, 4);
  put(P.Info + usb_idle::CallbackOffset, 0);
  Deny("executable");
  put(P.Info + usb_idle::CallbackOffset, IdlePC);
  put(Stack + StackType3InputOffset, profile::UserArenaBase);
  Deny("kernel callback info");
  put(Stack + StackType3InputOffset, P.Info);
  put(Stack + StackIOControlOffset, usb_idle::SubmitIdleNotification + 4, 4);
  Deny("provider submission requires USB idle");
  put(Stack + StackIOControlOffset, usb_idle::SubmitIdleNotification, 4);
  put(Stack, uint8_t(RequestMajor::DeviceControl), 1);
  Deny("internal device control");
  put(Stack, uint8_t(RequestMajor::InternalDeviceControl), 1);
  EXPECT_EQ(call("KfRaiseIrql", {scheduler::DispatchLevel}), 0u);
  Deny("PASSIVE_LEVEL");
  call("KeLowerIrql", {scheduler::PassiveLevel});
  submit(P);
  cancel(P);
}

TEST_F(KernelUsbIdleBridge, DuplicateCompletionFreesOnlyTheSecondCallerPacket) {
  const auto First = packet();
  submit(First);
  const auto Second = packet();
  EXPECT_EQ(call("IoCallDriver", {Devices.at("port").PDO, Second.IRP}),
            StatusDeviceBusy);
  auto Complete = pending(CompletionPC);
  ASSERT_TRUE(Complete);
  EXPECT_FALSE(observation(First.IRP).Completed);
  dispose(Second, *Complete, StatusDeviceBusy);
  EXPECT_EQ(observation(Second.IRP).DispatchStatus, StatusDeviceBusy);
  EXPECT_EQ(observation(Second.IRP).IOStatus, StatusDeviceBusy);
  ASSERT_TRUE(observation(Second.IRP).UsbIdle);
  EXPECT_FALSE(observation(Second.IRP).UsbIdle->CompletionCause);
  Model->enterExecution(1);
  rejected(Model->call("ExFreePoolWithTag", {First.Info, PoolTag}), "borrowed");
  cancel(First);
}

TEST_F(KernelUsbIdleBridge, QueuedPermissionCancellationWithdrawsTheExactCall) {
  const auto P = packet();
  submit(P);
  permission();
  const auto Dpc = Scratch + 0x100;
  call("KeInitializeDpc", {Dpc, WorkerPC, 0});
  EXPECT_EQ(call("KeInsertQueueDpc", {Dpc, 0, 0}), 1u);
  const auto Next = take(Model->nextScheduled(false));
  ASSERT_TRUE(Next);
  EXPECT_EQ(Next->Kind, KernelScheduler::CallbackKind::DPC);
  ASSERT_EQ(Result.PowerPolicyEvents.size(), 1u);
  EXPECT_EQ(Result.PowerPolicyEvents[0].OccurredAt100ns, 0u);
  EXPECT_FALSE(observation(P.IRP).UsbIdle->CallbackEnteredAt100ns);
  EXPECT_TRUE(Model->requestPending(
      P.IRP, KernelModel::PendingRequestScope::ExcludePowerParked));
  cancel(P);
  EXPECT_EQ(call("KeGetCurrentIrql", {}), scheduler::DispatchLevel);
  success(Model->finishScheduled(Next->ID));
  EXPECT_FALSE(take(Model->nextScheduled(false)));
  EXPECT_FALSE(Model->requestPending());
  EXPECT_FALSE(observation(P.IRP).UsbIdle->CallbackEnteredAt100ns);
}

TEST_F(KernelUsbIdleBridge,
       PermissionUsesTheCapturedCallbackAndOpaqueNullContext) {
  const auto P = packet();
  submit(P);
  put(P.Info + usb_idle::CallbackOffset, WorkerPC);
  put(P.Info + usb_idle::ContextOffset, Scratch);
  permission();
  const auto Next = take(Model->nextScheduled(false));
  ASSERT_TRUE(Next);
  EXPECT_EQ(Next->Kind, KernelScheduler::CallbackKind::UsbIdle);
  EXPECT_EQ(Next->PC, IdlePC);
  EXPECT_EQ(Next->Arguments, (std::vector<uint64_t>{0}));
  EXPECT_EQ(Next->IRQL, scheduler::PassiveLevel);
  ASSERT_TRUE(observation(P.IRP).UsbIdle);
  EXPECT_EQ(observation(P.IRP).UsbIdle->CallbackEnteredAt100ns, 0u);
  rejected(Model->continueScheduled(Next->ID, 0), "D2 request completed");
  EXPECT_FALSE(observation(P.IRP).UsbIdle->CallbackReturnedAt100ns);
  rejected(Model->call("ExFreePoolWithTag", {P.Info, PoolTag}), "borrowed");
  rejected(Model->call("IoFreeIrp", {P.IRP}), "completion");
}

TEST_F(KernelUsbIdleBridge,
       D0ReceiptCompletesIdleBeforeTheRealPowerAcknowledgement) {
  const auto P = packet();
  submit(P);
  const auto &Device = Devices.at("port");
  EXPECT_EQ(
      call("PoRequestPowerIrp", {Device.FDO, uint8_t(DevicePowerRequest::Set),
                                 uint32_t(DevicePowerState::D0), 0, 0, 0}),
      StatusPending);
  auto Dispatch = pending(DispatchPC);
  ASSERT_TRUE(Dispatch);
  ASSERT_EQ(Dispatch->Arguments.size(), 2u);
  const auto PowerIRP = Dispatch->Arguments[1];
  copyDown(PowerIRP);
  call("PoCallDriver", {Device.PDO, PowerIRP});
  auto Complete = pending(CompletionPC);
  ASSERT_TRUE(Complete);
  dispose(P, *Complete, StatusPending);
  EXPECT_TRUE(observation(P.IRP).Completed);
  EXPECT_EQ(observation(P.IRP).IOStatus, StatusSuccess);
  EXPECT_EQ(observation(P.IRP).UsbIdle->CompletionCause,
            DriverUsbIdleCompletionCause::DeviceD0);
  EXPECT_FALSE(observation(PowerIRP).Power->BusCompletedAt100ns);
  finish(*Dispatch, StatusPending, StatusPending);
  Model->enterExecution(1);
  EXPECT_FALSE(take(Model->nextScheduled(true)));
  EXPECT_EQ(observation(PowerIRP).Power->BusCompletedAt100ns, 5u);
  EXPECT_TRUE(observation(PowerIRP).Completed);
  EXPECT_FALSE(Model->requestPending());
}

class KernelUsbCompositeBridge : public KernelUsbIdleBridge {
protected:
  std::vector<DriverPnpDevice> configuration() override {
    auto Parent = KernelUsbIdleBridge::configuration(
        "parent", DriverUsbIdleRole::CompositeParent);
    auto First = KernelUsbIdleBridge::configuration(
        "first", DriverUsbIdleRole::CompositeFunction);
    auto Second = KernelUsbIdleBridge::configuration(
        "second", DriverUsbIdleRole::CompositeFunction);
    First.ParentID = Second.ParentID = Parent.ID;
    return {Parent, First, Second};
  }
};

TEST_F(KernelUsbCompositeBridge,
       FullCallbackQueuePreservesEveryMemberAndEvent) {
  const auto First = packet(), Second = packet();
  submit(First, "first");
  submit(Second, "second");
  for (uint64_t I = 0; I + 1 < scheduler::DefaultMaxPendingCallbacks; ++I)
    ASSERT_NE(queueWorker(Devices.at("parent").FDO), 0u);
  permission("parent");
  ASSERT_EQ(Result.PowerPolicyEvents.size(), 1u);
  const auto &Event = Result.PowerPolicyEvents[0];
  ASSERT_EQ(Event.UsbIdleMembers.size(), 2u);
  EXPECT_EQ(Event.UsbIdleMembers[0].IRP, First.IRP);
  EXPECT_EQ(Event.UsbIdleMembers[1].IRP, Second.IRP);
  rejected(Model->nextScheduled(false), "pending callback limit");
  EXPECT_FALSE(Event.OccurredAt100ns);
  for (const auto &P : {First, Second}) {
    EXPECT_FALSE(observation(P.IRP).UsbIdle->CallbackEnteredAt100ns);
    EXPECT_FALSE(Model->requestPending(
        P.IRP, KernelModel::PendingRequestScope::ExcludePowerParked));
    rejected(Model->call("ExFreePoolWithTag", {P.Info, PoolTag}), "borrowed");
  }
  cancel(First);
  cancel(Second);
}

} // namespace
} // namespace neverd::emulation
