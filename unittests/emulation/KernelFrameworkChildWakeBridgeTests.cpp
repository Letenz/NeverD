//===- KernelFrameworkChildWakeBridgeTests.cpp - Retained child wake ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercise parent wake against real provider-owned WAIT_WAKE packets. A bad
/// member must reject propagation before any other retained packet completes.
///
//===----------------------------------------------------------------------===//

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

#include "llvm/Support/JSON.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
using namespace windows;
using namespace framework;
namespace policy = power_policy;

class KernelFrameworkChildWakeBridge : public ::testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000;
  static constexpr uint64_t BindInfo = Scratch;
  static constexpr uint64_t LibraryName = Scratch + 0x100;
  static constexpr uint64_t TableSlot = Scratch + 0x180;
  static constexpr uint64_t GlobalsSlot = Scratch + 0x188;
  static constexpr uint64_t DriverConfig = Scratch + 0x200;
  static constexpr uint64_t DriverSlot = Scratch + 0x280;
  static constexpr uint64_t InitSlot = Scratch + 0x300;
  static constexpr uint64_t DeviceSlot = Scratch + 0x308;
  static constexpr uint64_t WakeConfig = Scratch + 0x400;
  static constexpr uint64_t AddPC = 0x180001000;
  std::unique_ptr<UnicornBackend> Memory;
  DriverResult Result;
  KernelExportRegistry Exports;
  std::unique_ptr<KernelModel> Model;
  uint64_t Globals = 0;

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
  void put(uint64_t Address, uint64_t Value, unsigned Width = 8) {
    success(Memory->writeInteger(Address, Value, Width));
  }
  uint64_t get(uint64_t Address, unsigned Width = 8) {
    return take(Memory->readInteger(Address, Width));
  }
  uint64_t invoke(llvm::StringRef Name,
                  std::initializer_list<uint64_t> Arguments) {
    const auto Address = take(Exports.insertFrameworkFunction(Globals, Name));
    return take(Model->call(*Exports.lookup(Address), Arguments, nullptr));
  }
  void SetUp() override {
    Memory = take(UnicornBackend::create(8 * 1024 * 1024));
    ASSERT_TRUE(Memory);
    success(Memory->map(Scratch, 0x10000, Read | Write));
    DriverOptions Options;
    for (const auto *ID : {"parent", "child-a", "child-b"}) {
      DriverPnpDevice Device;
      Device.ID = ID;
      Device.InitialDevicePower = DevicePowerState::D0;
      Device.InitialSystemPower = SystemPowerState::Working;
      Device.InitialReportedDevicePower = DevicePowerState::D0;
      Device.WakeCapabilities = DriverWakeCapabilities{false, true};
      if (Device.ID != "parent")
        Device.ParentID = "parent";
      for (auto State : {DevicePowerState::D3, DevicePowerState::D0}) {
        DriverPowerOperation Operation;
        Operation.State = uint32_t(State);
        Operation.BusCompletion = {StatusSuccess, 0};
        Device.RequestedDevicePower.push_back(Operation);
      }
      Options.PnpDevices.push_back(Device);
    }
    success(Exports.initialize(Options));
    Model = std::make_unique<KernelModel>(*Memory, Result, &Exports);
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = AddPC;
    Image.Size = 0x3000;
    success(Model->initialize(Image, Options));
    Model->enterForeground();
    Model->enterExecution(0xa0, 0xa0);
    constexpr llvm::StringLiteral Name = "KmdfLibrary";
    for (size_t I = 0; I < Name.size(); ++I)
      put(LibraryName + I * 2, Name[I], 2);
    put(BindInfo, BindV1Size, 4);
    put(BindInfo + BindComponent, LibraryName);
    put(BindInfo + BindMajor, MajorVersion, 4);
    put(BindInfo + BindMinor, MinorVersion, 4);
    put(BindInfo + BindCount, FunctionCount, 4);
    put(BindInfo + BindTable, TableSlot);
    const auto Bind =
        take(Exports.bindImport({0, "WDFLDR.SYS", "WdfVersionBind"}));
    EXPECT_EQ(take(Model->call(*Exports.lookup(Bind),
                               {Model->driverObject(), Model->registryPath(),
                                BindInfo, GlobalsSlot},
                               nullptr)),
              StatusSuccess);
    Globals = get(GlobalsSlot);
    put(DriverConfig, DriverConfigSize, 4);
    put(DriverConfig + DriverConfigAddDevice, AddPC);
    EXPECT_EQ(invoke(api::WdfDriverCreate,
                     {Globals, Model->driverObject(), Model->registryPath(), 0,
                      DriverConfig, DriverSlot}),
              StatusSuccess);
    success(Model->finishEntry());
    success(Model->preparePnpDevices());
    for (const auto &Provider : Options.PnpDevices) {
      const auto Add = take(Model->beginAddDevice(Provider.ID));
      put(InitSlot, Add.Argument1);
      EXPECT_EQ(
          invoke(api::WdfDeviceCreate, {Globals, InitSlot, 0, DeviceSlot}),
          StatusSuccess);
      const uint64_t Device = get(DeviceSlot);
      success(Model->finishAddDevice(Provider.ID, StatusSuccess));
      put(WakeConfig, policy::WakeSize, 4);
      put(WakeConfig + policy::WakeDxState, uint32_t(DevicePowerState::D3), 4);
      put(WakeConfig + policy::WakeUserControl, policy::NoUserControl, 4);
      put(WakeConfig + policy::WakeEnabled, policy::True, 4);
      const bool Parent = Provider.ID == "parent";
      put(WakeConfig + policy::WakeChildren, Parent, 1);
      put(WakeConfig + policy::WakePropagate, Parent, 1);
      EXPECT_EQ(invoke(api::WdfDeviceAssignSxWakeSettings,
                       {Globals, Device, WakeConfig}),
                StatusSuccess);
    }
    for (const auto *ID : {"parent", "child-a", "child-b"}) {
      DriverRequest Request;
      Request.Kind = DriverRequestKind::Pnp;
      Request.DeviceID = ID;
      Request.Pnp =
          DriverPnpOperation{DevicePnpRequest::Start, {StatusSuccess, 0}};
      const auto Call = take(Model->beginRequest(Request));
      EXPECT_FALSE(Model->takeGuestCall());
      success(Model->finalizeRequest(Call.IRP));
      ASSERT_FALSE(HasFailure());
    }
    for (const auto *ID : {"child-a", "child-b", "parent"}) {
      systemPower(ID, SystemPowerState::Sleeping3);
      ASSERT_FALSE(HasFailure());
    }
  }
  void systemPower(llvm::StringRef ID, SystemPowerState State) {
    DriverRequest Request;
    Request.Kind = DriverRequestKind::Power;
    Request.DeviceID = ID.str();
    DriverPowerOperation Operation;
    Operation.Type = DriverPowerType::System;
    Operation.State = uint32_t(State);
    Operation.BusCompletion = {StatusSuccess, 0};
    Request.Power = Operation;
    const auto Call = take(Model->beginRequest(Request));
    EXPECT_FALSE(Model->takeGuestCall());
    take(Model->nextScheduled(false));
    success(Model->finalizeRequest(Call.IRP));
  }
  DriverRequestResult &wake(llvm::StringRef ID) {
    auto Found = std::find_if(
        Result.Requests.begin(), Result.Requests.end(),
        [&](const auto &Request) {
          return Request.DeviceID == ID &&
                 Request.Origin == DriverRequestOrigin::FrameworkWaitWake;
        });
    EXPECT_NE(Found, Result.Requests.end());
    return *Found;
  }
  void queueWake() {
    DriverRequest Request;
    Request.Kind = DriverRequestKind::Create;
    Request.DeviceID = "parent";
    Request.PowerPolicyEvents.push_back(
        {0, "parent", DriverPowerPolicyAction::Wake});
    const auto Call = take(Model->beginRequest(Request));
    success(Model->finalizeRequest(Call.IRP));
  }
};

