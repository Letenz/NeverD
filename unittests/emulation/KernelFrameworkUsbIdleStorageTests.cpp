//===- KernelFrameworkUsbIdleStorageTests.cpp - Native USB storage ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercise native framework USB packet initialization, completion preflight,
/// callback-info retirement and exact framework registration disposal.
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

class KernelFrameworkUsbIdleStorage : public ::testing::Test {
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
  static constexpr uint64_t IdleConfig = Scratch + 0x400;
  static constexpr uint64_t AddPC = 0x180001000;
  std::unique_ptr<UnicornBackend> Memory;
  DriverResult Result;
  KernelExportRegistry Exports;
  std::unique_ptr<KernelModel> Model;
  uint64_t Globals = 0, Device = 0;
  uint64_t IdleIRP = 0, Info = 0;

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
    DriverPnpDevice Config;
    Config.ID = "port";
    Config.InitialDevicePower = DevicePowerState::D0;
    Config.InitialSystemPower = SystemPowerState::Working;
    Config.UsbIdle =
        DriverUsbIdleConfig{DriverUsbIdleRole::IndependentFunction, false};
    Options.PnpDevices.push_back(Config);
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
      Device = get(DeviceSlot);
      success(Model->finishAddDevice(Provider.ID, StatusSuccess));
      put(IdleConfig, policy::IdleSize, 4);
      put(IdleConfig + policy::IdleCapabilities, policy::UsbSelectiveSuspend,
          4);
      put(IdleConfig + policy::IdleDxState, uint32_t(DevicePowerState::D2), 4);
      put(IdleConfig + policy::IdleTimeout, 1, 4);
      put(IdleConfig + policy::IdleUserControl, policy::NoUserControl, 4);
      put(IdleConfig + policy::IdleEnabled, policy::True, 4);
      put(IdleConfig + policy::IdlePowerUpOnSystemWake, policy::UseDefault, 4);
      put(IdleConfig + policy::IdleTimeoutType, policy::DriverManagedTimeout,
          4);
      put(IdleConfig + policy::IdleExcludeD3Cold, policy::UseDefault, 4);
      EXPECT_EQ(invoke(api::WdfDeviceAssignS0IdleSettings,
                       {Globals, Device, IdleConfig}),
                StatusSuccess);
    }
    for (const auto *ID : {"port"}) {
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
    DriverRequest Create;
    Create.Kind = DriverRequestKind::Create;
    Create.DeviceID = "port";
    Create.File = 1;
    Create.PowerPolicyEvents.push_back(
        {0, "port", DriverPowerPolicyAction::Idle});
    const auto Call = take(Model->beginRequest(Create));
    success(Model->finalizeRequest(Call.IRP));
    EXPECT_FALSE(take(Model->nextScheduled(false)));
    EXPECT_FALSE(take(Model->nextScheduled(true)));
    const auto Found = std::find_if(
        Result.Requests.begin(), Result.Requests.end(), [](const auto &R) {
          return R.Origin == DriverRequestOrigin::FrameworkUsbIdle;
        });
    ASSERT_NE(Found, Result.Requests.end());
    IdleIRP = Found->IRP;
    Info = get(get(IdleIRP + IRPStackPointerOffset) + StackType3InputOffset);
    ASSERT_NE(Info, 0u);
    ASSERT_FALSE(HasFailure());
  }
  DriverRequestResult &idle() {
    const auto Found =
        std::find_if(Result.Requests.begin(), Result.Requests.end(),
                     [&](const auto &R) { return R.IRP == IdleIRP; });
    EXPECT_NE(Found, Result.Requests.end());
    return *Found;
  }
  llvm::Expected<uint64_t> stopIdle() {
    const auto Address = take(Exports.insertFrameworkFunction(
        Globals, api::WdfDeviceStopIdleNoTrack));
    return Model->call(*Exports.lookup(Address), {Globals, Device, 1}, nullptr);
  }
  void inaccessible(uint64_t Address, uint32_t Size) {
    auto E = Model->validateGuestAccess(Address, Size, false);
    ASSERT_TRUE(bool(E));
    EXPECT_NE(llvm::toString(std::move(E)).find("freed"), std::string::npos);
  }
};

