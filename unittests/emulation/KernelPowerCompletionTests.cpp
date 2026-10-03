//===- KernelPowerCompletionTests.cpp - Child power IRP continuations ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Verify real child request identity, terminal callbacks and snapshot lifetime
/// independently from instruction execution and the scenario foreground loop.
///
//===----------------------------------------------------------------------===//

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelExportRegistry.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

namespace neverd::emulation {
namespace {
using namespace windows;

class KernelPowerCompletion : public ::testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000;
  static constexpr uint64_t DispatchPC = 0x180001000;
  static constexpr uint64_t CompletionPC = 0x180001100;
  static constexpr uint64_t PowerPC = 0x180001200;
  static constexpr uint64_t WorkerPC = 0x180001300;
  std::unique_ptr<UnicornBackend> Memory;
  std::unique_ptr<KernelModel> Model;
  DriverResult Result;
  KernelExportRegistry Exports;
  uint64_t PDO = 0, FDO = 0;

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
  template <class T>
  void rejected(llvm::Expected<T> Value, llvm::StringRef Message) {
    ASSERT_FALSE(bool(Value));
    EXPECT_NE(llvm::toString(Value.takeError()).find(Message.str()),
              std::string::npos);
  }
  void denied(uint64_t Address, uint32_t Size) {
    auto E = Model->validateGuestAccess(Address, Size, false);
    ASSERT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
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
  DriverPowerOperation
  operation(DevicePowerState State, uint64_t Delay = 0,
            DevicePowerRequest Minor = DevicePowerRequest::Set,
            uint32_t Status = StatusSuccess) {
    DriverPowerOperation Power;
    Power.Minor = Minor;
    Power.State = uint32_t(State);
    Power.BusCompletion.Status = Status;
    Power.BusCompletion.Delay100ns = Delay;
    return Power;
  }
  void copyDown(uint64_t IRP, uint64_t Callback = 0) {
    const uint64_t Stack = get(IRP + IRPStackPointerOffset);
    std::vector<uint8_t> Prefix(StackCompletionOffset);
    success(Model->validateGuestAccess(Stack, Prefix.size(), false));
    success(Memory->read(Stack, Prefix));
    success(Model->validateGuestAccess(Stack - StackSize, Prefix.size(), true));
    success(Memory->write(Stack - StackSize, Prefix));
    put(Stack - StackSize + StackControlOffset,
        Callback
            ? StackInvokeOnSuccess | StackInvokeOnError | StackInvokeOnCancel
            : 0,
        1);
    if (Callback) {
      put(Stack - StackSize + StackCompletionOffset, Callback);
      put(Stack - StackSize + StackCompletionContextOffset, Scratch + 0x200);
    }
  }
  void initialize(std::vector<DriverPowerOperation> Responses,
                  std::optional<DriverWakeCapabilities> Wake = std::nullopt,
                  bool Start = true) {
    Memory = take(UnicornBackend::create(8 * 1024 * 1024));
    ASSERT_TRUE(Memory);
    success(Memory->map(Scratch, 0x10000, Read | Write));
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = DispatchPC;
    Image.Size = 0x3000;
    DriverOptions Options;
    DriverPnpDevice Device;
    Device.ID = "power0";
    Device.InitialDevicePower = DevicePowerState::D0;
    Device.InitialSystemPower = SystemPowerState::Working;
    Device.InitialReportedDevicePower = DevicePowerState::D0;
    Device.RequestedDevicePower = std::move(Responses);
    Device.WakeCapabilities = Wake;
    Options.PnpDevices.push_back(std::move(Device));
    success(Exports.initialize(Options));
    Model = std::make_unique<KernelModel>(*Memory, Result, &Exports);
    success(Model->initialize(Image, Options));
    Model->enterExecution(profile::StackBase);
    const uint64_t Extension =
        get(Model->driverObject() + DriverExtensionOffset);
    put(Extension + DriverAddDeviceOffset, DispatchPC);
    put(Model->driverObject() + DriverDispatchOffset + 0x1b * 8, DispatchPC);
    put(Model->driverObject() + DriverDispatchOffset + 0x16 * 8, DispatchPC);
    success(Model->finishEntry());
    success(Model->preparePnpDevices());
    const auto Add = take(Model->beginAddDevice("power0"));
    PDO = Add.Argument1;
    ASSERT_NE(PDO, 0u);
    EXPECT_EQ(call("IoCreateDevice", {Model->driverObject(), 16, 0,
                                      UnknownDeviceType, 0, 0, Scratch}),
              0u);
    FDO = get(Scratch);
    put(FDO + DeviceFlagsOffset, DeviceBufferedIO | DevicePowerPageable, 4);
    EXPECT_EQ(call("IoAttachDeviceToDeviceStack", {FDO, PDO}), PDO);
    success(Model->finishAddDevice("power0", 0));
    if (!Start)
      return;
    DriverRequest StartRequest;
    StartRequest.Kind = DriverRequestKind::Pnp;
    StartRequest.DeviceID = "power0";
    StartRequest.Pnp = DriverPnpOperation{};
    StartRequest.Pnp->BusCompletion.Status = 0;
    const uint64_t IRP = take(Model->beginRequest(StartRequest)).IRP;
    copyDown(IRP);
    EXPECT_EQ(call("IofCallDriver", {PDO, IRP}), 0u);
    success(Model->recordDispatchReturn(IRP, 0));
    success(Model->finalizeRequest(IRP));
  }
  KernelGuestCall request(DevicePowerState State, uint64_t Callback = PowerPC,
                          uint64_t Device = 0,
                          DevicePowerRequest Minor = DevicePowerRequest::Set) {
    EXPECT_EQ(call("PoRequestPowerIrp",
                   {Device ? Device : FDO, uint8_t(Minor), uint32_t(State),
                    Callback, Scratch + 0x100, 0}),
              StatusPending);
    auto Call = Model->takeGuestCall();
    EXPECT_TRUE(Call);
    if (!Call)
      return {};
    EXPECT_EQ(Call->Token.Owner, GuestCallOwner::WDM);
    EXPECT_EQ(Call->PC, DispatchPC);
    EXPECT_EQ(Call->Arguments.front(), FDO);
    return *Call;
  }
  KernelGuestCall waitWake(SystemPowerState State = SystemPowerState::Working,
                           uint64_t Callback = PowerPC, uint64_t Output = 0,
                           uint64_t Device = 0) {
    EXPECT_EQ(
        call("PoRequestPowerIrp",
             {Device ? Device : FDO, uint8_t(DevicePowerRequest::WaitWake),
              uint32_t(State), Callback, Scratch + 0x100, Output}),
        StatusPending);
    if (Output) {
      EXPECT_NE(get(Output), 0u);
      EXPECT_EQ(get(Output), Result.Requests.back().IRP);
    }
    return pendingCall(DispatchPC);
  }
  uint64_t queuePower(DevicePowerState State, uint64_t Callback = PowerPC,
                      uint64_t Device = 0,
                      DevicePowerRequest Minor = DevicePowerRequest::Set) {
    const size_t Count = Result.Requests.size();
    EXPECT_EQ(call("PoRequestPowerIrp",
                   {Device ? Device : FDO, uint8_t(Minor), uint32_t(State),
                    Callback, Scratch + 0x100, 0}),
              StatusPending);
    EXPECT_FALSE(Model->takeGuestCall());
    EXPECT_EQ(Result.Requests.size(), Count + 1);
    return Result.Requests.size() == Count + 1 ? Result.Requests.back().IRP : 0;
  }
  std::optional<KernelScheduler::Invocation> powerWorker() {
    auto Next = take(Model->nextScheduled(false));
    EXPECT_TRUE(Next);
    if (Next) {
      EXPECT_EQ(Next->Kind, KernelScheduler::CallbackKind::WDMDispatch);
      EXPECT_EQ(Next->PC, DispatchPC);
      EXPECT_EQ(Next->IRQL, scheduler::PassiveLevel);
      EXPECT_EQ(Next->Arguments.size(), 2u);
    }
    return Next;
  }
  void finishWorker(const KernelScheduler::Invocation &Call, uint32_t Status) {
    EXPECT_FALSE(take(Model->continueScheduled(Call.ID, Status)));
    success(Model->finishScheduled(Call.ID));
  }
  void providerQueued(uint64_t Delay, bool Callback) {
    initialize({operation(DevicePowerState::D3, Delay)});
    call("IoDetachDevice", {PDO});
    call("IoDeleteDevice", {FDO});
    const auto Old = call("KfRaiseIrql", {scheduler::DispatchLevel});
    const auto IRP =
        queuePower(DevicePowerState::D3, Callback ? PowerPC : 0, PDO);
    ASSERT_NE(IRP, 0u);
    EXPECT_EQ(call("KeGetCurrentIrql", {}), scheduler::DispatchLevel);
    EXPECT_FALSE(Result.Requests.back().DispatchStatus);
    EXPECT_FALSE(Result.Requests.back().Power->BusReceivedAt100ns);
    call("KeLowerIrql", {Old});
    auto Next = take(Model->nextScheduled(false));
    EXPECT_EQ(Result.Requests.back().DispatchStatus,
              Delay ? StatusPending : StatusSuccess);
    EXPECT_EQ(Result.Requests.back().Power->BusReceivedAt100ns, 0u);
    if (Delay) {
      EXPECT_FALSE(Next);
      EXPECT_FALSE(Result.Requests.back().Completed);
      Next = take(Model->nextScheduled(true));
    }
    if (Callback) {
      ASSERT_TRUE(Next);
      EXPECT_EQ(Next->Kind, KernelScheduler::CallbackKind::WDMCompletion);
      EXPECT_EQ(Next->PC, PowerPC);
      EXPECT_EQ(Next->IRQL, scheduler::PassiveLevel);
      ASSERT_EQ(Next->Arguments.size(), 5u);
      EXPECT_EQ(Next->Arguments[0], PDO);
      EXPECT_EQ(Next->Arguments[3], Scratch + 0x100);
      EXPECT_EQ(get(Next->Arguments[4], 4), StatusSuccess);
      finishWorker(*Next, StatusSuccess);
    } else {
      EXPECT_FALSE(Next);
    }
    EXPECT_TRUE(Result.Requests.back().Completed);
    EXPECT_EQ(Result.Requests.back().Power->DeviceStateAfter,
              DevicePowerState::D3);
    EXPECT_EQ(Result.Requests.back().Power->BusCompletedAt100ns, Delay);
    EXPECT_FALSE(Model->requestPending());
    EXPECT_FALSE(take(Model->nextScheduled(false)));
  }
  KernelGuestCall pendingCall(uint64_t ExpectedPC) {
    auto Call = Model->takeGuestCall();
    EXPECT_TRUE(Call);
    if (!Call)
      return {};
    EXPECT_EQ(Call->PC, ExpectedPC);
    return *Call;
  }
  void finishCall(const KernelGuestCall &Call, uint64_t ReturnValue,
                  uint64_t Expected) {
    auto Finished = take(Model->finishGuestCall(Call.Token, ReturnValue));
    ASSERT_TRUE(Finished);
    EXPECT_EQ(*Finished, Expected);
  }
  KernelScheduler::Invocation scheduled(uint64_t PC) {
    auto Next = take(Model->nextScheduled(true));
    if (!Next)
      Next = take(Model->nextScheduled(false));
    EXPECT_TRUE(Next);
    if (!Next)
      return {};
    EXPECT_EQ(Next->Kind, KernelScheduler::CallbackKind::WDMCompletion);
    EXPECT_EQ(Next->PC, PC);
    EXPECT_EQ(Next->IRQL, scheduler::PassiveLevel);
    return *Next;
  }
};

TEST_F(KernelPowerCompletion, SynchronousChildHasItsOwnResultAndVoidCallback) {
  initialize({operation(DevicePowerState::D3)});
  const auto Dispatch = request(DevicePowerState::D3);
  const uint64_t IRP = Dispatch.Arguments[1];
  ASSERT_EQ(Result.Requests.size(), 2u);
  EXPECT_NE(IRP, Result.Requests.front().IRP);
  EXPECT_EQ(Result.Requests.back().Origin,
            DriverRequestOrigin::PoRequestPowerIrp);
  EXPECT_EQ(Result.Requests.back().ResponseIndex, 0u);
  copyDown(IRP);
  EXPECT_EQ(call("PoCallDriver", {PDO, IRP}), 0u);
  const auto Completion = pendingCall(PowerPC);
  ASSERT_EQ(Completion.Arguments.size(), 5u);
  EXPECT_EQ(Completion.Arguments[0], FDO);
  EXPECT_EQ(Completion.Arguments[1], uint8_t(DevicePowerRequest::Set));
  EXPECT_EQ(Completion.Arguments[2], uint32_t(DevicePowerState::D3));
  EXPECT_EQ(Completion.Arguments[3], Scratch + 0x100);
  const uint64_t Snapshot = Completion.Arguments[4];
  EXPECT_NE(Snapshot, IRP + IRPStatusOffset);
  success(Model->validateGuestAccess(Snapshot, PowerStatusBlockSize, false));
  EXPECT_EQ(get(Snapshot, 4), 0u);
  EXPECT_EQ(get(Snapshot + PowerStatusBlockInformationOffset), 0u);
  denied(IRP + IRPStatusOffset, 4);
  EXPECT_TRUE(Result.Requests.back().Completed);
  EXPECT_FALSE(Result.Requests.back().DispatchStatus);
  finishCall(Completion, StatusMoreProcessingRequired, 0);
  denied(Snapshot, PowerStatusBlockSize);
  finishCall(Dispatch, 0, StatusPending);
  success(Model->finalizeRequest(IRP));
  EXPECT_EQ(Result.Requests.back().DispatchStatus, 0u);
  EXPECT_EQ(Result.Requests.back().IOStatus, 0u);
  EXPECT_EQ(Result.Requests.back().Power->DeviceStateAfter,
            DevicePowerState::D3);
}

TEST_F(KernelPowerCompletion,
       DelayedProviderSchedulesTerminalCallbackWithoutIoCompletion) {
  initialize({operation(DevicePowerState::D3, 17)});
  const auto Dispatch = request(DevicePowerState::D3, PowerPC, PDO);
  const uint64_t IRP = Dispatch.Arguments[1];
  copyDown(IRP);
  EXPECT_EQ(call("IofCallDriver", {PDO, IRP}), StatusPending);
  finishCall(Dispatch, StatusPending, StatusPending);
  EXPECT_FALSE(Result.Requests.back().Completed);
  const auto Callback = scheduled(PowerPC);
  ASSERT_EQ(Callback.Arguments.size(), 5u);
  EXPECT_EQ(Callback.Arguments[0], PDO);
  EXPECT_EQ(Callback.DueTime100ns, 17u);
  EXPECT_EQ(Result.Requests.back().Power->BusReceivedAt100ns, 0u);
  EXPECT_EQ(Result.Requests.back().Power->BusCompletedAt100ns, 17u);
  denied(IRP, 1);
  const uint64_t Snapshot = Callback.Arguments[4];
  success(Model->validateGuestAccess(Snapshot, 16, false));
  EXPECT_FALSE(take(Model->continueScheduled(Callback.ID, UINT64_MAX)));
  success(Model->finishScheduled(Callback.ID));
  denied(Snapshot, 16);
  success(Model->finalizeRequest(IRP));
}

TEST_F(KernelPowerCompletion,
       DelayedChildWithoutCallbackFinalizesAutomatically) {
  initialize({operation(DevicePowerState::D3, 9)});
  const auto Dispatch = request(DevicePowerState::D3, 0);
  const uint64_t IRP = Dispatch.Arguments[1];
  copyDown(IRP);
  EXPECT_EQ(call("IofCallDriver", {PDO, IRP}), StatusPending);
  finishCall(Dispatch, StatusPending, StatusPending);
  EXPECT_FALSE(take(Model->nextScheduled(true)));
  EXPECT_FALSE(Model->requestPending());
  EXPECT_TRUE(Result.Requests.back().Completed);
  EXPECT_EQ(Result.Requests.back().Power->BusCompletedAt100ns, 9u);
  success(Model->finalizeRequest(IRP));
}

TEST_F(KernelPowerCompletion,
       DirectProviderChildHasNoSyntheticDispatchCallback) {
  initialize({operation(DevicePowerState::D3)});
  call("IoDetachDevice", {PDO});
  call("IoDeleteDevice", {FDO});
  EXPECT_EQ(call("PoRequestPowerIrp", {PDO, 2, 4, PowerPC, Scratch, 0}),
            StatusPending);
  const auto Completion = pendingCall(PowerPC);
  ASSERT_EQ(Completion.Arguments.size(), 5u);
  EXPECT_EQ(Completion.Arguments[0], PDO);
  EXPECT_EQ(Result.Requests.back().DispatchStatus, 0u);
  EXPECT_TRUE(Result.Requests.back().Completed);
  const uint64_t IRP = Result.Requests.back().IRP;
  finishCall(Completion, 0, StatusPending);
  success(Model->finalizeRequest(IRP));
}

TEST_F(KernelPowerCompletion,
       ChildCallbackCanCompleteItsIndependentSystemParent) {
  auto ChildResponse = operation(DevicePowerState::D3, 5);
  ChildResponse.Action = DriverPowerAction::Sleep;
  initialize({ChildResponse});
  DriverRequest Input;
  Input.Kind = DriverRequestKind::Power;
  Input.DeviceID = "power0";
  Input.Power = DriverPowerOperation{};
  Input.Power->Type = DriverPowerType::System;
  Input.Power->State = uint32_t(SystemPowerState::Sleeping3);
  Input.Power->Action = DriverPowerAction::Sleep;
  Input.Power->BusCompletion.Status = 0;
  const uint64_t Parent = take(Model->beginRequest(Input)).IRP;
  copyDown(Parent, CompletionPC);
  EXPECT_EQ(call("IofCallDriver", {PDO, Parent}), 0u);
  const auto ParentCompletion = pendingCall(CompletionPC);
  call("IoMarkIrpPending", {Parent});
  EXPECT_EQ(call("PoRequestPowerIrp", {FDO, 2, 4, PowerPC, Parent, 0}),
            StatusPending);
  const auto ChildDispatch = pendingCall(DispatchPC);
  const uint64_t Child = ChildDispatch.Arguments[1];
  ASSERT_NE(Child, Parent);
  copyDown(Child);
  EXPECT_EQ(call("IofCallDriver", {PDO, Child}), StatusPending);
  finishCall(ChildDispatch, StatusPending, StatusPending);
  finishCall(ParentCompletion, StatusMoreProcessingRequired, 0);
  success(Model->recordDispatchReturn(Parent, StatusPending));
  const auto Completion = scheduled(PowerPC);
  EXPECT_EQ(Completion.Arguments[3], Parent);
  call("IofCompleteRequest", {Parent, 0});
  EXPECT_FALSE(Model->takeGuestCall());
  success(Model->finalizeRequest(Parent));
  success(Model->validateGuestAccess(Completion.Arguments[4], 16, false));
  EXPECT_FALSE(take(Model->continueScheduled(Completion.ID, 0)));
  success(Model->finishScheduled(Completion.ID));
  ASSERT_EQ(Result.Requests.size(), 3u);
  EXPECT_EQ(Result.Requests[1].Origin, DriverRequestOrigin::Scenario);
  EXPECT_EQ(Result.Requests[2].Origin, DriverRequestOrigin::PoRequestPowerIrp);
  EXPECT_EQ(Result.Requests[1].Power->SystemStateAfter,
            SystemPowerState::Sleeping3);
  EXPECT_EQ(Result.Requests[2].Power->DeviceStateAfter, DevicePowerState::D3);
  EXPECT_EQ(Result.Requests[1].IOStatus, 0u);
  EXPECT_EQ(Result.Requests[2].IOStatus, 0u);
  EXPECT_FALSE(Model->requestPending());
}

TEST_F(KernelPowerCompletion, TerminalCallbackCanIssueAnotherIndependentChild) {
  initialize(
      {operation(DevicePowerState::D3), operation(DevicePowerState::D0)});
  const auto First = request(DevicePowerState::D3);
  copyDown(First.Arguments[1]);
  EXPECT_EQ(call("IofCallDriver", {PDO, First.Arguments[1]}), 0u);
  const auto Completion = pendingCall(PowerPC);
  const uint64_t Snapshot = Completion.Arguments[4];
  const auto Second = request(DevicePowerState::D0, 0);
  EXPECT_NE(First.Arguments[1], Second.Arguments[1]);
  copyDown(Second.Arguments[1]);
  EXPECT_EQ(call("IofCallDriver", {PDO, Second.Arguments[1]}), 0u);
  finishCall(Second, 0, StatusPending);
  success(Model->validateGuestAccess(Snapshot, 16, false));
  finishCall(Completion, 0, 0);
  finishCall(First, 0, StatusPending);
  ASSERT_EQ(Result.Requests.size(), 3u);
  EXPECT_EQ(Result.Requests[1].ResponseIndex, 0u);
  EXPECT_EQ(Result.Requests[2].ResponseIndex, 1u);
  EXPECT_EQ(Result.Requests[1].Power->DeviceStateAfter, DevicePowerState::D3);
  EXPECT_EQ(Result.Requests[2].Power->DeviceStateBefore, DevicePowerState::D3);
  EXPECT_EQ(Result.Requests[2].Power->DeviceStateAfter, DevicePowerState::D0);
}

TEST_F(KernelPowerCompletion, MPRPreservesChildBeforeTerminalCallback) {
  initialize({operation(DevicePowerState::D3)});
  const auto Dispatch = request(DevicePowerState::D3);
  const uint64_t IRP = Dispatch.Arguments[1];
  copyDown(IRP, CompletionPC);
  EXPECT_EQ(call("IofCallDriver", {PDO, IRP}), 0u);
  const auto IoCompletion = pendingCall(CompletionPC);
  call("IoMarkIrpPending", {IRP});
  finishCall(IoCompletion, StatusMoreProcessingRequired, 0);
  EXPECT_FALSE(Model->takeGuestCall());
  success(Model->validateGuestAccess(IRP + IRPStatusOffset, 4, false));
  EXPECT_FALSE(Result.Requests.back().Completed);
  finishCall(Dispatch, StatusPending, StatusPending);
  call("IofCompleteRequest", {IRP, 0});
  const auto Completion = pendingCall(PowerPC);
  EXPECT_TRUE(Result.Requests.back().Completed);
  finishCall(Completion, 0, 0);
  success(Model->finalizeRequest(IRP));
}

TEST_F(KernelPowerCompletion, SnapshotRemainsLiveAcrossWaitAndWorker) {
  initialize({operation(DevicePowerState::D3, 5)});
  const auto Dispatch = request(DevicePowerState::D3);
  copyDown(Dispatch.Arguments[1]);
  EXPECT_EQ(call("IofCallDriver", {PDO, Dispatch.Arguments[1]}), StatusPending);
  finishCall(Dispatch, StatusPending, StatusPending);
  const auto Completion = scheduled(PowerPC);
  const uint64_t Snapshot = Completion.Arguments[4];
  const uint64_t Event = Scratch + 0x400;
  call("KeInitializeEvent", {Event, 0, 0});
  call("KeWaitForSingleObject", {Event, 0, 0, 0, 0});
  const auto Wait = Model->takeWait();
  ASSERT_TRUE(Wait);
  success(Model->suspendScheduled(Completion.ID));
  const uint64_t Item = call("IoAllocateWorkItem", {FDO});
  call("IoQueueWorkItem", {Item, WorkerPC, profile::DelayedWorkQueue, Event});
  const auto Next = take(Model->nextScheduled(false));
  ASSERT_TRUE(Next);
  EXPECT_EQ(Next->PC, WorkerPC);
  success(Model->validateGuestAccess(Snapshot, 16, false));
  call("KeSetEvent", {Event, 0, 0});
  const auto Ready = take(Model->pollWait(*Wait));
  ASSERT_TRUE(Ready);
  EXPECT_EQ(*Ready, StatusSuccess);
  call("IoFreeWorkItem", {Item});
  success(Model->finishScheduled(Next->ID));
  success(Model->resumeScheduled(Completion.ID));
  EXPECT_FALSE(take(Model->continueScheduled(Completion.ID, 0)));
  success(Model->finishScheduled(Completion.ID));
  denied(Snapshot, 16);
}

TEST_F(KernelPowerCompletion, SnapshotWritesDoNotRewriteCompletedPacketStatus) {
  initialize({operation(DevicePowerState::D3)});
  const auto Dispatch = request(DevicePowerState::D3);
  copyDown(Dispatch.Arguments[1]);
  call("IofCallDriver", {PDO, Dispatch.Arguments[1]});
  const auto Completion = pendingCall(PowerPC);
  put(Completion.Arguments[4], 0xc0000001, 4);
  put(Completion.Arguments[4] + 8, 123);
  EXPECT_EQ(Result.Requests.back().IOStatus, 0u);
  EXPECT_EQ(Result.Requests.back().Information, 0u);
  finishCall(Completion, 0, 0);
  finishCall(Dispatch, 0, StatusPending);
  EXPECT_EQ(Result.Requests.back().IOStatus, 0u);
  EXPECT_EQ(Result.Requests.back().Information, 0u);
}

TEST_F(KernelPowerCompletion,
       FailedLocalQueryStillCompletesCallbackAndPreservesBusNull) {
  initialize({operation(DevicePowerState::D3, 0, DevicePowerRequest::Query)});
  const auto Dispatch =
      request(DevicePowerState::D3, PowerPC, 0, DevicePowerRequest::Query);
  const uint64_t IRP = Dispatch.Arguments[1];
  put(IRP + IRPStatusOffset, 0xc0000001, 4);
  put(IRP + IRPInformationOffset, 0);
  call("IofCompleteRequest", {IRP, 0});
  const auto Completion = pendingCall(PowerPC);
  EXPECT_EQ(get(Completion.Arguments[4], 4), 0xc0000001u);
  EXPECT_FALSE(Result.Requests.back().Power->BusReceivedAt100ns);
  EXPECT_FALSE(Result.Requests.back().Power->BusCompletedAt100ns);
  finishCall(Completion, 0, 0);
  finishCall(Dispatch, 0xc0000001, StatusPending);
  EXPECT_EQ(Result.Requests.back().Power->DeviceStateAfter,
            DevicePowerState::D0);
}

TEST_F(KernelPowerCompletion,
       RejectedRequestsDoNotConsumeResponseOrCreateObservation) {
  initialize({operation(DevicePowerState::D3)});
  const size_t Count = Result.Requests.size();
  rejected(Model->call("PoRequestPowerIrp", {FDO, 2, 1, PowerPC, 0, 0}),
           "does not match");
  rejected(Model->call("PoRequestPowerIrp", {FDO, 2, 4, PowerPC, 0, Scratch}),
           "null output");
  EXPECT_EQ(call("PoRequestPowerIrp", {FDO, 255, 4, PowerPC, 0, 0}),
            StatusInvalidParameter2);
  EXPECT_EQ(Result.Requests.size(), Count);
  EXPECT_FALSE(Model->takeGuestCall());
  const auto Dispatch = request(DevicePowerState::D3, 0);
  copyDown(Dispatch.Arguments[1]);
  call("IofCallDriver", {PDO, Dispatch.Arguments[1]});
  finishCall(Dispatch, 0, StatusPending);
  EXPECT_EQ(Result.Requests.back().ResponseIndex, 0u);
  rejected(Model->call("PoRequestPowerIrp", {FDO, 2, 4, 0, 0, 0}), "exhausted");
}

TEST_F(KernelPowerCompletion,
       DPCRequestReservesOnePacketBeforePassiveDispatch) {
  initialize(
      {operation(DevicePowerState::D3), operation(DevicePowerState::D3)});
  const uint64_t Dpc = Scratch + 0x600;
  call("KeInitializeDpc", {Dpc, WorkerPC, 0});
  EXPECT_EQ(call("KeInsertQueueDpc", {Dpc, 0, 0}), 1u);
  const auto DPC = take(Model->nextScheduled(false));
  ASSERT_TRUE(DPC);
  ASSERT_EQ(DPC->IRQL, scheduler::DispatchLevel);
  const uint64_t IRP = queuePower(DevicePowerState::D3);
  ASSERT_NE(IRP, 0u);
  EXPECT_EQ(call("KeGetCurrentIrql", {}), scheduler::DispatchLevel);
  EXPECT_FALSE(Result.Requests.back().DispatchStatus);
  EXPECT_FALSE(Result.Requests.back().Power->BusReceivedAt100ns);
  EXPECT_EQ(Result.Requests.back().Power->DeviceStateBefore,
            DevicePowerState::D0);
  EXPECT_EQ(Result.Requests.back().Power->DeviceStateAfter,
            DevicePowerState::D0);
  EXPECT_EQ(Result.Requests.back().ResponseIndex, 0u);
  rejected(Model->call("PoRequestPowerIrp", {FDO, 2, 4, 0, 0, 0}),
           "already active");
  EXPECT_EQ(Result.Requests.size(), 2u);
  rejected(Model->nextScheduled(false), "unfinished callback");
  success(Model->finishScheduled(DPC->ID));
  const auto Dispatch = powerWorker();
  ASSERT_TRUE(Dispatch);
  ASSERT_EQ(Dispatch->Arguments.size(), 2u);
  EXPECT_EQ(Dispatch->Arguments[0], FDO);
  EXPECT_EQ(Dispatch->Arguments[1], IRP);
  EXPECT_EQ(call("KeGetCurrentIrql", {}), scheduler::PassiveLevel);
  copyDown(IRP);
  EXPECT_EQ(call("PoCallDriver", {PDO, IRP}), StatusSuccess);
  const auto Completion = pendingCall(PowerPC);
  ASSERT_EQ(Completion.Arguments.size(), 5u);
  EXPECT_EQ(Completion.Arguments[0], FDO);
  EXPECT_EQ(Completion.Arguments[3], Scratch + 0x100);
  finishCall(Completion, 0, StatusSuccess);
  finishWorker(*Dispatch, StatusSuccess);
  EXPECT_EQ(Result.Requests.back().DispatchStatus, StatusSuccess);
  EXPECT_EQ(Result.Requests.back().Power->DeviceStateAfter,
            DevicePowerState::D3);
  EXPECT_FALSE(Model->requestPending());
}

TEST_F(KernelPowerCompletion,
       FullCapacityPreservesDelayedTerminalCallbackAndConsumedResponse) {
  initialize(
      {operation(DevicePowerState::D3, 11), operation(DevicePowerState::D0)});
  const auto Dispatch = request(DevicePowerState::D3);
  const uint64_t IRP = Dispatch.Arguments[1];
  copyDown(IRP);
  EXPECT_EQ(call("IofCallDriver", {PDO, IRP}), StatusPending);
  finishCall(Dispatch, StatusPending, StatusPending);
  uint64_t LastWorker = 0, LastItem = 0;
  for (unsigned I = 0; I < scheduler::DefaultMaxPendingCallbacks; ++I) {
    LastItem = call("IoAllocateWorkItem", {FDO});
    call("IoQueueWorkItem", {LastItem, WorkerPC, profile::DelayedWorkQueue, 0});
    const auto Worker = take(Model->nextScheduled(false));
    ASSERT_TRUE(Worker);
    LastWorker = Worker->ID;
    success(Model->suspendScheduled(LastWorker));
  }
  std::vector<uint8_t> Before(IRPSize + 2 * StackSize);
  success(Memory->read(IRP, Before));
  rejected(Model->nextScheduled(true), "pending callback limit");
  std::vector<uint8_t> After(Before.size());
  success(Memory->read(IRP, After));
  EXPECT_EQ(After, Before);
  ASSERT_EQ(Result.Requests.size(), 2u);
  EXPECT_EQ(Result.Requests.back().ResponseIndex, 0u);
  EXPECT_FALSE(Result.Requests.back().Completed);
  EXPECT_FALSE(Result.Requests.back().Power->BusStatus);
  EXPECT_FALSE(Result.Requests.back().Power->BusCompletedAt100ns);
  EXPECT_EQ(Model->nextEventTime(), 11u);
  EXPECT_FALSE(Model->takeGuestCall());
  success(Model->resumeScheduled(LastWorker));
  call("IoFreeWorkItem", {LastItem});
  success(Model->finishScheduled(LastWorker));
  const auto Completion = scheduled(PowerPC);
  EXPECT_EQ(Result.Requests.back().Power->BusCompletedAt100ns, 11u);
  EXPECT_FALSE(take(Model->continueScheduled(Completion.ID, 0)));
  success(Model->finishScheduled(Completion.ID));
  // Retrying the due batch must neither re-consume response zero nor consume
  // the next response before its actual PoRequestPowerIrp call.
  const auto Next = request(DevicePowerState::D0, 0);
  copyDown(Next.Arguments[1]);
  call("IofCallDriver", {PDO, Next.Arguments[1]});
  finishCall(Next, 0, StatusPending);
  ASSERT_EQ(Result.Requests.size(), 3u);
  EXPECT_EQ(Result.Requests.back().ResponseIndex, 1u);
  EXPECT_EQ(Result.Requests.back().Power->DeviceStateAfter,
            DevicePowerState::D0);
}

TEST_F(KernelPowerCompletion, WaitWakePublishesExactPacketBeforeGuestDispatch) {
  initialize({operation(DevicePowerState::D3)},
             DriverWakeCapabilities{true, true});
  const uint64_t Output = Scratch + 0x800;
  const auto Dispatch =
      waitWake(SystemPowerState::Sleeping3, PowerPC, Output, PDO);
  const uint64_t IRP = Dispatch.Arguments[1];
  EXPECT_EQ(get(Output), IRP);
  const uint64_t Stack = get(IRP + IRPStackPointerOffset);
  EXPECT_EQ(get(Stack + StackMinorOffset, 1),
            uint8_t(DevicePowerRequest::WaitWake));
  EXPECT_EQ(get(Stack + StackPowerSystemContextOffset, 4),
            uint32_t(SystemPowerState::Sleeping3));
  EXPECT_EQ(get(Stack + StackPowerTypeOffset, 4), 0u);
  EXPECT_EQ(get(Stack + StackPowerStateOffset, 4), 0u);
  EXPECT_EQ(get(Stack + StackPowerActionOffset, 4), 0u);
  EXPECT_EQ(Result.Requests.back().Origin,
            DriverRequestOrigin::PoRequestPowerIrp);
  EXPECT_FALSE(Result.Requests.back().ResponseIndex);
  EXPECT_EQ(Result.Requests.back().Power->Type, DriverPowerType::System);
  // The upper driver can fail before forwarding. Its callback sees the original
  // requester, while the output slot belongs to the caller after publication.
  put(Output, 0);
  put(IRP + IRPStatusOffset, StatusUnsuccessful, 4);
  put(IRP + IRPInformationOffset, 0);
  call("IofCompleteRequest", {IRP, 0});
  const auto Completion = pendingCall(PowerPC);
  ASSERT_EQ(Completion.Arguments.size(), 5u);
  EXPECT_EQ(Completion.Arguments[0], PDO);
  EXPECT_EQ(Completion.Arguments[1], uint8_t(DevicePowerRequest::WaitWake));
  EXPECT_EQ(Completion.Arguments[2], uint32_t(SystemPowerState::Sleeping3));
  EXPECT_EQ(Completion.Arguments[3], Scratch + 0x100);
  EXPECT_EQ(get(Completion.Arguments[4], 4), StatusUnsuccessful);
  EXPECT_FALSE(Result.Requests.back().Power->BusReceivedAt100ns);
  finishCall(Completion, StatusMoreProcessingRequired, 0);
  finishCall(Dispatch, StatusUnsuccessful, StatusPending);
  EXPECT_EQ(get(Output), 0u);
  const auto Power = request(DevicePowerState::D3, 0);
  copyDown(Power.Arguments[1]);
  call("PoCallDriver", {PDO, Power.Arguments[1]});
  finishCall(Power, StatusSuccess, StatusPending);
  EXPECT_EQ(Result.Requests.back().ResponseIndex, 0u);
}

TEST_F(KernelPowerCompletion,
       WaitWakeCallbackAndOutputAreIndependentlyOptional) {
  initialize({});
  for (const uint64_t Callback : {uint64_t(0), PowerPC}) {
    for (const uint64_t Output : {uint64_t(0), Scratch + 0x800}) {
      const auto Dispatch =
          waitWake(SystemPowerState::Working, Callback, Output);
      const uint64_t IRP = Dispatch.Arguments[1];
      copyDown(IRP);
      EXPECT_EQ(call("PoCallDriver", {PDO, IRP}), StatusNotSupported);
      if (Callback) {
        const auto Completion = pendingCall(PowerPC);
        EXPECT_EQ(get(Completion.Arguments[4], 4), StatusNotSupported);
        finishCall(Completion, 0, StatusNotSupported);
      } else {
        EXPECT_FALSE(Model->takeGuestCall());
      }
      finishCall(Dispatch, StatusNotSupported, StatusPending);
      EXPECT_TRUE(Result.Requests.back().Completed);
      EXPECT_EQ(Result.Requests.back().Power->BusStatus, StatusNotSupported);
      EXPECT_FALSE(Result.Requests.back().ResponseIndex);
      EXPECT_FALSE(Model->requestPending());
    }
  }
}

TEST_F(KernelPowerCompletion,
       WaitWakeCapabilityFailuresAreSentProviderResults) {
  initialize({}, DriverWakeCapabilities{false, false});
  const auto Dispatch = waitWake(SystemPowerState::Sleeping3, 0);
  copyDown(Dispatch.Arguments[1]);
  EXPECT_EQ(call("PoCallDriver", {PDO, Dispatch.Arguments[1]}),
            StatusNotSupported);
  finishCall(Dispatch, StatusNotSupported, StatusPending);
  const auto &Power = *Result.Requests.back().Power;
  EXPECT_EQ(Power.BusReceivedAt100ns, 0u);
  EXPECT_EQ(Power.BusCompletedAt100ns, 0u);
  EXPECT_EQ(Power.BusStatus, StatusNotSupported);
  EXPECT_EQ(Power.DeviceStateAfter, DevicePowerState::D0);
  EXPECT_EQ(Power.SystemStateAfter, SystemPowerState::Working);
}

TEST_F(KernelPowerCompletion,
       WaitWakeSystemLimitCannotExceedProviderCapability) {
  initialize({}, DriverWakeCapabilities{true, false});
  const auto Dispatch = waitWake(SystemPowerState::Sleeping3, 0);
  copyDown(Dispatch.Arguments[1]);
  EXPECT_EQ(call("PoCallDriver", {PDO, Dispatch.Arguments[1]}),
            StatusInvalidDeviceState);
  finishCall(Dispatch, StatusInvalidDeviceState, StatusPending);
  EXPECT_EQ(Result.Requests.back().Power->BusStatus, StatusInvalidDeviceState);
  EXPECT_FALSE(Result.Requests.back().ResponseIndex);
}

TEST_F(KernelPowerCompletion, DuplicateWaitWakeDoesNotReplaceRetainedPacket) {
  initialize({}, DriverWakeCapabilities{true, true});
  const auto First = waitWake(SystemPowerState::Working, 0);
  const uint64_t IRP = First.Arguments[1];
  copyDown(IRP);
  EXPECT_EQ(call("PoCallDriver", {PDO, IRP}), StatusPending);
  finishCall(First, StatusPending, StatusPending);
  const auto Second = waitWake(SystemPowerState::Sleeping3, 0);
  copyDown(Second.Arguments[1]);
  EXPECT_EQ(call("PoCallDriver", {PDO, Second.Arguments[1]}), StatusDeviceBusy);
  finishCall(Second, StatusDeviceBusy, StatusPending);
  EXPECT_FALSE(Result.Requests[1].Completed);
  EXPECT_FALSE(Result.Requests[1].Power->BusCompletedAt100ns);
  EXPECT_TRUE(Result.Requests.back().Completed);
  EXPECT_EQ(Result.Requests.back().Power->BusStatus, StatusDeviceBusy);
  EXPECT_TRUE(Model->requestPending(IRP));
  EXPECT_FALSE(Model->requestPending(
      IRP, KernelModel::PendingRequestScope::ExcludePowerParked));
  EXPECT_NE(get(IRP + IRPCancelRoutineOffset), 0u);
}

TEST_F(KernelPowerCompletion,
       WaitWakeRejectsInvalidOutputAndStateWithoutAllocation) {
  initialize({}, DriverWakeCapabilities{true, true});
  const uint64_t Output = Scratch + 0x800;
  const uint64_t Before = call("IoAllocateIrp", {1, 0});
  ASSERT_NE(Before, 0u);
  call("IoFreeIrp", {Before});
  put(Output, UINT64_MAX);
  success(Memory->protect(Scratch, profile::PageSize, Read));
  rejected(Model->call("PoRequestPowerIrp",
                       {FDO, uint8_t(DevicePowerRequest::WaitWake),
                        uint32_t(SystemPowerState::Working), 0, 0, Output}),
           "writable");
  success(Memory->protect(Scratch, profile::PageSize, Read | Write));
  rejected(Model->call("PoRequestPowerIrp",
                       {FDO, uint8_t(DevicePowerRequest::WaitWake),
                        uint32_t(SystemPowerState::Working), 0, 0,
                        Scratch + 0x10000 - 4}),
           "writable");
  rejected(Model->call("PoRequestPowerIrp",
                       {FDO, uint8_t(DevicePowerRequest::WaitWake),
                        uint32_t(SystemPowerState::Sleeping1), 0, 0, Output}),
           "unsupported");
  EXPECT_EQ(get(Output), UINT64_MAX);
  EXPECT_EQ(Result.Requests.size(), 1u);
  EXPECT_FALSE(Model->takeGuestCall());
  const uint64_t After = call("IoAllocateIrp", {1, 0});
  EXPECT_EQ(After, (Before + IRPSize + StackSize + PoolAlignment - 1) &
                       ~(PoolAlignment - 1));
  call("IoFreeIrp", {After});
}

TEST_F(KernelPowerCompletion, WaitWakeRequiresActualLowerStartAcknowledgement) {
  initialize({}, DriverWakeCapabilities{true, true}, false);
  const auto Issue = [&] {
    return Model->call("PoRequestPowerIrp",
                       {FDO, uint8_t(DevicePowerRequest::WaitWake),
                        uint32_t(SystemPowerState::Working), 0, 0,
                        Scratch + 0x800});
  };
  rejected(Issue(), "completed START epoch");
  EXPECT_TRUE(Result.Requests.empty());
  DriverRequest Start;
  Start.Kind = DriverRequestKind::Pnp;
  Start.DeviceID = "power0";
  Start.Pnp = DriverPnpOperation{};
  Start.Pnp->BusCompletion.Status = StatusSuccess;
  const uint64_t IRP = take(Model->beginRequest(Start)).IRP;
  rejected(Issue(), "lower START completion");
  EXPECT_EQ(Result.Requests.size(), 1u);
  EXPECT_FALSE(Model->takeGuestCall());
  copyDown(IRP, CompletionPC);
  EXPECT_EQ(call("IofCallDriver", {PDO, IRP}), StatusSuccess);
  const auto Completion = pendingCall(CompletionPC);
  ASSERT_NE(Completion.Token.ID, 0u);
  call("IoMarkIrpPending", {IRP});
  finishCall(Completion, StatusMoreProcessingRequired, StatusSuccess);
  EXPECT_EQ(take(Issue()), StatusPending);
  const auto Dispatch = pendingCall(DispatchPC);
  ASSERT_EQ(Dispatch.Arguments.size(), 2u);
  copyDown(Dispatch.Arguments[1]);
  EXPECT_EQ(call("PoCallDriver", {PDO, Dispatch.Arguments[1]}), StatusPending);
  finishCall(Dispatch, StatusPending, StatusPending);
  call("IofCompleteRequest", {IRP, 0});
  success(Model->recordDispatchReturn(IRP, StatusPending));
  success(Model->finalizeRequest(IRP));
}

TEST_F(KernelPowerCompletion, WaitWakeRequiresStableD0BeforePacketPublication) {
  initialize({operation(DevicePowerState::D2, 7, DevicePowerRequest::Query),
              operation(DevicePowerState::D2)},
             DriverWakeCapabilities{true, true});
  const auto Query =
      request(DevicePowerState::D2, 0, 0, DevicePowerRequest::Query);
  copyDown(Query.Arguments[1]);
  EXPECT_EQ(call("PoCallDriver", {PDO, Query.Arguments[1]}), StatusPending);
  finishCall(Query, StatusPending, StatusPending);
  const auto Issue = [&] {
    return Model->call("PoRequestPowerIrp",
                       {FDO, uint8_t(DevicePowerRequest::WaitWake),
                        uint32_t(SystemPowerState::Working), 0, 0,
                        Scratch + 0x800});
  };
  put(Scratch + 0x800, UINT64_MAX);
  rejected(Issue(), "stable D0");
  EXPECT_EQ(Result.Requests.size(), 2u);
  EXPECT_FALSE(take(Model->nextScheduled(true)));
  const auto Set = request(DevicePowerState::D2, 0);
  copyDown(Set.Arguments[1]);
  EXPECT_EQ(call("PoCallDriver", {PDO, Set.Arguments[1]}), StatusSuccess);
  finishCall(Set, StatusSuccess, StatusPending);
  rejected(Issue(), "stable D0");
  EXPECT_EQ(Result.Requests.size(), 3u);
  EXPECT_EQ(get(Scratch + 0x800), UINT64_MAX);
  EXPECT_FALSE(Model->takeGuestCall());
}

TEST_F(KernelPowerCompletion,
       WaitWakeSnapshotBudgetFailureLeavesPacketSpaceUntouched) {
  initialize({}, DriverWakeCapabilities{true, true});
  const uint64_t Probe = call("IoAllocateIrp", {1, 0});
  ASSERT_NE(Probe, 0u);
  call("IoFreeIrp", {Probe});
  const uint64_t NextPage =
      (Probe + IRPSize + StackSize + profile::PageSize - 1) &
      ~(profile::PageSize - 1);
  const uint64_t PacketSize = IRPSize + 2 * StackSize;
  const uint64_t FillSize = profile::KernelArenaBase +
                            profile::KernelArenaSize - NextPage - PacketSize;
  ASSERT_GT(FillSize, profile::PageSize);
  const uint64_t Fill = call("ExAllocatePoolWithTag", {PoolNX, FillSize, 1});
  ASSERT_EQ(Fill, NextPage);
  const uint64_t Output = Scratch + 0x800;
  put(Output, UINT64_MAX);
  EXPECT_EQ(call("PoRequestPowerIrp",
                 {FDO, uint8_t(DevicePowerRequest::WaitWake),
                  uint32_t(SystemPowerState::Working), PowerPC, 0, Output}),
            StatusInsufficientResources);
  EXPECT_EQ(get(Output), UINT64_MAX);
  EXPECT_EQ(Result.Requests.size(), 1u);
  EXPECT_FALSE(Model->takeGuestCall());
  const auto Dispatch = waitWake(SystemPowerState::Working, 0, Output);
  EXPECT_EQ(Dispatch.Arguments[1], NextPage + FillSize);
  put(Dispatch.Arguments[1] + IRPStatusOffset, StatusUnsuccessful, 4);
  put(Dispatch.Arguments[1] + IRPInformationOffset, 0);
  call("IofCompleteRequest", {Dispatch.Arguments[1], 0});
  finishCall(Dispatch, StatusUnsuccessful, StatusPending);
  call("ExFreePoolWithTag", {Fill, 1});
}

TEST_F(KernelPowerCompletion,
       WaitWakeCannotSucceedWithoutRealProviderCompletion) {
  initialize({}, DriverWakeCapabilities{true, true});
  const auto Dispatch = waitWake();
  const uint64_t IRP = Dispatch.Arguments[1];
  const uint64_t Stack = get(IRP + IRPStackPointerOffset);
  put(Stack + StackControlOffset, StackInvokeOnSuccess | StackInvokeOnError, 1);
  put(Stack + StackCompletionOffset, CompletionPC);
  put(Stack + StackCompletionContextOffset, Scratch + 0x200);
  put(IRP + IRPStatusOffset, StatusSuccess, 4);
  put(IRP + IRPInformationOffset, 0);
  rejected(Model->call("IofCompleteRequest", {IRP, 0}), "provider completion");
  EXPECT_FALSE(Model->takeGuestCall());
  EXPECT_EQ(get(IRP + IRPStackPointerOffset), Stack);
  EXPECT_EQ(get(Stack + StackCompletionOffset), CompletionPC);
  EXPECT_FALSE(Result.Requests.back().Completed);
  put(IRP + IRPStatusOffset, StatusUnsuccessful, 4);
  call("IofCompleteRequest", {IRP, 0});
  const auto IoCompletion = pendingCall(CompletionPC);
  EXPECT_FALSE(take(Model->finishGuestCall(IoCompletion.Token, StatusSuccess)));
  const auto Completion = pendingCall(PowerPC);
  ASSERT_EQ(Completion.Arguments.size(), 5u);
  EXPECT_EQ(get(Completion.Arguments[4], 4), StatusUnsuccessful);
  finishCall(Completion, 0, 0);
  finishCall(Dispatch, StatusUnsuccessful, StatusPending);
}

TEST_F(KernelPowerCompletion,
       CancelledWaitWakeMPRAndTerminalCallbackMustDrain) {
  initialize({}, DriverWakeCapabilities{true, true});
  const auto Dispatch = waitWake();
  const uint64_t IRP = Dispatch.Arguments[1];
  copyDown(IRP, CompletionPC);
  EXPECT_EQ(call("PoCallDriver", {PDO, IRP}), StatusPending);
  finishCall(Dispatch, StatusPending, StatusPending);
  EXPECT_FALSE(Model->requestPending(
      IRP, KernelModel::PendingRequestScope::ExcludePowerParked));
  EXPECT_EQ(call("IoCancelIrp", {IRP}), 0u);
  const auto Cancel = Model->takeGuestCall();
  ASSERT_TRUE(Cancel);
  const auto *Routine = Exports.lookup(Cancel->PC);
  ASSERT_TRUE(Routine);
  success(Model->beginGuestCall(Cancel->Token));
  Model->enterExecution(2, 2, Cancel->Token);
  EXPECT_EQ(take(Model->call(*Routine, Cancel->Arguments, nullptr)), 0u);
  const auto IoCompletion = pendingCall(CompletionPC);
  call("IoMarkIrpPending", {IRP});
  finishCall(IoCompletion, StatusMoreProcessingRequired, 0);
  finishCall(*Cancel, 0, 1);
  Model->enterExecution(1);
  EXPECT_TRUE(Model->requestPending(
      IRP, KernelModel::PendingRequestScope::ExcludePowerParked));
  EXPECT_FALSE(Result.Requests.back().Completed);
  EXPECT_EQ(Result.Requests.back().Power->BusStatus,
            framework::RequestCancelled);
  call("IofCompleteRequest", {IRP, 0});
  const auto Completion = pendingCall(PowerPC);
  EXPECT_TRUE(Result.Requests.back().Completed);
  EXPECT_TRUE(Model->requestPending(
      IRP, KernelModel::PendingRequestScope::ExcludePowerParked));
  EXPECT_EQ(get(Completion.Arguments[4], 4), framework::RequestCancelled);
  finishCall(Completion, 0, 0);
  EXPECT_FALSE(Model->requestPending());
}

TEST_F(KernelPowerCompletion, RetainedWaitWakeIsNotASystemPowerTransaction) {
  auto ChildResponse = operation(DevicePowerState::D3);
  ChildResponse.Action = DriverPowerAction::Sleep;
  initialize({ChildResponse}, DriverWakeCapabilities{true, true});
  const auto Wake = waitWake(SystemPowerState::Sleeping3, 0);
  copyDown(Wake.Arguments[1]);
  EXPECT_EQ(call("PoCallDriver", {PDO, Wake.Arguments[1]}), StatusPending);
  finishCall(Wake, StatusPending, StatusPending);
  DriverRequest Input;
  Input.Kind = DriverRequestKind::Power;
  Input.DeviceID = "power0";
  Input.Power = DriverPowerOperation{};
  Input.Power->Type = DriverPowerType::System;
  Input.Power->State = uint32_t(SystemPowerState::Sleeping3);
  Input.Power->Action = DriverPowerAction::Sleep;
  Input.Power->BusCompletion.Status = StatusSuccess;
  const uint64_t Parent = take(Model->beginRequest(Input)).IRP;
  copyDown(Parent, CompletionPC);
  EXPECT_EQ(call("PoCallDriver", {PDO, Parent}), StatusSuccess);
  const auto ParentCompletion = pendingCall(CompletionPC);
  call("IoMarkIrpPending", {Parent});
  finishCall(ParentCompletion, StatusMoreProcessingRequired, StatusSuccess);
  success(Model->recordDispatchReturn(Parent, StatusPending));
  const auto Child = request(DevicePowerState::D3, 0);
  copyDown(Child.Arguments[1]);
  EXPECT_EQ(call("PoCallDriver", {PDO, Child.Arguments[1]}), StatusSuccess);
  finishCall(Child, StatusSuccess, StatusPending);
  call("IofCompleteRequest", {Parent, 0});
  success(Model->finalizeRequest(Parent));
  EXPECT_FALSE(Result.Requests[1].Completed);
  EXPECT_EQ(Result.Requests.back().ResponseIndex, 0u);
  EXPECT_EQ(Result.Requests.back().Power->DeviceStateAfter,
            DevicePowerState::D3);
}

TEST_F(KernelPowerCompletion, APCQueryUsesPassiveDispatchAndPreservesFailure) {
  initialize({operation(DevicePowerState::D3, 0, DevicePowerRequest::Query,
                        StatusUnsuccessful)});
  const auto Old = call("KfRaiseIrql", {APCLevel});
  const auto IRP =
      queuePower(DevicePowerState::D3, 0, 0, DevicePowerRequest::Query);
  ASSERT_NE(IRP, 0u);
  EXPECT_EQ(call("KeGetCurrentIrql", {}), APCLevel);
  EXPECT_FALSE(Result.Requests.back().Power->BusStatus);
  call("KeLowerIrql", {Old});
  const auto Dispatch = powerWorker();
  ASSERT_TRUE(Dispatch);
  copyDown(IRP);
  EXPECT_EQ(call("PoCallDriver", {PDO, IRP}), StatusUnsuccessful);
  EXPECT_FALSE(Model->takeGuestCall());
  finishWorker(*Dispatch, StatusUnsuccessful);
  EXPECT_EQ(Result.Requests.back().IOStatus, StatusUnsuccessful);
  EXPECT_EQ(Result.Requests.back().DispatchStatus, StatusUnsuccessful);
  EXPECT_EQ(Result.Requests.back().Power->DeviceStateAfter,
            DevicePowerState::D0);
}

TEST_F(KernelPowerCompletion, ElevatedWaitWakeStillRejectsBeforePublication) {
  initialize({}, DriverWakeCapabilities{true, true});
  for (uint8_t IRQL : {APCLevel, scheduler::DispatchLevel}) {
    const auto Old = call("KfRaiseIrql", {IRQL});
    rejected(Model->call("PoRequestPowerIrp",
                         {FDO, uint8_t(DevicePowerRequest::WaitWake),
                          uint32_t(SystemPowerState::Working), PowerPC, 0,
                          Scratch + 0x800}),
             "PASSIVE_LEVEL");
    EXPECT_EQ(call("KeGetCurrentIrql", {}), IRQL);
    EXPECT_EQ(Result.Requests.size(), 1u);
    EXPECT_FALSE(Model->takeGuestCall());
    call("KeLowerIrql", {Old});
  }
  EXPECT_FALSE(take(Model->nextScheduled(false)));
}

TEST_F(KernelPowerCompletion,
       ProviderOnlyWorkerReusesItsSlotForInlineCallback) {
  providerQueued(0, true);
}

TEST_F(KernelPowerCompletion,
       ProviderOnlyWorkerWithoutCallbackRetiresInternally) {
  providerQueued(0, false);
}

TEST_F(KernelPowerCompletion, ProviderOnlyWorkerRetainsDelayedCallback) {
  providerQueued(11, true);
}

TEST_F(KernelPowerCompletion,
       ProviderOnlyDelayedWorkerWithoutCallbackCompletes) {
  providerQueued(11, false);
}

TEST_F(KernelPowerCompletion, FullDPCQueueFailurePreservesPacketBudgetAndFIFO) {
  initialize({operation(DevicePowerState::D3)});
  call("KeInitializeDpc", {Scratch + 0x600, WorkerPC, 0});
  call("KeInsertQueueDpc", {Scratch + 0x600, 0, 0});
  const auto DPC = take(Model->nextScheduled(false));
  ASSERT_TRUE(DPC);
  for (unsigned I = 1; I < scheduler::DefaultMaxPendingCallbacks; ++I) {
    const auto Item = call("IoAllocateWorkItem", {FDO});
    call("IoQueueWorkItem", {Item, WorkerPC, profile::DelayedWorkQueue, 0});
  }
  const uint64_t Before = call("IoAllocateIrp", {1, 0});
  ASSERT_NE(Before, 0u);
  call("IoFreeIrp", {Before});
  rejected(Model->call("PoRequestPowerIrp", {FDO, 2, 4, PowerPC, 0, 0}),
           "pending callback limit");
  EXPECT_EQ(Result.Requests.size(), 1u);
  EXPECT_EQ(call("KeGetCurrentIrql", {}), scheduler::DispatchLevel);
  EXPECT_FALSE(Model->takeGuestCall());
  const uint64_t After = call("IoAllocateIrp", {1, 0});
  ASSERT_NE(After, 0u);
  EXPECT_EQ(After, (Before + IRPSize + StackSize + PoolAlignment - 1) &
                       ~(PoolAlignment - 1));
  call("IoFreeIrp", {After});
  success(Model->finishScheduled(DPC->ID));
  const auto Old = call("KfRaiseIrql", {scheduler::DispatchLevel});
  ASSERT_NE(queuePower(DevicePowerState::D3), 0u);
  call("KeLowerIrql", {Old});
  EXPECT_EQ(Result.Requests.size(), 2u);
  EXPECT_EQ(Result.Requests.back().ResponseIndex, 0u);
  EXPECT_FALSE(Result.Requests.back().DispatchStatus);
}

TEST_F(KernelPowerCompletion,
       QueuedPowerOwnsCapturedRouteAfterDetachAndDelete) {
  initialize({operation(DevicePowerState::D3)});
  const auto Old = call("KfRaiseIrql", {scheduler::DispatchLevel});
  const uint64_t IRP = queuePower(DevicePowerState::D3, 0);
  ASSERT_NE(IRP, 0u);
  call("KeLowerIrql", {Old});
  call("IoDetachDevice", {PDO});
  call("IoDeleteDevice", {FDO});
  success(Model->validateGuestAccess(FDO + DeviceFlagsOffset, 4, false));
  rejected(Model->beginUnload(), "provider devices");
  const auto Dispatch = powerWorker();
  ASSERT_TRUE(Dispatch);
  ASSERT_EQ(Dispatch->Arguments.size(), 2u);
  EXPECT_EQ(Dispatch->Arguments[0], FDO);
  copyDown(IRP);
  EXPECT_EQ(call("PoCallDriver", {PDO, IRP}), StatusSuccess);
  success(Model->validateGuestAccess(FDO + DeviceFlagsOffset, 4, false));
  finishWorker(*Dispatch, StatusSuccess);
  EXPECT_TRUE(Result.Requests.back().Completed);
  denied(FDO + DeviceFlagsOffset, 4);
}

TEST_F(KernelPowerCompletion, QueuedPowerMPRRetainsItsTerminalCallback) {
  initialize({operation(DevicePowerState::D3)});
  const auto Old = call("KfRaiseIrql", {scheduler::DispatchLevel});
  const uint64_t IRP = queuePower(DevicePowerState::D3);
  ASSERT_NE(IRP, 0u);
  call("KeLowerIrql", {Old});
  const auto Dispatch = powerWorker();
  ASSERT_TRUE(Dispatch);
  copyDown(IRP, CompletionPC);
  EXPECT_EQ(call("PoCallDriver", {PDO, IRP}), StatusSuccess);
  const auto Completion = pendingCall(CompletionPC);
  call("IoMarkIrpPending", {IRP});
  finishCall(Completion, StatusMoreProcessingRequired, StatusSuccess);
  finishWorker(*Dispatch, StatusPending);
  EXPECT_TRUE(Model->requestPending(IRP));
  EXPECT_FALSE(Result.Requests.back().Completed);
  call("IofCompleteRequest", {IRP, 0});
  const auto Terminal = pendingCall(PowerPC);
  ASSERT_EQ(Terminal.Arguments.size(), 5u);
  EXPECT_EQ(get(Terminal.Arguments[4], 4), StatusSuccess);
  finishCall(Terminal, 0, 0);
  EXPECT_FALSE(Model->requestPending());
}

TEST_F(KernelPowerCompletion, QueuedPowerDelayedProviderKeepsRealPendingState) {
  initialize({operation(DevicePowerState::D3, 19)});
  const auto Old = call("KfRaiseIrql", {scheduler::DispatchLevel});
  const uint64_t IRP = queuePower(DevicePowerState::D3);
  ASSERT_NE(IRP, 0u);
  call("KeLowerIrql", {Old});
  const auto Dispatch = powerWorker();
  ASSERT_TRUE(Dispatch);
  copyDown(IRP);
  EXPECT_EQ(call("PoCallDriver", {PDO, IRP}), StatusPending);
  finishWorker(*Dispatch, StatusPending);
  EXPECT_FALSE(Result.Requests.back().Completed);
  EXPECT_EQ(Result.Requests.back().Power->DeviceStateAfter,
            DevicePowerState::D0);
  const auto Completion = scheduled(PowerPC);
  EXPECT_EQ(Completion.DueTime100ns, 19u);
  finishWorker(Completion, StatusSuccess);
  EXPECT_EQ(Result.Requests.back().Power->DeviceStateAfter,
            DevicePowerState::D3);
}
} // namespace
} // namespace neverd::emulation