TEST_F(KernelFrameworkChildWakeBridge,
       NativeWakeCannotAcquireAFrameworkRouteOrPublishAnOutputPacket) {
  systemPower("parent", SystemPowerState::Working);
  const auto Count = Result.Requests.size();
  const uint64_t Output = Scratch + 0x500;
  put(Output, UINT64_MAX);
  auto Call = Model->call(
      "PoRequestPowerIrp",
      {Result.PnpDevices.front().PDO, uint8_t(DevicePowerRequest::WaitWake),
       uint32_t(SystemPowerState::Sleeping3), 0, 0, Output});
  ASSERT_FALSE(bool(Call));
  EXPECT_NE(llvm::toString(Call.takeError()).find("framework"),
            std::string::npos);
  EXPECT_EQ(Result.Requests.size(), Count);
  EXPECT_EQ(get(Output), UINT64_MAX);
  EXPECT_FALSE(Model->takeGuestCall());
}

TEST_F(KernelFrameworkChildWakeBridge,
       InvalidChildPacketLeavesEveryRetainedWakeAvailableForRetry) {
  const auto ChildIRP = wake("child-b").IRP;
  const auto Cursor = get(ChildIRP + IRPStackPointerOffset);
  put(ChildIRP + IRPStackPointerOffset, Cursor + StackSize);
  queueWake();
  auto Next = Model->nextScheduled(false);
  ASSERT_FALSE(bool(Next));
  EXPECT_NE(llvm::toString(Next.takeError()).find("stack cursor"),
            std::string::npos);
  for (const auto *ID : {"parent", "child-a", "child-b"}) {
    EXPECT_FALSE(wake(ID).Completed);
    EXPECT_FALSE(wake(ID).Power->BusStatus);
    EXPECT_FALSE(wake(ID).Power->WakeSourcePDO);
  }
  EXPECT_FALSE(Result.PowerPolicyEvents.back().OccurredAt100ns);
  put(ChildIRP + IRPStackPointerOffset, Cursor);
  EXPECT_FALSE(take(Model->nextScheduled(false)));
  for (const auto *ID : {"parent", "child-a", "child-b"}) {
    EXPECT_TRUE(wake(ID).Completed);
    EXPECT_EQ(wake(ID).Power->BusStatus, StatusSuccess);
    EXPECT_EQ(wake(ID).Power->WakeSourceDeviceID, "parent");
    EXPECT_EQ(wake(ID).Power->WakeSourcePDO, Result.PnpDevices.front().PDO);
  }
  EXPECT_TRUE(Result.PowerPolicyEvents.back().OccurredAt100ns);
}

