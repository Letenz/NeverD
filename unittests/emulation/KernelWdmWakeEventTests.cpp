//===- KernelWdmWakeEventTests.cpp - Captured WDM wake identities --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// An external wake belongs to the retained IRP and its successful START.
/// Cancellation or a restart must never redirect that event to a replacement.
///
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
constexpr llvm::StringLiteral DeviceID = "wake-device";
enum class RequestMajor : uint8_t {
#define NEVERD_DRIVER_REQUEST_KIND(Name, Spelling, Major) Name = Major,
#include "neverd/emulation/DriverRequestKinds.def"
#undef NEVERD_DRIVER_REQUEST_KIND
};

class KernelWdmWakeEvent : public ::testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000;
  static constexpr uint64_t DispatchPC = 0x180001000;
  std::unique_ptr<UnicornBackend> Memory;
  std::unique_ptr<KernelModel> Model;
  KernelExportRegistry Exports;
  DriverResult Result;
  uint64_t PDO = 0, FDO = 0;
  uint32_t NextFile = 1;

  void ok(llvm::Error E) {
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
  void reject(llvm::Expected<T> Value, llvm::StringRef Message) {
    ASSERT_FALSE(bool(Value));
    EXPECT_NE(llvm::toString(Value.takeError()).find(Message.str()),
              std::string::npos);
  }
  uint64_t get(uint64_t Address, unsigned Width = 8) {
    return take(Memory->readInteger(Address, Width));
  }
  void put(uint64_t Address, uint64_t Value, unsigned Width = 8) {
    ok(Model->validateGuestAccess(Address, Width, true));
    ok(Memory->writeInteger(Address, Value, Width));
  }
  uint64_t call(const char *Name, std::initializer_list<uint64_t> Arguments) {
    return take(Model->call(Name, Arguments));
  }
  void copyDown(uint64_t IRP) {
    const uint64_t Stack = get(IRP + IRPStackPointerOffset);
    std::array<uint8_t, StackCompletionOffset> Prefix{};
    ok(Memory->read(Stack, Prefix));
    ok(Memory->write(Stack - StackSize, Prefix));
    put(Stack - StackSize + StackControlOffset, 0, 1);
  }
  DriverRequest pnp(DevicePnpRequest Minor) {
    DriverRequest Request;
    Request.Kind = DriverRequestKind::Pnp;
    Request.DeviceID = DeviceID.str();
    Request.Pnp = DriverPnpOperation{Minor, {StatusSuccess, 0}};
    return Request;
  }
  void completePnp(DevicePnpRequest Minor) {
    const uint64_t IRP = take(Model->beginRequest(pnp(Minor))).IRP;
    copyDown(IRP);
    EXPECT_EQ(call("IofCallDriver", {PDO, IRP}), StatusSuccess);
    ok(Model->recordDispatchReturn(IRP, StatusSuccess));
    ok(Model->finalizeRequest(IRP));
  }
  virtual DriverWakeCapabilities wakeCapabilities() const {
    return {true, true};
  }
  void SetUp() override {
    Memory = take(UnicornBackend::create(8 * 1024 * 1024));
    ASSERT_TRUE(Memory);
    ok(Memory->map(Scratch, 0x10000, Read | Write));
    DriverOptions Options;
    DriverPnpDevice Device;
    Device.ID = DeviceID.str();
    Device.InitialDevicePower = DevicePowerState::D0;
    Device.InitialSystemPower = SystemPowerState::Working;
    Device.InitialReportedDevicePower = DevicePowerState::D0;
    Device.WakeCapabilities = wakeCapabilities();
    Options.PnpDevices.push_back(Device);
    ok(Exports.initialize(Options));
    Model = std::make_unique<KernelModel>(*Memory, Result, &Exports);
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = DispatchPC;
    Image.Size = 0x3000;
    ok(Model->initialize(Image, Options));
    Model->enterForeground();
    Model->enterExecution(1);
    for (auto Major :
         {RequestMajor::Pnp, RequestMajor::Power, RequestMajor::Create})
      put(Model->driverObject() + DriverDispatchOffset +
              uint8_t(Major) * profile::PointerSize,
          DispatchPC);
    put(get(Model->driverObject() + DriverExtensionOffset) +
            DriverAddDeviceOffset,
        DispatchPC);
    ok(Model->finishEntry());
    ok(Model->preparePnpDevices());
    PDO = take(Model->beginAddDevice(DeviceID)).Argument1;
    EXPECT_EQ(call("IoCreateDevice", {Model->driverObject(), 0, 0,
                                      UnknownDeviceType, 0, 0, Scratch}),
              StatusSuccess);
    FDO = get(Scratch);
    put(FDO + DeviceFlagsOffset, DeviceBufferedIO | DevicePowerPageable, 4);
    EXPECT_EQ(call("IoAttachDeviceToDeviceStack", {FDO, PDO}), PDO);
    ok(Model->finishAddDevice(DeviceID, StatusSuccess));
    completePnp(DevicePnpRequest::Start);
  }
  uint64_t arm(SystemPowerState Limit = SystemPowerState::Working) {
    EXPECT_EQ(
        call("PoRequestPowerIrp", {FDO, uint8_t(DevicePowerRequest::WaitWake),
                                   uint32_t(Limit), 0, 0, Scratch}),
        StatusPending);
    auto Dispatch = Model->takeGuestCall();
    EXPECT_TRUE(Dispatch);
    if (!Dispatch)
      return 0;
    const uint64_t IRP = get(Scratch);
    EXPECT_EQ(Dispatch->Arguments[1], IRP);
    copyDown(IRP);
    EXPECT_EQ(call("PoCallDriver", {PDO, IRP}), StatusPending);
    const auto Returned =
        take(Model->finishGuestCall(Dispatch->Token, StatusPending));
    EXPECT_EQ(Returned, StatusPending);
    return IRP;
  }
  DriverRequestResult &result(uint64_t IRP) {
    auto Found = std::find_if(Result.Requests.begin(), Result.Requests.end(),
                              [&](const auto &R) { return R.IRP == IRP; });
    EXPECT_NE(Found, Result.Requests.end());
    return *Found;
  }
  DriverRequest eventRequest(unsigned Count = 1) {
    DriverRequest Request;
    Request.Kind = DriverRequestKind::Create;
    Request.DeviceID = DeviceID.str();
    Request.File = NextFile++;
    Request.PowerPolicyEvents.resize(
        Count, DriverPowerPolicyEvent{0, DeviceID.str(),
                                      DriverPowerPolicyAction::Wake});
    return Request;
  }
  void queueWake(unsigned Count = 1) {
    const uint64_t IRP = take(Model->beginRequest(eventRequest(Count))).IRP;
    put(IRP + IRPStatusOffset, StatusSuccess, 4);
    put(IRP + IRPInformationOffset, 0);
    EXPECT_EQ(call("IofCompleteRequest", {IRP, 0}), 0u);
    ok(Model->recordDispatchReturn(IRP, StatusSuccess));
    ok(Model->finalizeRequest(IRP));
  }
  void cancel(uint64_t IRP) {
    EXPECT_EQ(call("IoCancelIrp", {IRP}), 0u);
    auto Cancel = Model->takeGuestCall();
    ASSERT_TRUE(Cancel);
    const auto *Routine = Exports.lookup(Cancel->PC);
    ASSERT_TRUE(Routine);
    ok(Model->beginGuestCall(Cancel->Token));
    Model->enterExecution(2, 2, Cancel->Token);
    const auto Status = take(Model->call(*Routine, Cancel->Arguments, nullptr));
    EXPECT_FALSE(Model->takeGuestCall());
    const auto Returned = take(Model->finishGuestCall(Cancel->Token, Status));
    EXPECT_EQ(Returned, 1u);
    Model->enterExecution(1);
    EXPECT_EQ(call("KeGetCurrentIrql", {}), scheduler::PassiveLevel);
    EXPECT_TRUE(result(IRP).Completed);
    EXPECT_EQ(result(IRP).IOStatus, framework::RequestCancelled);
  }
};

