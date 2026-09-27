//===- KernelProviderWaitWakeTests.cpp - Provider wake cancellation -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercise the actual provider cancel thunk, callback identity and IRQL
/// restoration through the same guest continuations used by native drivers.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "unicorn/UnicornBackend.h"
#include "windows/DriverImage.h"
#include "windows/KernelModel.h"
#include "windows/WindowsKernelLayout.h"

namespace neverd::emulation {
namespace {
using namespace windows;

class KernelProviderWaitWake : public ::testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000;
  static constexpr uint64_t DispatchPC = 0x180001000;
  static constexpr uint64_t CompletionPC = 0x180001100;
  static constexpr uint64_t PowerPC = 0x180001200;
  KernelExportRegistry Exports;
  std::unique_ptr<UnicornBackend> Memory;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;
  uint64_t PDO = 0, FDO = 0;

  void success(llvm::Error E) {
    if (E)
      ADD_FAILURE() << llvm::toString(std::move(E));
  }
  template <typename T> T take(llvm::Expected<T> Value) {
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return {};
    }
    return std::move(*Value);
  }
  template <typename T>
  void rejected(llvm::Expected<T> Value, llvm::StringRef Text) {
    ASSERT_FALSE(bool(Value));
    EXPECT_NE(llvm::toString(Value.takeError()).find(Text.str()),
              std::string::npos);
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
  void copyDown(uint64_t IRP, uint8_t Control = 0) {
    const uint64_t Stack = get(IRP + IRPStackPointerOffset);
    std::vector<uint8_t> Prefix(StackCompletionOffset);
    success(Memory->read(Stack, Prefix));
    success(Model->validateGuestAccess(Stack - StackSize, Prefix.size(), true));
    success(Memory->write(Stack - StackSize, Prefix));
    put(Stack - StackSize + StackControlOffset, Control, 1);
    if (Control) {
      put(Stack - StackSize + StackCompletionOffset, CompletionPC);
      put(Stack - StackSize + StackCompletionContextOffset, Scratch + 0x100);
    }
  }
  void SetUp() override {
    Memory = take(UnicornBackend::create(8 * 1024 * 1024));
    ASSERT_TRUE(Memory);
    success(Memory->map(Scratch, 0x10000, Read | Write));
    DriverOptions Options;
    DriverPnpDevice Device;
    Device.ID = "wake0";
    Device.InitialDevicePower = DevicePowerState::D0;
    Device.InitialSystemPower = SystemPowerState::Working;
    Device.WakeCapabilities = DriverWakeCapabilities{true, true};
    Options.PnpDevices.push_back(Device);
    success(Exports.initialize(Options));
    Model = std::make_unique<KernelModel>(*Memory, Result, &Exports);
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = DispatchPC;
    Image.Size = 0x3000;
    success(Model->initialize(Image, Options));
    const uint64_t Extension =
        get(Model->driverObject() + DriverExtensionOffset);
    put(Extension + DriverAddDeviceOffset, DispatchPC);
    for (unsigned Major : {0x16u, 0x1bu})
      put(Model->driverObject() + DriverDispatchOffset + Major * 8, DispatchPC);
    success(Model->finishEntry());
    success(Model->preparePnpDevices());
    PDO = take(Model->beginAddDevice("wake0")).Argument1;
    ASSERT_NE(PDO, 0u);
    EXPECT_EQ(call("IoCreateDevice", {Model->driverObject(), 16, 0,
                                      UnknownDeviceType, 0, 0, Scratch}),
              0u);
    FDO = get(Scratch);
    put(FDO + DeviceFlagsOffset, DevicePowerPageable, 4);
    EXPECT_EQ(call("IoAttachDeviceToDeviceStack", {FDO, PDO}), PDO);
    success(Model->finishAddDevice("wake0", 0));
    DriverRequest Start;
    Start.Kind = DriverRequestKind::Pnp;
    Start.DeviceID = "wake0";
    Start.Pnp = DriverPnpOperation{};
    Start.Pnp->BusCompletion.Status = 0;
    const uint64_t IRP = take(Model->beginRequest(Start)).IRP;
    copyDown(IRP);
    EXPECT_EQ(call("IofCallDriver", {PDO, IRP}), 0u);
    success(Model->recordDispatchReturn(IRP, 0));
    success(Model->finalizeRequest(IRP));
  }
  KernelGuestCall pending(uint64_t PC) {
    auto Call = Model->takeGuestCall();
    EXPECT_TRUE(Call);
    if (!Call)
      return {};
    EXPECT_EQ(Call->PC, PC);
    return *Call;
  }
  void finish(const KernelGuestCall &Call, uint64_t Value,
              uint64_t Expected = 0) {
    auto Finished = take(Model->finishGuestCall(Call.Token, Value));
    ASSERT_TRUE(Finished);
    EXPECT_EQ(*Finished, Expected);
  }
  uint64_t arm(uint8_t Control = 0, uint64_t Callback = 0) {
    EXPECT_EQ(
        call("PoRequestPowerIrp", {FDO, uint8_t(DevicePowerRequest::WaitWake),
                                   uint32_t(SystemPowerState::Sleeping3),
                                   Callback, Scratch + 0x200, Scratch}),
        StatusPending);
    const auto Dispatch = pending(DispatchPC);
    const uint64_t IRP = get(Scratch);
    EXPECT_EQ(Dispatch.Arguments[1], IRP);
    copyDown(IRP, Control);
    EXPECT_EQ(call("IofCallDriver", {PDO, IRP}), StatusPending);
    finish(Dispatch, StatusPending, StatusPending);
    EXPECT_FALSE(Result.Requests.back().Completed);
    return IRP;
  }
  KernelGuestCall cancel(uint64_t IRP) {
    const uint64_t Routine = get(IRP + IRPCancelRoutineOffset);
    EXPECT_NE(Routine, 0u);
    EXPECT_EQ(call("IoCancelIrp", {IRP}), 0u);
    auto Call = pending(Routine);
    EXPECT_EQ(Call.Arguments, (std::vector<uint64_t>{PDO, IRP}));
    Model->enterExecution(11, 11, Call.Token);
    success(Model->beginGuestCall(Call.Token));
    return Call;
  }
  uint64_t invoke(const KernelGuestCall &Call) {
    const auto *Export = Exports.lookup(Call.PC);
    EXPECT_NE(Export, nullptr);
    return Export ? take(Model->call(*Export, Call.Arguments, nullptr)) : 0;
  }
};

TEST_F(KernelProviderWaitWake, ProviderThunkIsPrivateAndRequiresActiveToken) {
  const uint64_t IRP = arm();
  const uint64_t Address = get(IRP + IRPCancelRoutineOffset);
  const auto *Export = Exports.lookup(Address);
  ASSERT_NE(Export, nullptr);
  EXPECT_EQ(Export->Kind, KernelExportRegistry::ExportKind::ProviderFunction);
  EXPECT_EQ(Export->Binding, PDO);
  EXPECT_EQ(KernelModel::argumentCount(*Export), 2u);
  rejected(Exports.resolve(Export->Name), "unspecified");
  rejected(Model->call(*Export, {PDO, IRP}, nullptr), "active owner");
  EXPECT_EQ(get(IRP + IRPCancelRoutineOffset), Address);
  EXPECT_EQ(get(IRP + IRPCancelOffset, 1), 0u);
  const auto Cancel = cancel(IRP);
  Model->enterExecution(12, 12);
  rejected(Model->call(*Export, Cancel.Arguments, nullptr), "active owner");
  EXPECT_FALSE(Result.Requests.back().Completed);
  Model->enterExecution(11, 11, Cancel.Token);
  EXPECT_EQ(invoke(Cancel), 0u);
  finish(Cancel, UINT64_MAX, 1);
  EXPECT_TRUE(Result.Requests.back().Completed);
  EXPECT_EQ(Result.Requests.back().IOStatus, framework::RequestCancelled);
}

TEST_F(KernelProviderWaitWake, CancellationBeforeBusReceiptCompletesAtReceipt) {
  EXPECT_EQ(call("PoRequestPowerIrp",
                 {FDO, uint8_t(DevicePowerRequest::WaitWake),
                  uint32_t(SystemPowerState::Sleeping3), 0, 0, Scratch}),
            StatusPending);
  const auto Dispatch = pending(DispatchPC);
  const uint64_t IRP = get(Scratch);
  EXPECT_EQ(call("IoCancelIrp", {IRP}), 0u);
  EXPECT_FALSE(Model->takeGuestCall());
  EXPECT_EQ(get(IRP + IRPCancelOffset, 1), 1u);
  copyDown(IRP);
  EXPECT_EQ(call("IofCallDriver", {PDO, IRP}), framework::RequestCancelled);
  EXPECT_TRUE(Result.Requests.back().Completed);
  EXPECT_EQ(Result.Requests.back().Power->BusStatus,
            framework::RequestCancelled);
  finish(Dispatch, framework::RequestCancelled, StatusPending);
  EXPECT_FALSE(Model->requestPending());
}

TEST_F(KernelProviderWaitWake, ForeignCancelPointerFailsBeforeMutation) {
  const uint64_t IRP = arm();
  const uint64_t Routine = get(IRP + IRPCancelRoutineOffset);
  for (uint64_t Foreign : {uint64_t(0), CompletionPC}) {
    put(IRP + IRPCancelRoutineOffset, Foreign);
    rejected(Model->call("IoCancelIrp", {IRP}), "provider cancel owner");
    EXPECT_EQ(get(IRP + IRPCancelOffset, 1), 0u);
    EXPECT_FALSE(Result.Requests.back().CancelRequestedAt100ns);
    EXPECT_FALSE(Model->takeGuestCall());
  }
  put(IRP + IRPCancelRoutineOffset, Routine);
  const auto Cancel = cancel(IRP);
  invoke(Cancel);
  finish(Cancel, 0, 1);
}

TEST_F(KernelProviderWaitWake, CancelOnlyCompletionPrecedesTerminalCallback) {
  const uint64_t IRP = arm(StackInvokeOnCancel, PowerPC);
  const auto Cancel = cancel(IRP);
  EXPECT_EQ(call("KeGetCurrentIrql", {}), scheduler::DispatchLevel);
  invoke(Cancel);
  EXPECT_EQ(call("KeGetCurrentIrql", {}), scheduler::PassiveLevel);
  const auto IoCompletion = pending(CompletionPC);
  EXPECT_EQ(IoCompletion.Arguments[0], FDO);
  EXPECT_EQ(IoCompletion.Arguments[1], IRP);
  EXPECT_EQ(get(IRP + IRPCancelRoutineOffset), 0u);
  EXPECT_EQ(get(IRP + IRPCancelOffset, 1), 1u);
  call("IoMarkIrpPending", {IRP});
  auto Intermediate = take(Model->finishGuestCall(IoCompletion.Token, 0));
  EXPECT_FALSE(Intermediate);
  const auto Terminal = pending(PowerPC);
  ASSERT_EQ(Terminal.Arguments.size(), 5u);
  EXPECT_EQ(Terminal.Arguments[0], FDO);
  EXPECT_EQ(Terminal.Arguments[1], uint8_t(DevicePowerRequest::WaitWake));
  EXPECT_EQ(Terminal.Arguments[2], uint32_t(SystemPowerState::Sleeping3));
  EXPECT_EQ(Terminal.Arguments[3], Scratch + 0x200);
  EXPECT_EQ(get(Terminal.Arguments[4], 4), framework::RequestCancelled);
  finish(Terminal, UINT64_MAX);
  finish(Cancel, 0, 1);
  EXPECT_FALSE(Model->requestPending());
  EXPECT_TRUE(Result.Requests.back().Completed);
}

TEST_F(KernelProviderWaitWake, DispatchCancellationKeepsTerminalCallbackIrql) {
  const uint64_t IRP = arm(0, PowerPC);
  Model->enterExecution(11, 11);
  EXPECT_EQ(call("KfRaiseIrql", {scheduler::DispatchLevel}), 0u);
  const auto Cancel = cancel(IRP);
  invoke(Cancel);
  EXPECT_EQ(call("KeGetCurrentIrql", {}), scheduler::DispatchLevel);
  const auto Terminal = pending(PowerPC);
  EXPECT_EQ(get(Terminal.Arguments[4], 4), framework::RequestCancelled);
  finish(Terminal, 0);
  finish(Cancel, 0, 1);
  EXPECT_EQ(call("KeGetCurrentIrql", {}), scheduler::DispatchLevel);
  call("KeLowerIrql", {scheduler::PassiveLevel});
  EXPECT_FALSE(Model->requestPending());
}

TEST_F(KernelProviderWaitWake, CompletionMayHoldCancelledPacketAndResumeLater) {
  const uint64_t IRP = arm(StackInvokeOnCancel, PowerPC);
  const auto Cancel = cancel(IRP);
  invoke(Cancel);
  const auto IoCompletion = pending(CompletionPC);
  call("IoMarkIrpPending", {IRP});
  finish(IoCompletion, StatusMoreProcessingRequired);
  finish(Cancel, 0, 1);
  EXPECT_FALSE(Model->takeGuestCall());
  EXPECT_FALSE(Result.Requests.back().Completed);
  call("IofCompleteRequest", {IRP, 0});
  const auto Terminal = pending(PowerPC);
  finish(Terminal, UINT64_MAX);
  EXPECT_TRUE(Result.Requests.back().Completed);
  EXPECT_FALSE(Model->requestPending());
}

} // namespace
} // namespace neverd::emulation