TEST_F(KernelFrameworkChildWakeBridge,
       MissingPendingPropagationRejectsBeforeAnyWakeCompletes) {
  const auto ChildIRP = wake("child-b").IRP;
  const auto Stack = get(ChildIRP + IRPStackPointerOffset);
  const auto Control = get(Stack + StackControlOffset, 1);
  put(Stack + StackControlOffset, Control & ~StackPendingReturned, 1);
  queueWake();
  auto Next = Model->nextScheduled(false);
  ASSERT_FALSE(bool(Next));
  EXPECT_NE(
      llvm::toString(Next.takeError()).find("propagation to the top stack"),
      std::string::npos);
  for (const auto *ID : {"parent", "child-a", "child-b"}) {
    EXPECT_FALSE(wake(ID).Completed);
    EXPECT_FALSE(wake(ID).Power->BusStatus);
    EXPECT_FALSE(wake(ID).Power->WakeSourcePDO);
  }
  put(Stack + StackControlOffset, Control, 1);
  EXPECT_FALSE(take(Model->nextScheduled(false)));
  for (const auto *ID : {"parent", "child-a", "child-b"})
    EXPECT_EQ(wake(ID).Power->BusStatus, StatusSuccess);
}

TEST_F(KernelFrameworkChildWakeBridge,
       CancelRoutineRejectsTheWholeBatchBeforePublishingStatus) {
  const auto ChildIRP = wake("child-b").IRP;
  put(ChildIRP + IRPCancelRoutineOffset, AddPC);
  queueWake();
  auto Next = Model->nextScheduled(false);
  ASSERT_FALSE(bool(Next));
  EXPECT_NE(llvm::toString(Next.takeError()).find("cancel routine"),
            std::string::npos);
  for (const auto *ID : {"parent", "child-a", "child-b"})
    EXPECT_FALSE(wake(ID).Completed);
  put(ChildIRP + IRPCancelRoutineOffset, 0);
  EXPECT_FALSE(take(Model->nextScheduled(false)));
  auto JSON = llvm::json::parse(driverResultJSON(Result));
  ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError());
  const auto *Requests = JSON->getAsObject()->getArray("requests");
  ASSERT_NE(Requests, nullptr);
  unsigned CompletedWakes = 0;
  for (const auto &Value : *Requests) {
    const auto &Request = *Value.getAsObject();
    if (Request.getString("origin") != "framework_wait_wake")
      continue;
    const auto *Power = Request.getObject("power");
    ASSERT_NE(Power, nullptr);
    EXPECT_EQ(Power->getString("wake_source_device_id"), "parent");
    EXPECT_TRUE(Power->getString("wake_source_pdo"));
    ++CompletedWakes;
  }
  EXPECT_EQ(CompletedWakes, 3u);
}
} // namespace
} // namespace neverd::emulation