TEST_F(KernelFrameworkUsbIdleStorage,
       NativePacketHasKernelAbiAndExactInfoLifetime) {
  const uint64_t Stack = get(IdleIRP + IRPStackPointerOffset);
  EXPECT_EQ(get(IdleIRP + IRPRequestorModeOffset, 1), KernelMode);
  EXPECT_EQ(get(IdleIRP + IRPFlagsOffset, 4) & IRPSynchronous, 0u);
  EXPECT_EQ(get(IdleIRP + IRPOriginalFileOffset), 0u);
  EXPECT_EQ(get(Stack + StackFileOffset), 0u);
  EXPECT_EQ(get(Stack + StackIOControlOffset, 4),
            usb_idle::SubmitIdleNotification);
  EXPECT_EQ(get(Stack + StackInputLengthOffset, 4), usb_idle::CallbackInfoSize);
  EXPECT_EQ(get(Stack + StackParametersOffset, 4), 0u);
  const auto *Callback = Exports.lookup(get(Info + usb_idle::CallbackOffset));
  ASSERT_NE(Callback, nullptr);
  EXPECT_EQ(Callback->Kind, KernelExportRegistry::ExportKind::ProviderFunction);
  EXPECT_EQ(Callback->Binding, Result.PnpDevices.front().PDO);
  EXPECT_EQ(get(Info + usb_idle::ContextOffset), IdleIRP);
  EXPECT_FALSE(idle().Completed);
  ASSERT_TRUE(idle().UsbIdle);
  EXPECT_FALSE(idle().UsbIdle->CallbackEnteredAt100ns);
  auto Free = Model->call("IoFreeIrp", {IdleIRP});
  ASSERT_FALSE(bool(Free));
  llvm::consumeError(Free.takeError());
  success(Model->validateGuestAccess(Info, usb_idle::CallbackInfoSize, false));
  EXPECT_EQ(take(stopIdle()), StatusSuccess);
  EXPECT_TRUE(idle().Completed);
  EXPECT_EQ(idle().IOStatus, framework::RequestCancelled);
  EXPECT_TRUE(idle().UsbIdle->CompletedAt100ns);
  EXPECT_FALSE(
      Model->requestPending(IdleIRP, KernelModel::PendingRequestScope::All));
  inaccessible(IdleIRP, IRPSize);
  inaccessible(Info, usb_idle::CallbackInfoSize);
  success(Model->finalizeRequest(IdleIRP));
}

TEST_F(KernelFrameworkUsbIdleStorage,
       CorruptCompletionLeavesInfoAndRegistrationLiveForRetry) {
  const uint64_t Stack = get(IdleIRP + IRPStackPointerOffset);
  put(IdleIRP + IRPStackPointerOffset, Stack + StackSize);
  auto Rejected = stopIdle();
  ASSERT_FALSE(bool(Rejected));
  EXPECT_NE(llvm::toString(Rejected.takeError()).find("stack cursor"),
            std::string::npos);
  EXPECT_FALSE(idle().Completed);
  EXPECT_FALSE(idle().UsbIdle->CompletedAt100ns);
  success(Model->validateGuestAccess(Info, usb_idle::CallbackInfoSize, false));
  put(IdleIRP + IRPStackPointerOffset, Stack);
  EXPECT_EQ(take(stopIdle()), StatusSuccess);
  EXPECT_TRUE(idle().Completed);
  inaccessible(Info, usb_idle::CallbackInfoSize);
}

TEST_F(KernelFrameworkUsbIdleStorage,
       RemoveDrainsParkedPacketBeforeReleasingDeviceRoute) {
  for (auto Kind : {DriverRequestKind::Cleanup, DriverRequestKind::Close}) {
    DriverRequest Request;
    Request.Kind = Kind;
    Request.DeviceID = "port";
    Request.File = 1;
    const auto Call = take(Model->beginRequest(Request));
    success(Model->finalizeRequest(Call.IRP));
    ASSERT_FALSE(HasFailure());
  }
  for (auto Minor : {DevicePnpRequest::QueryRemove, DevicePnpRequest::Remove}) {
    DriverRequest Request;
    Request.Kind = DriverRequestKind::Pnp;
    Request.DeviceID = "port";
    Request.Pnp = DriverPnpOperation{Minor, {StatusSuccess, 0}};
    const auto Call = take(Model->beginRequest(Request));
    EXPECT_FALSE(Model->takeGuestCall());
    if (Minor == DevicePnpRequest::Remove) {
      success(Model->beginFrameworkRemoval(Call.IRP));
      EXPECT_FALSE(Model->takeGuestCall());
    }
    success(Model->finalizeRequest(Call.IRP));
    ASSERT_FALSE(HasFailure());
  }
  EXPECT_TRUE(idle().Completed);
  EXPECT_EQ(idle().IOStatus, framework::RequestCancelled);
  inaccessible(Info, usb_idle::CallbackInfoSize);
  EXPECT_FALSE(
      Model->requestPending(IdleIRP, KernelModel::PendingRequestScope::All));
}

} // namespace
} // namespace neverd::emulation
