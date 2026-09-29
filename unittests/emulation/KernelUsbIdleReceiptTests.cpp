//===- KernelUsbIdleReceiptTests.cpp - Nested USB provider receipts ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/DriverImage.h"
#include "os/windows/KernelExportRegistry.h"
#include "os/windows/KernelModel.h"
#include "os/windows/WindowsKernelLayout.h"

#include <algorithm>
#include <array>

namespace neverd::emulation {
namespace {
using namespace windows;

enum class RequestMajor : uint8_t {
#define NEVERD_DRIVER_REQUEST_KIND(Name, Spelling, Major) Name = Major,
#include "neverd/emulation/DriverRequestKinds.def"
#undef NEVERD_DRIVER_REQUEST_KIND
};

class KernelUsbIdleReceipt : public ::testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000;
  static constexpr uint64_t ImageBase = 0x180000000;
  static constexpr uint64_t DispatchPC = ImageBase + 0x1000;
  static constexpr uint64_t IdlePC = DispatchPC + 0x100;
  static constexpr uint64_t CompletionPC = DispatchPC + 0x200;
  static constexpr uint64_t PowerPC = DispatchPC + 0x300;
  static constexpr uint32_t PoolTag = 0x69627375;
  std::unique_ptr<UnicornBackend> Memory;
  std::unique_ptr<KernelModel> Model;
  KernelExportRegistry Exports;
  DriverResult Result;
  uint64_t PDO = 0, FDO = 0, Info = 0;
  std::vector<uint64_t> Children;

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
  uint64_t get(uint64_t Address, unsigned Width = 8) {
    return take(Memory->readInteger(Address, Width));
  }
  void put(uint64_t Address, uint64_t Value, unsigned Width = 8) {
    success(Model->validateGuestAccess(Address, Width, true));
    success(Memory->writeInteger(Address, Value, Width));
  }
  uint64_t call(const char *Name, std::initializer_list<uint64_t> Arguments) {
    return take(Model->call(Name, Arguments));
  }
  DriverRequestResult &result(uint64_t IRP) {
    auto Found = std::find_if(Result.Requests.begin(), Result.Requests.end(),
                              [&](const auto &R) { return R.IRP == IRP; });
    EXPECT_NE(Found, Result.Requests.end());
    return *Found;
  }
  DriverPowerOperation operation(DevicePowerState Target, uint64_t Delay = 0,
                                 uint32_t Status = StatusSuccess) {
    DriverPowerOperation Power;
    Power.State = uint32_t(Target);
    Power.BusCompletion = {Status, Delay};
    return Power;
  }
  void copyDown(uint64_t IRP) {
    const uint64_t Stack = get(IRP + IRPStackPointerOffset);
    std::array<uint8_t, StackCompletionOffset> Prefix{};
    success(Memory->read(Stack, Prefix));
    success(Memory->write(Stack - StackSize, Prefix));
    put(Stack - StackSize + StackControlOffset, 0, 1);
  }
  void initialize(std::vector<DriverPowerOperation> Responses = {},
                  bool Composite = false) {
    Memory = take(UnicornBackend::create(8 * 1024 * 1024));
    ASSERT_TRUE(Memory);
    success(Memory->map(Scratch, 0x10000, Read | Write));
    success(Memory->map(ImageBase, 0x3000, Read | Execute));
    DriverOptions Options;
    DriverPnpDevice Device;
    Device.ID = "usb";
    Device.InitialDevicePower = DevicePowerState::D0;
    Device.InitialSystemPower = SystemPowerState::Working;
    Device.InitialReportedDevicePower = DevicePowerState::D0;
    Device.UsbIdle = DriverUsbIdleConfig{};
    Device.RequestedDevicePower = std::move(Responses);
    if (Composite)
      Device.UsbIdle->Role = DriverUsbIdleRole::CompositeParent;
    Options.PnpDevices.push_back(Device);
    if (Composite)
      for (llvm::StringRef ID : {"usb-a", "usb-b"}) {
        auto Child = Device;
        Child.ID = ID.str();
        Child.ParentID = Device.ID;
        Child.UsbIdle->Role = DriverUsbIdleRole::CompositeFunction;
        Child.RequestedDevicePower.clear();
        Options.PnpDevices.push_back(std::move(Child));
      }
    success(Exports.initialize(Options));
    Model = std::make_unique<KernelModel>(*Memory, Result, &Exports);
    DriverImage Image;
    Image.Base = ImageBase;
    Image.Entry = DispatchPC;
    Image.Size = 0x3000;
    success(Model->initialize(Image, Options));
    Model->enterForeground();
    Model->enterExecution(profile::StackBase);
    for (auto Major :
         {RequestMajor::Create, RequestMajor::Pnp, RequestMajor::Power,
          RequestMajor::InternalDeviceControl})
      put(Model->driverObject() + DriverDispatchOffset +
              uint8_t(Major) * profile::PointerSize,
          DispatchPC);
    put(get(Model->driverObject() + DriverExtensionOffset) +
            DriverAddDeviceOffset,
        DispatchPC);
    success(Model->finishEntry());
    success(Model->preparePnpDevices());
    for (const auto &Config : Options.PnpDevices) {
      const uint64_t Provider =
          take(Model->beginAddDevice(Config.ID)).Argument1;
      EXPECT_EQ(call("IoCreateDevice", {Model->driverObject(), 0, 0,
                                        UnknownDeviceType, 0, 0, Scratch}),
                StatusSuccess);
      const uint64_t Function = get(Scratch);
      put(Function + DeviceFlagsOffset, DeviceBufferedIO | DevicePowerPageable,
          4);
      EXPECT_EQ(call("IoAttachDeviceToDeviceStack", {Function, Provider}),
                Provider);
      success(Model->finishAddDevice(Config.ID, StatusSuccess));
      DriverRequest Start;
      Start.Kind = DriverRequestKind::Pnp;
      Start.DeviceID = Config.ID;
      Start.Pnp =
          DriverPnpOperation{DevicePnpRequest::Start, {StatusSuccess, 0}};
      const uint64_t IRP = take(Model->beginRequest(Start)).IRP;
      copyDown(IRP);
      EXPECT_EQ(call("IofCallDriver", {Provider, IRP}), StatusSuccess);
      success(Model->recordDispatchReturn(IRP, StatusSuccess));
      success(Model->finalizeRequest(IRP));
      if (Config.ID == "usb") {
        PDO = Provider;
        FDO = Function;
      } else {
        Children.push_back(Provider);
      }
    }
  }
  uint64_t submit(uint64_t Device = 0) {
    Info =
        call("ExAllocatePoolWithTag", {0, usb_idle::CallbackInfoSize, PoolTag});
    EXPECT_NE(Info, 0u);
    put(Info + usb_idle::CallbackOffset, IdlePC);
    put(Info + usb_idle::ContextOffset, 0);
    const uint64_t IRP = call("IoAllocateIrp", {1, false});
    const uint64_t Stack = get(IRP + IRPStackPointerOffset) - StackSize;
    put(Stack, uint8_t(RequestMajor::InternalDeviceControl), 1);
    put(Stack + StackInputLengthOffset, usb_idle::CallbackInfoSize, 4);
    put(Stack + StackIOControlOffset, usb_idle::SubmitIdleNotification, 4);
    put(Stack + StackType3InputOffset, Info);
    put(Stack + StackCompletionOffset, CompletionPC);
    put(Stack + StackControlOffset,
        StackInvokeOnSuccess | StackInvokeOnError | StackInvokeOnCancel, 1);
    EXPECT_EQ(call("IofCallDriver", {Device ? Device : PDO, IRP}),
              StatusPending);
    EXPECT_FALSE(Model->takeGuestCall());
    EXPECT_EQ(result(IRP).DispatchStatus, StatusPending);
    return IRP;
  }
  uint64_t power(const DriverPowerOperation &Power) {
    DriverRequest Input;
    Input.Kind = DriverRequestKind::Power;
    Input.DeviceID = "usb";
    Input.Power = Power;
    const uint64_t IRP = take(Model->beginRequest(Input)).IRP;
    copyDown(IRP);
    EXPECT_EQ(call("PoCallDriver", {PDO, IRP}), StatusPending);
    return IRP;
  }
  KernelGuestCall idleCompletion(uint64_t IRP, uint32_t Status) {
    auto Call = Model->takeGuestCall();
    EXPECT_TRUE(Call);
    if (!Call)
      return {};
    EXPECT_EQ(Call->PC, CompletionPC);
    EXPECT_EQ(Call->Arguments[1], IRP);
    EXPECT_EQ(get(IRP + IRPStatusOffset, 4), Status);
    success(Model->beginGuestCall(Call->Token));
    Model->enterExecution(profile::CallbackStackBase, profile::StackBase,
                          Call->Token);
    return *Call;
  }
  void freeIdle(uint64_t IRP) {
    call("ExFreePoolWithTag", {Info, PoolTag});
    call("IoFreeIrp", {IRP});
  }
};

TEST_F(KernelUsbIdleReceipt,
       D0ReceiptCompletesIdleBeforeDelayedAcknowledgement) {
  initialize();
  const uint64_t Idle = submit();
  const uint64_t Power = power(operation(DevicePowerState::D0, 100));
  auto Complete = idleCompletion(Idle, StatusSuccess);
  ASSERT_TRUE(result(Power).Power);
  EXPECT_EQ(result(Power).Power->BusReceivedAt100ns, 0u);
  EXPECT_FALSE(result(Power).Power->BusCompletedAt100ns);
  EXPECT_EQ(result(Idle).UsbIdle->CompletionCause,
            UsbIdleCompletionCause::DeviceD0);
  freeIdle(Idle);
  EXPECT_FALSE(take(Model->nextScheduled(true, 25)));
  EXPECT_EQ(take(Model->finishGuestCall(Complete.Token,
                                        StatusMoreProcessingRequired)),
            StatusPending);
  Model->enterExecution(profile::StackBase);
  success(Model->recordDispatchReturn(Power, StatusPending));
  EXPECT_FALSE(take(Model->nextScheduled(true)));
  EXPECT_EQ(result(Power).Power->BusReceivedAt100ns, 0u);
  EXPECT_EQ(result(Power).Power->BusCompletedAt100ns, 100u);
  EXPECT_TRUE(result(Idle).Completed);
  EXPECT_TRUE(result(Power).Completed);
  success(Model->finalizeRequest(Power));
}

TEST_F(KernelUsbIdleReceipt, InvalidD0FailureDoesNotClaimTheIdleRegistration) {
  initialize();
  const uint64_t Idle = submit();
  DriverRequest Input;
  Input.Kind = DriverRequestKind::Power;
  Input.DeviceID = "usb";
  Input.Power = operation(DevicePowerState::D0, 0, StatusNotSupported);
  const uint64_t Power = take(Model->beginRequest(Input)).IRP;
  copyDown(Power);
  auto Rejected = Model->call("PoCallDriver", {PDO, Power});
  ASSERT_FALSE(bool(Rejected));
  EXPECT_NE(llvm::toString(Rejected.takeError()).find("must not fail"),
            std::string::npos);
  EXPECT_FALSE(result(Power).Power->BusReceivedAt100ns);
  EXPECT_FALSE(result(Idle).UsbIdle->CompletionCause);
  EXPECT_FALSE(result(Idle).Completed);
  EXPECT_NE(get(Idle + IRPCancelRoutineOffset), 0u);
  EXPECT_FALSE(Model->takeGuestCall());
  auto Free = Model->call("ExFreePoolWithTag", {Info, PoolTag});
  ASSERT_FALSE(bool(Free));
  EXPECT_NE(llvm::toString(Free.takeError()).find("borrowed"),
            std::string::npos);
}

TEST_F(KernelUsbIdleReceipt, D3ReceiptUsesPowerStateInvalidForIdlePacket) {
  initialize();
  const uint64_t Idle = submit();
  const uint64_t Power = power(operation(DevicePowerState::D3));
  auto Complete = idleCompletion(Idle, usb_idle::StatusPowerStateInvalid);
  freeIdle(Idle);
  EXPECT_EQ(take(Model->finishGuestCall(Complete.Token,
                                        StatusMoreProcessingRequired)),
            StatusSuccess);
  success(Model->recordDispatchReturn(Power, StatusSuccess));
  EXPECT_EQ(result(Idle).UsbIdle->CompletionCause,
            UsbIdleCompletionCause::DeviceD3);
  EXPECT_EQ(result(Power).Power->DeviceStateAfter, DevicePowerState::D3);
  success(Model->finalizeRequest(Power));
}

TEST_F(KernelUsbIdleReceipt, SystemSleepReceiptCancelsTheIdlePacket) {
  initialize();
  const uint64_t Idle = submit();
  auto Sleep = operation(DevicePowerState::D0);
  Sleep.Type = DriverPowerType::System;
  Sleep.State = uint32_t(SystemPowerState::Sleeping3);
  const uint64_t Power = power(Sleep);
  auto Complete = idleCompletion(Idle, StatusCancelled);
  freeIdle(Idle);
  EXPECT_EQ(take(Model->finishGuestCall(Complete.Token,
                                        StatusMoreProcessingRequired)),
            StatusSuccess);
  success(Model->recordDispatchReturn(Power, StatusSuccess));
  EXPECT_EQ(result(Idle).UsbIdle->CompletionCause,
            UsbIdleCompletionCause::SystemSleep);
  EXPECT_EQ(result(Power).Power->SystemStateAfter, SystemPowerState::Sleeping3);
  success(Model->finalizeRequest(Power));
}

TEST_F(KernelUsbIdleReceipt, HeldIdleMPRDoesNotDelayTheOriginalPowerReturn) {
  initialize();
  const uint64_t Idle = submit();
  const uint64_t Power = power(operation(DevicePowerState::D0));
  auto Complete = idleCompletion(Idle, StatusSuccess);
  EXPECT_EQ(take(Model->finishGuestCall(Complete.Token,
                                        StatusMoreProcessingRequired)),
            StatusSuccess);
  success(Model->recordDispatchReturn(Power, StatusSuccess));
  EXPECT_TRUE(result(Power).Completed);
  EXPECT_TRUE(result(Idle).Completed);
  Model->enterExecution(profile::StackBase);
  freeIdle(Idle);
  success(Model->finalizeRequest(Power));
}

TEST_F(KernelUsbIdleReceipt,
       ScheduledProviderTransfersToIdleThenPowerCompletion) {
  initialize({operation(DevicePowerState::D0)});
  call("IoDetachDevice", {PDO});
  call("IoDeleteDevice", {FDO});
  const uint64_t Idle = submit();
  const uint64_t Old = call("KfRaiseIrql", {scheduler::DispatchLevel});
  EXPECT_EQ(call("PoRequestPowerIrp",
                 {PDO, uint8_t(DevicePowerRequest::Set),
                  uint32_t(DevicePowerState::D0), PowerPC, 0, 0}),
            StatusPending);
  const uint64_t Power = Result.Requests.back().IRP;
  EXPECT_FALSE(result(Power).DispatchStatus);
  call("KeLowerIrql", {Old});
  auto Next = take(Model->nextScheduled(false));
  ASSERT_TRUE(Next);
  EXPECT_EQ(Next->Kind, KernelScheduler::CallbackKind::WDMCompletion);
  EXPECT_EQ(Next->PC, CompletionPC);
  EXPECT_EQ(Next->Arguments[1], Idle);
  EXPECT_FALSE(result(Power).DispatchStatus);
  const auto Token = Model->scheduledGuestCall(Next->ID);
  success(Model->beginGuestCall(Token));
  Model->enterExecution(profile::CallbackStackBase, Next->ID, Token);
  freeIdle(Idle);
  auto Continued =
      take(Model->continueScheduled(Next->ID, StatusMoreProcessingRequired));
  ASSERT_TRUE(Continued);
  EXPECT_EQ(Continued->PC, PowerPC);
  EXPECT_NE(Continued->Token.ID, Token.ID);
  EXPECT_EQ(result(Power).DispatchStatus, StatusSuccess);
  EXPECT_EQ(result(Power).Power->BusReceivedAt100ns, 0u);
  EXPECT_EQ(result(Power).Power->BusCompletedAt100ns, 0u);
  success(Model->beginGuestCall(Continued->Token));
  Model->enterExecution(profile::CallbackStackBase, Next->ID, Continued->Token);
  EXPECT_FALSE(take(Model->continueScheduled(Next->ID, 0)));
  success(Model->finishScheduled(Next->ID));
  EXPECT_FALSE(Model->requestPending());
}

TEST_F(KernelUsbIdleReceipt,
       ScheduledProviderRetainsItsOriginalDelayedDeadline) {
  initialize({operation(DevicePowerState::D0, 100)});
  call("IoDetachDevice", {PDO});
  call("IoDeleteDevice", {FDO});
  const uint64_t Idle = submit();
  const uint64_t Old = call("KfRaiseIrql", {scheduler::DispatchLevel});
  EXPECT_EQ(
      call("PoRequestPowerIrp", {PDO, uint8_t(DevicePowerRequest::Set),
                                 uint32_t(DevicePowerState::D0), 0, 0, 0}),
      StatusPending);
  const uint64_t Power = Result.Requests.back().IRP;
  call("KeLowerIrql", {Old});
  auto Next = take(Model->nextScheduled(false));
  ASSERT_TRUE(Next);
  EXPECT_EQ(Next->PC, CompletionPC);
  const auto Token = Model->scheduledGuestCall(Next->ID);
  success(Model->beginGuestCall(Token));
  Model->enterExecution(profile::CallbackStackBase, Next->ID, Token);
  freeIdle(Idle);
  EXPECT_FALSE(
      take(Model->continueScheduled(Next->ID, StatusMoreProcessingRequired)));
  success(Model->finishScheduled(Next->ID));
  EXPECT_EQ(result(Power).DispatchStatus, StatusPending);
  EXPECT_FALSE(result(Power).Power->BusCompletedAt100ns);
  EXPECT_FALSE(take(Model->nextScheduled(true)));
  EXPECT_EQ(result(Power).Power->BusCompletedAt100ns, 100u);
  EXPECT_FALSE(Model->requestPending());
}
TEST_F(KernelUsbIdleReceipt,
       CompositeReceiptKeepsFirstCauseAcrossNestedCancelAndRearm) {
  initialize({}, true);
  ASSERT_EQ(Children.size(), 2u);
  const uint64_t First = submit(Children[0]);
  const uint64_t FirstInfo = Info;
  const uint64_t Second = submit(Children[1]);
  const uint64_t Power = power(operation(DevicePowerState::D0));
  auto FirstCompletion = idleCompletion(First, StatusSuccess);

  EXPECT_EQ(call("IoCancelIrp", {Second}), 0u);
  auto Cancel = Model->takeGuestCall();
  ASSERT_TRUE(Cancel);
  Model->enterExecution(profile::CallbackStackBase + profile::PageSize,
                        profile::StackBase, Cancel->Token);
  success(Model->beginGuestCall(Cancel->Token));
  const auto *Routine = Exports.lookup(Cancel->PC);
  ASSERT_NE(Routine, nullptr);
  EXPECT_EQ(take(Model->call(*Routine, Cancel->Arguments, nullptr)), 0u);
  auto SecondCompletion = idleCompletion(Second, StatusSuccess);
  freeIdle(Second);
  EXPECT_EQ(take(Model->finishGuestCall(SecondCompletion.Token,
                                        StatusMoreProcessingRequired)),
            0u);
  Model->enterExecution(profile::CallbackStackBase + profile::PageSize,
                        profile::StackBase, Cancel->Token);
  EXPECT_EQ(take(Model->finishGuestCall(Cancel->Token, 0)), 1u);

  Model->enterExecution(profile::CallbackStackBase, profile::StackBase,
                        FirstCompletion.Token);
  const uint64_t Replacement = submit(Children[1]);
  EXPECT_NE(Replacement, Second);
  Info = FirstInfo;
  freeIdle(First);
  EXPECT_EQ(take(Model->finishGuestCall(FirstCompletion.Token,
                                        StatusMoreProcessingRequired)),
            StatusSuccess);
  success(Model->recordDispatchReturn(Power, StatusSuccess));
  EXPECT_EQ(result(Second).UsbIdle->CompletionCause,
            UsbIdleCompletionCause::DeviceD0);
  EXPECT_TRUE(result(Second).Completed);
  EXPECT_FALSE(result(Replacement).Completed);
  EXPECT_FALSE(result(Replacement).UsbIdle->CompletionCause);
  EXPECT_TRUE(result(Power).Completed);
  success(Model->finalizeRequest(Power));
}

TEST_F(KernelUsbIdleReceipt, CancelPreflightPreservesFlagRoutineAndBorrow) {
  initialize();
  const uint64_t Idle = submit();
  const uint64_t Stack = get(Idle + IRPStackPointerOffset);
  const uint64_t Control = get(Stack + StackControlOffset, 1);
  const uint64_t Routine = get(Idle + IRPCancelRoutineOffset);
  put(Stack + StackControlOffset, Control | 2, 1);
  auto Cancel = Model->call("IoCancelIrp", {Idle});
  ASSERT_FALSE(bool(Cancel));
  EXPECT_NE(llvm::toString(Cancel.takeError()).find("control flags"),
            std::string::npos);
  EXPECT_EQ(get(Idle + IRPCancelOffset, 1), 0u);
  EXPECT_EQ(get(Idle + IRPCancelRoutineOffset), Routine);
  EXPECT_FALSE(result(Idle).CancelRequestedAt100ns);
  EXPECT_FALSE(Model->takeGuestCall());
  auto Free = Model->call("ExFreePoolWithTag", {Info, PoolTag});
  ASSERT_FALSE(bool(Free));
  EXPECT_NE(llvm::toString(Free.takeError()).find("borrowed"),
            std::string::npos);

  put(Stack + StackControlOffset, Control, 1);
  const uint64_t Power = power(operation(DevicePowerState::D0));
  auto Complete = idleCompletion(Idle, StatusSuccess);
  freeIdle(Idle);
  EXPECT_EQ(take(Model->finishGuestCall(Complete.Token,
                                        StatusMoreProcessingRequired)),
            StatusSuccess);
  success(Model->recordDispatchReturn(Power, StatusSuccess));
  success(Model->finalizeRequest(Power));
}

TEST_F(KernelUsbIdleReceipt, CompositePreflightFailureDoesNotClaimAnyMember) {
  initialize({}, true);
  ASSERT_EQ(Children.size(), 2u);
  const uint64_t First = submit(Children[0]);
  const uint64_t FirstInfo = Info;
  const uint64_t Second = submit(Children[1]);
  const uint64_t SecondInfo = Info;
  const uint64_t Stack = get(Second + IRPStackPointerOffset);
  const uint64_t Control = get(Stack + StackControlOffset, 1);
  put(Stack + StackControlOffset, Control | 2, 1);
  DriverRequest Input;
  Input.Kind = DriverRequestKind::Power;
  Input.DeviceID = "usb";
  Input.Power = operation(DevicePowerState::D0);
  const uint64_t Power = take(Model->beginRequest(Input)).IRP;
  copyDown(Power);
  auto Rejected = Model->call("PoCallDriver", {PDO, Power});
  ASSERT_FALSE(bool(Rejected));
  EXPECT_NE(llvm::toString(Rejected.takeError()).find("control flags"),
            std::string::npos);
  EXPECT_FALSE(result(Power).Power->BusReceivedAt100ns);
  EXPECT_FALSE(result(First).UsbIdle->CompletionCause);
  EXPECT_FALSE(result(Second).UsbIdle->CompletionCause);
  EXPECT_FALSE(Model->takeGuestCall());

  put(Stack + StackControlOffset, Control, 1);
  EXPECT_EQ(call("PoCallDriver", {PDO, Power}), StatusPending);
  auto FirstCompletion = idleCompletion(First, StatusSuccess);
  Info = FirstInfo;
  freeIdle(First);
  EXPECT_FALSE(take(Model->finishGuestCall(FirstCompletion.Token,
                                           StatusMoreProcessingRequired)));
  auto SecondCompletion = idleCompletion(Second, StatusSuccess);
  Info = SecondInfo;
  freeIdle(Second);
  EXPECT_EQ(take(Model->finishGuestCall(SecondCompletion.Token,
                                        StatusMoreProcessingRequired)),
            StatusSuccess);
  success(Model->recordDispatchReturn(Power, StatusSuccess));
  success(Model->finalizeRequest(Power));
}

TEST_F(KernelUsbIdleReceipt,
       PendingCancelPreventsReceiptWithoutReleasingBorrow) {
  initialize();
  const uint64_t Idle = submit();
  DriverRequest Input;
  Input.Kind = DriverRequestKind::Power;
  Input.DeviceID = "usb";
  Input.Power = operation(DevicePowerState::D0);
  const uint64_t Power = take(Model->beginRequest(Input)).IRP;
  copyDown(Power);
  EXPECT_EQ(call("IoCancelIrp", {Idle}), 0u);
  auto Rejected = Model->call("PoCallDriver", {PDO, Power});
  ASSERT_FALSE(bool(Rejected));
  llvm::consumeError(Rejected.takeError());
  EXPECT_FALSE(result(Power).Power->BusReceivedAt100ns);
  EXPECT_FALSE(result(Idle).UsbIdle->CompletionCause);
  auto Free = Model->call("ExFreePoolWithTag", {Info, PoolTag});
  ASSERT_FALSE(bool(Free));
  EXPECT_NE(llvm::toString(Free.takeError()).find("borrowed"),
            std::string::npos);
  auto Cancel = Model->takeGuestCall();
  ASSERT_TRUE(Cancel);
  const auto *Routine = Exports.lookup(Cancel->PC);
  ASSERT_NE(Routine, nullptr);
  Model->enterExecution(profile::CallbackStackBase + profile::PageSize,
                        profile::StackBase, Cancel->Token);
  success(Model->beginGuestCall(Cancel->Token));
  EXPECT_EQ(take(Model->call(*Routine, Cancel->Arguments, nullptr)), 0u);
  auto Complete = idleCompletion(Idle, StatusCancelled);
  freeIdle(Idle);
  EXPECT_EQ(take(Model->finishGuestCall(Complete.Token,
                                        StatusMoreProcessingRequired)),
            0u);
  Model->enterExecution(profile::CallbackStackBase + profile::PageSize,
                        profile::StackBase, Cancel->Token);
  EXPECT_EQ(take(Model->finishGuestCall(Cancel->Token, 0)), 1u);
  Model->enterExecution(profile::StackBase);
  EXPECT_EQ(call("PoCallDriver", {PDO, Power}), StatusSuccess);
  success(Model->recordDispatchReturn(Power, StatusSuccess));
  success(Model->finalizeRequest(Power));
}

TEST_F(KernelUsbIdleReceipt, QueuedPermissionCannotEnterAfterAnUnrelatedD2) {
  initialize({operation(DevicePowerState::D2)});
  const uint64_t Idle = submit();
  DriverRequest Observe;
  Observe.Kind = DriverRequestKind::Create;
  Observe.DeviceID = "usb";
  Observe.File = 1;
  Observe.PowerPolicyEvents.push_back(
      {0, "usb", DriverPowerPolicyAction::UsbIdlePermission});
  const uint64_t Foreground = take(Model->beginRequest(Observe)).IRP;
  put(Foreground + IRPStatusOffset, StatusSuccess, 4);
  put(Foreground + IRPInformationOffset, 0);
  call("IofCompleteRequest", {Foreground, 0});
  success(Model->recordDispatchReturn(Foreground, StatusSuccess));
  success(Model->finalizeRequest(Foreground));

  const uint64_t Work = call("IoAllocateWorkItem", {FDO});
  call("IoQueueWorkItem", {Work, DispatchPC, profile::DelayedWorkQueue, 0});
  auto Worker = take(Model->nextScheduled(false));
  ASSERT_TRUE(Worker);
  EXPECT_EQ(Worker->Kind, KernelScheduler::CallbackKind::WorkItem);
  EXPECT_FALSE(result(Idle).UsbIdle->CallbackEnteredAt100ns);
  Model->enterExecution(profile::CallbackStackBase, Worker->ID);
  EXPECT_EQ(
      call("PoRequestPowerIrp", {FDO, uint8_t(DevicePowerRequest::Set),
                                 uint32_t(DevicePowerState::D2), 0, 0, 0}),
      StatusPending);
  auto Dispatch = Model->takeGuestCall();
  ASSERT_TRUE(Dispatch);
  const uint64_t Power = Dispatch->Arguments[1];
  Model->enterExecution(profile::CallbackStackBase + profile::PageSize,
                        Worker->ID, Dispatch->Token);
  copyDown(Power);
  EXPECT_EQ(call("PoCallDriver", {PDO, Power}), StatusSuccess);
  EXPECT_EQ(take(Model->finishGuestCall(Dispatch->Token, StatusSuccess)),
            StatusPending);
  EXPECT_EQ(result(Power).Power->DeviceStateAfter, DevicePowerState::D2);
  Model->enterExecution(profile::CallbackStackBase, Worker->ID);
  call("IoFreeWorkItem", {Work});
  success(Model->finishScheduled(Worker->ID));

  auto Rejected = Model->nextScheduled(false);
  ASSERT_FALSE(bool(Rejected));
  EXPECT_NE(llvm::toString(Rejected.takeError()).find("stable S0/D0"),
            std::string::npos);
  EXPECT_FALSE(result(Idle).UsbIdle->CallbackEnteredAt100ns);
  EXPECT_FALSE(result(Idle).UsbIdle->CompletionCause);
  EXPECT_FALSE(result(Idle).UsbIdle->D2IRP);
  auto Free = Model->call("ExFreePoolWithTag", {Info, PoolTag});
  ASSERT_FALSE(bool(Free));
  EXPECT_NE(llvm::toString(Free.takeError()).find("borrowed"),
            std::string::npos);
}
} // namespace
} // namespace neverd::emulation