class KernelWdmSxWakeEvent : public KernelWdmWakeEvent {
protected:
  DriverWakeCapabilities wakeCapabilities() const override {
    return {false, true};
  }
};

TEST_F(KernelWdmSxWakeEvent, SleepingLimitDoesNotGrantWorkingWakeCapability) {
  const auto IRP = arm(SystemPowerState::Sleeping3);
  const auto CancelRoutine = get(IRP + IRPCancelRoutineOffset);
  queueWake();
  reject(Model->nextScheduled(false), "current system state");
  EXPECT_FALSE(result(IRP).Completed);
  EXPECT_FALSE(result(IRP).Power->BusCompletedAt100ns);
  EXPECT_FALSE(Result.PowerPolicyEvents.front().OccurredAt100ns);
  EXPECT_EQ(get(IRP + IRPCancelRoutineOffset), CancelRoutine);
  cancel(IRP);
}

TEST_F(KernelWdmSxWakeEvent, SleepingCapabilityAppliesToTheActualSystemState) {
  const auto IRP = arm(SystemPowerState::Sleeping3);
  DriverRequest Power;
  Power.Kind = DriverRequestKind::Power;
  Power.DeviceID = DeviceID.str();
  Power.Power = DriverPowerOperation{};
  Power.Power->Type = DriverPowerType::System;
  Power.Power->State = uint32_t(SystemPowerState::Sleeping3);
  Power.Power->Action = DriverPowerAction::Sleep;
  Power.Power->BusCompletion = {StatusSuccess, 0};
  const auto Set = take(Model->beginRequest(Power));
  copyDown(Set.IRP);
  EXPECT_EQ(call("PoCallDriver", {PDO, Set.IRP}), StatusSuccess);
  ok(Model->recordDispatchReturn(Set.IRP, StatusSuccess));
  ok(Model->finalizeRequest(Set.IRP));
  queueWake();
  EXPECT_FALSE(take(Model->nextScheduled(false)));
  EXPECT_TRUE(result(IRP).Completed);
  EXPECT_EQ(result(IRP).Power->SystemStateAfter, SystemPowerState::Sleeping3);
  EXPECT_TRUE(Result.PowerPolicyEvents.front().OccurredAt100ns);
}

