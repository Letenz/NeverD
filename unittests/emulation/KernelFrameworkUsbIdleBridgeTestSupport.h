//===- KernelFrameworkUsbIdleBridgeTestSupport.h - USB bridge fixture -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TESTS_KERNELFRAMEWORKUSBIDLEBRIDGETESTSUPPORT_H
#define NEVERD_TESTS_KERNELFRAMEWORKUSBIDLEBRIDGETESTSUPPORT_H

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation::framework_usb_test {
using namespace windows;
using namespace framework;
namespace policy = power_policy;

class KernelFrameworkUsbIdleBridge : public ::testing::Test {
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
  static constexpr uint64_t PolicyConfig = Scratch + 0x500;
  static constexpr uint64_t PnpConfig = Scratch + 0xa00;
  static constexpr uint64_t AddPC = 0x180001000;
  static constexpr uint64_t ArmPC = AddPC + 0x100;
  static constexpr uint64_t DisarmPC = AddPC + 0x200;
  static constexpr uint64_t D2Delay = 5;
  static constexpr uint64_t D0Delay = 7;
  std::unique_ptr<UnicornBackend> Memory;
  DriverResult Result;
  KernelExportRegistry Exports;
  std::unique_ptr<KernelModel> Model;
  uint64_t Globals = 0, Device = 0, PDO = 0;
  uint32_t NextFile = 1;
  uint32_t IdleTimeoutType = policy::DriverManagedTimeout;
  uint64_t D0EntryCallback = 0;
  bool ObserveWakeCallbacks = true;

  virtual void configurePowerFramework() {}
  virtual void finishStartRequest(uint64_t IRP) {
    EXPECT_FALSE(Model->takeGuestCall());
    success(Model->finalizeRequest(IRP));
  }

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
  void initialize(bool HasD2 = true, bool Wake = false) {
    Memory = take(UnicornBackend::create(8 * 1024 * 1024));
    ASSERT_TRUE(Memory);
    success(Memory->map(Scratch, 0x10000, Read | Write));
    success(Memory->map(AddPC, profile::PageSize, Read | Execute));
    DriverOptions Options;
    DriverPnpDevice Config;
    Config.ID = "port";
    Config.InitialDevicePower = DevicePowerState::D0;
    Config.InitialReportedDevicePower = DevicePowerState::D0;
    Config.InitialSystemPower = SystemPowerState::Working;
    Config.UsbIdle = DriverUsbIdleConfig{DriverUsbIdleRole::IndependentFunction,
                                         Wake, DevicePowerState::D2};
    if (Wake)
      Config.WakeCapabilities = DriverWakeCapabilities{true, false};
    if (HasD2) {
      DriverPowerOperation Operation;
      Operation.State = uint32_t(DevicePowerState::D2);
      Operation.BusCompletion = {StatusSuccess, D2Delay};
      Config.RequestedDevicePower.push_back(Operation);
    }
    DriverPowerOperation Operation;
    Operation.State = uint32_t(DevicePowerState::D0);
    Operation.BusCompletion = {StatusSuccess, D0Delay};
    Config.RequestedDevicePower.push_back(Operation);
    Options.PnpDevices.push_back(Config);
    success(Exports.initialize(Options));
    Model = std::make_unique<KernelModel>(*Memory, Result, &Exports);
    DriverImage Image;
    Image.Base = AddPC - profile::PageSize;
    Image.Entry = AddPC;
    Image.Size = 3 * profile::PageSize;
    success(Model->initialize(Image, Options));
    Model->enterForeground();
    Model->enterExecution(profile::StackBase);
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
    const auto Add = take(Model->beginAddDevice("port"));
    put(InitSlot, Add.Argument1);
    if (D0EntryCallback) {
      put(PnpConfig, PnpPowerCallbacksSize, 4);
      put(PnpConfig + PnpPowerCallbacksFirstOffset, D0EntryCallback);
      invoke(api::WdfDeviceInitSetPnpPowerEventCallbacks,
             {Globals, Add.Argument1, PnpConfig});
    }
    if (Wake && ObserveWakeCallbacks) {
      put(PolicyConfig, policy::CallbacksSize, 4);
      put(PolicyConfig + policy::CallbacksFirst, ArmPC);
      put(PolicyConfig + policy::CallbacksFirst + profile::PointerSize,
          DisarmPC);
      invoke(api::WdfDeviceInitSetPowerPolicyEventCallbacks,
             {Globals, Add.Argument1, PolicyConfig});
    }
    EXPECT_EQ(invoke(api::WdfDeviceCreate, {Globals, InitSlot, 0, DeviceSlot}),
              StatusSuccess);
    Device = get(DeviceSlot);
    success(Model->finishAddDevice("port", StatusSuccess));
    PDO = Result.PnpDevices.front().PDO;
    put(IdleConfig, policy::IdleSize, 4);
    put(IdleConfig + policy::IdleCapabilities, policy::UsbSelectiveSuspend, 4);
    put(IdleConfig + policy::IdleDxState, policy::DevicePowerMaximum, 4);
    put(IdleConfig + policy::IdleTimeout, 1, 4);
    put(IdleConfig + policy::IdleUserControl, policy::NoUserControl, 4);
    put(IdleConfig + policy::IdleEnabled, policy::True, 4);
    put(IdleConfig + policy::IdlePowerUpOnSystemWake, policy::UseDefault, 4);
    put(IdleConfig + policy::IdleTimeoutType, IdleTimeoutType, 4);
    put(IdleConfig + policy::IdleExcludeD3Cold, policy::True, 4);
    EXPECT_EQ(invoke(api::WdfDeviceAssignS0IdleSettings,
                     {Globals, Device, IdleConfig}),
              StatusSuccess);
    configurePowerFramework();
    DriverRequest Start;
    Start.Kind = DriverRequestKind::Pnp;
    Start.DeviceID = "port";
    Start.Pnp = DriverPnpOperation{DevicePnpRequest::Start, {StatusSuccess, 0}};
    const auto Call = take(Model->beginRequest(Start));
    finishStartRequest(Call.IRP);
  }
  void event(DriverPowerPolicyAction Action) {
    DriverRequest Request;
    Request.Kind = DriverRequestKind::Create;
    Request.DeviceID = "port";
    Request.File = NextFile++;
    Request.PowerPolicyEvents.push_back({0, "port", Action});
    const auto Call = take(Model->beginRequest(Request));
    EXPECT_FALSE(Model->takeGuestCall());
    success(Model->finalizeRequest(Call.IRP));
  }
  uint64_t submit() {
    event(DriverPowerPolicyAction::Idle);
    EXPECT_FALSE(take(Model->nextScheduled(true)));
    auto Found = std::find_if(
        Result.Requests.begin(), Result.Requests.end(), [](const auto &R) {
          return R.Origin == DriverRequestOrigin::FrameworkUsbIdle;
        });
    EXPECT_NE(Found, Result.Requests.end());
    return Found == Result.Requests.end() ? 0 : Found->IRP;
  }
  DriverRequestResult &observation(uint64_t IRP) {
    auto Found = std::find_if(Result.Requests.begin(), Result.Requests.end(),
                              [IRP](const auto &R) { return R.IRP == IRP; });
    EXPECT_NE(Found, Result.Requests.end());
    if (Found != Result.Requests.end())
      return *Found;
    static DriverRequestResult Missing;
    return Missing;
  }
  uint64_t info(uint64_t IRP) {
    return get(IRP + IRPSize + StackType3InputOffset);
  }
  size_t powerCount() const {
    return std::count_if(
        Result.Requests.begin(), Result.Requests.end(), [](const auto &R) {
          return R.Origin == DriverRequestOrigin::FrameworkPowerPolicy;
        });
  }
};

} // namespace neverd::emulation::framework_usb_test
#endif