TEST_F(KernelWdmWakeEvent, CaptureRequiresAnAlreadyRetainedPacket) {
  reject(Model->beginRequest(eventRequest()), "already retained");
  EXPECT_TRUE(Result.PowerPolicyEvents.empty());
}

TEST_F(KernelWdmWakeEvent, CancellingAndRearmingCannotRetargetAnOlderEvent) {
  const auto Original = arm();
  queueWake();
  cancel(Original);
  const auto Replacement = arm();
  ASSERT_NE(Original, Replacement);
  reject(Model->nextScheduled(false), "retained");
  EXPECT_FALSE(result(Replacement).Completed);
  EXPECT_FALSE(result(Replacement).Power->BusCompletedAt100ns);
  EXPECT_FALSE(Result.PowerPolicyEvents.front().OccurredAt100ns);
}

TEST_F(KernelWdmWakeEvent, RestartCannotRetargetAnOlderEvent) {
  const auto Original = arm();
  queueWake();
  cancel(Original);
  completePnp(DevicePnpRequest::QueryStop);
  completePnp(DevicePnpRequest::Stop);
  completePnp(DevicePnpRequest::Start);
  const auto Replacement = arm();
  reject(Model->nextScheduled(false), "earlier START epoch");
  EXPECT_FALSE(result(Replacement).Completed);
  EXPECT_FALSE(Result.PowerPolicyEvents.front().OccurredAt100ns);
}

TEST_F(KernelWdmWakeEvent, WakeDoesNotRestoreDevicePowerImplicitly) {
  const auto IRP = arm();
  DriverRequest Power;
  Power.Kind = DriverRequestKind::Power;
  Power.DeviceID = DeviceID.str();
  Power.Power = DriverPowerOperation{};
  Power.Power->State = uint32_t(DevicePowerState::D2);
  Power.Power->BusCompletion = {StatusSuccess, 0};
  const auto Set = take(Model->beginRequest(Power));
  copyDown(Set.IRP);
  EXPECT_EQ(call("PoCallDriver", {PDO, Set.IRP}), StatusSuccess);
  ok(Model->recordDispatchReturn(Set.IRP, StatusSuccess));
  ok(Model->finalizeRequest(Set.IRP));
  queueWake();
  EXPECT_FALSE(take(Model->nextScheduled(false)));
  EXPECT_TRUE(result(IRP).Completed);
  ASSERT_TRUE(result(IRP).Power);
  EXPECT_EQ(result(IRP).Power->DeviceStateAfter, DevicePowerState::D2);
  EXPECT_EQ(result(IRP).Power->WakeSourcePDO, PDO);
  EXPECT_EQ(Result.PnpDevices.front().DevicePower, DevicePowerState::D2);
  EXPECT_TRUE(Result.PowerPolicyEvents.front().OccurredAt100ns);
}

TEST_F(KernelWdmWakeEvent, InvalidPacketDoesNotConsumeItsCapturedEvent) {
  const auto IRP = arm();
  queueWake();
  const auto Stack = get(IRP + IRPStackPointerOffset);
  put(IRP + IRPStackPointerOffset, Stack + StackSize);
  reject(Model->nextScheduled(false), "stack cursor");
  EXPECT_FALSE(result(IRP).Completed);
  EXPECT_FALSE(result(IRP).Power->BusStatus);
  EXPECT_FALSE(Result.PowerPolicyEvents.front().OccurredAt100ns);
  put(IRP + IRPStackPointerOffset, Stack);
  EXPECT_FALSE(take(Model->nextScheduled(false)));
  EXPECT_TRUE(result(IRP).Completed);
}

TEST_F(KernelWdmWakeEvent, DuplicateDueSignalsFailBeforeEitherCompletes) {
  const auto IRP = arm();
  queueWake(2);
  reject(Model->nextScheduled(false), "same IRP twice");
  EXPECT_FALSE(result(IRP).Completed);
  EXPECT_FALSE(result(IRP).Power->BusStatus);
  for (const auto &Event : Result.PowerPolicyEvents)
    EXPECT_FALSE(Event.OccurredAt100ns);
}

TEST_F(KernelWdmWakeEvent, QueryStopDoesNotRetireTheCapturedStart) {
  const auto IRP = arm();
  queueWake();
  completePnp(DevicePnpRequest::QueryStop);
  EXPECT_FALSE(take(Model->nextScheduled(false)));
  EXPECT_TRUE(result(IRP).Completed);
  EXPECT_EQ(result(IRP).IOStatus, StatusSuccess);
  EXPECT_TRUE(Result.PowerPolicyEvents.front().OccurredAt100ns);
}

TEST_F(KernelWdmWakeEvent, StopRequiresTheOriginatorToDrainItsWake) {
  const auto Wake = arm();
  completePnp(DevicePnpRequest::QueryStop);
  const auto Stop = take(Model->beginRequest(pnp(DevicePnpRequest::Stop)));
  copyDown(Stop.IRP);
  reject(Model->call("IofCallDriver", {PDO, Stop.IRP}), "wait/wake");
  EXPECT_FALSE(result(Wake).Completed);
  EXPECT_FALSE(result(Stop.IRP).Completed);
  EXPECT_EQ(Result.PnpDevices.front().PnpState, DevicePnpState::StopPending);
}
} // namespace
} // namespace neverd::emulation
