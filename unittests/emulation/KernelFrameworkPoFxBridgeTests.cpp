//===- KernelFrameworkPoFxBridgeTests.cpp - KMDF PoFx model integration ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercise copied component settings and framework handle rights through the
/// real WDF binding, PnP lifecycle, kernel API and scheduler bridges.
///
//===----------------------------------------------------------------------===//

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/DriverImage.h"
#include "os/windows/KernelModel.h"
#include "os/windows/WindowsKernelLayout.h"

namespace neverd::emulation {
namespace {
using namespace windows;
using namespace framework;
namespace policy = power_policy;

class KernelFrameworkPoFxBridge : public ::testing::Test {
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
  static constexpr uint64_t Settings = Scratch + 0x500;
  static constexpr uint64_t Component = Scratch + 0x600;
  static constexpr uint64_t IdleStates = Scratch + 0x700;
  static constexpr uint64_t Context = Scratch + 0x800;
  static constexpr uint64_t Thread = 0xa0;
  static constexpr uint64_t AddPC = 0x180001000;
  static constexpr uint64_t PostPC = 0x180001100;
  static constexpr uint64_t PrePC = 0x180001200;
  static constexpr uint64_t IdlePC = 0x180001300;
  static constexpr uint64_t StatePC = 0x180001400;
  std::unique_ptr<UnicornBackend> Memory;
  DriverResult Result;
  KernelExportRegistry Exports;
  std::unique_ptr<KernelModel> Model;
  uint64_t Globals = 0, Device = 0, Handle = 0, StartIRP = 0;

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
  void put(uint64_t Address, uint64_t Value, unsigned Width = 8) {
    success(Memory->writeInteger(Address, Value, Width));
  }
  uint64_t get(uint64_t Address) {
    return take(Memory->readInteger(Address, 8));
  }
  llvm::Expected<uint64_t> invoke(llvm::StringRef Name,
                                  std::initializer_list<uint64_t> Args) {
    auto Address = Exports.insertFrameworkFunction(Globals, Name);
    if (!Address)
      return Address.takeError();
    return Model->call(*Exports.lookup(*Address), Args, nullptr);
  }
  uint64_t call(llvm::StringRef Name, std::initializer_list<uint64_t> Args) {
    return take(Model->call(Name.str(), Args));
  }
  llvm::Expected<uint64_t> assign() {
    return invoke(api::WdfDeviceWdmAssignPowerFrameworkSettings,
                  {Globals, Device, Settings});
  }
  void SetUp() override {
    Memory = take(UnicornBackend::create(8 * 1024 * 1024));
    ASSERT_TRUE(Memory);
    success(Memory->map(Scratch, 0x10000, Read | Write));
    DriverOptions Options;
    DriverPnpDevice PDO;
    PDO.ID = "framework-pofx";
    PDO.InitialDevicePower = DevicePowerState::D0;
    PDO.InitialSystemPower = SystemPowerState::Working;
    PDO.InitialReportedDevicePower = DevicePowerState::D0;
    Options.PnpDevices.push_back(PDO);
    success(Exports.initialize(Options));
    Model = std::make_unique<KernelModel>(*Memory, Result, &Exports);
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = AddPC;
    Image.Size = 0x3000;
    success(Model->initialize(Image, Options));
    Model->enterForeground();
    Model->enterExecution(Thread, Thread);
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
    EXPECT_EQ(take(invoke(api::WdfDriverCreate, {Globals, Model->driverObject(),
                                                 Model->registryPath(), 0,
                                                 DriverConfig, DriverSlot})),
              StatusSuccess);
    success(Model->finishEntry());
    success(Model->preparePnpDevices());
    const auto Add = take(Model->beginAddDevice(PDO.ID));
    EXPECT_EQ(Add.PC, AddPC);
    put(InitSlot, Add.Argument1);
    EXPECT_EQ(
        take(invoke(api::WdfDeviceCreate, {Globals, InitSlot, 0, DeviceSlot})),
        StatusSuccess);
    Device = get(DeviceSlot);
    success(Model->finishAddDevice(PDO.ID, StatusSuccess));
    put(IdleConfig, policy::IdleSize, 4);
    put(IdleConfig + policy::IdleCapabilities, policy::CannotWake, 4);
    put(IdleConfig + policy::IdleDxState, uint32_t(DevicePowerState::D3), 4);
    put(IdleConfig + policy::IdleUserControl, policy::NoUserControl, 4);
    put(IdleConfig + policy::IdleEnabled, policy::True, 4);
    put(IdleConfig + policy::IdleTimeoutType, policy::SystemManagedTimeout, 4);
    put(IdleConfig + policy::IdleExcludeD3Cold, policy::True, 4);
    EXPECT_EQ(take(invoke(api::WdfDeviceAssignS0IdleSettings,
                          {Globals, Device, IdleConfig})),
              StatusSuccess);
    put(Settings, PoFxSettingsSize, 4);
    put(Settings + PoFxSettingsPostRegister, PostPC);
    put(Settings + PoFxSettingsPreUnregister, PrePC);
    put(Settings + PoFxSettingsComponent, Component);
    put(Settings + PoFxSettingsIdleCondition, IdlePC);
    put(Settings + PoFxSettingsIdleState, StatePC);
    put(Settings + PoFxSettingsContext, Context);
    put(Settings + PoFxSettingsDirected, policy::False, 4);
    put(Component + pofx::ComponentIdleStateCount, 2, 4);
    put(Component + pofx::ComponentDeepestWakeableState, 1, 4);
    put(Component + pofx::ComponentIdleStates, IdleStates);
    put(IdleStates + pofx::IdleStateNominalPower, 100, 4);
    put(IdleStates + pofx::IdleStateSize + pofx::IdleStateTransitionLatency,
        10);
    put(IdleStates + pofx::IdleStateSize + pofx::IdleStateResidencyRequirement,
        20);
    put(IdleStates + pofx::IdleStateSize + pofx::IdleStateNominalPower, 25, 4);
  }
  KernelGuestCall start(uint32_t LowerStatus = StatusSuccess) {
    DriverRequest Request;
    Request.Kind = DriverRequestKind::Pnp;
    Request.DeviceID = "framework-pofx";
    Request.Pnp = DriverPnpOperation{DevicePnpRequest::Start, {LowerStatus, 0}};
    StartIRP = take(Model->beginRequest(Request)).IRP;
    auto Call = Model->takeGuestCall();
    if (LowerStatus & profile::NTStatusFailureMask) {
      EXPECT_FALSE(Call);
      return {};
    }
    EXPECT_TRUE(Call);
    if (!Call)
      return {};
    EXPECT_EQ(Call->PC, PostPC);
    EXPECT_EQ(Call->Token.Owner, GuestCallOwner::Framework);
    EXPECT_EQ(Call->Arguments.size(), 2u);
    if (Call->Arguments.size() == 2) {
      EXPECT_EQ(Call->Arguments[0], Device);
      Handle = Call->Arguments[1];
    }
    success(Model->beginGuestCall(Call->Token));
    return *Call;
  }
  void finishStart(const KernelGuestCall &Post) {
    take(Model->finishGuestCall(Post.Token, StatusSuccess));
    success(Model->finalizeRequest(StartIRP));
  }
  KernelScheduler::Invocation idleCallback() {
    DriverRequest Request;
    Request.Kind = DriverRequestKind::Create;
    Request.DeviceID = "framework-pofx";
    Request.PowerPolicyEvents.push_back(
        {0, Request.DeviceID, DriverPowerPolicyAction::Idle});
    EXPECT_EQ(take(Model->beginRequest(Request)).PC, 0u);
    const auto Call = take(Model->nextScheduled(false));
    EXPECT_TRUE(Call);
    if (!Call)
      return {};
    EXPECT_EQ(Call->Kind, KernelScheduler::CallbackKind::PoFx);
    EXPECT_EQ(Call->PC, IdlePC);
    EXPECT_EQ(Call->Arguments, (std::vector<uint64_t>{Context, 0}));
    Model->enterExecution(Call->ID, Call->ID);
    return *Call;
  }
  void finish(const KernelScheduler::Invocation &Call) {
    EXPECT_FALSE(take(Model->continueScheduled(Call.ID, 0)));
    success(Model->finishScheduled(Call.ID));
    Model->enterForeground();
    Model->enterExecution(Thread, Thread);
  }
};

TEST_F(KernelFrameworkPoFxBridge, PostRegisterHandlePermitsHintsAndCompletion) {
  EXPECT_EQ(take(assign()), StatusSuccess);
  const auto Post = start();
  ASSERT_NE(Handle, 0u);
  call("PoFxSetComponentLatency", {Handle, 0, 10});
  call("PoFxSetComponentResidency", {Handle, 0, 20});
  call("PoFxSetComponentWake", {Handle, 0, 1});
  reject(Model->call("PoFxCompleteIdleCondition", {Handle, 0}), "pending");
  reject(Model->call("PoFxCompleteIdleState", {Handle, 0}), "pending");
  finishStart(Post);
  const auto Idle = idleCallback();
  call("PoFxCompleteIdleCondition", {Handle, 0});
  finish(Idle);
  EXPECT_FALSE(take(Model->nextScheduled(false)));
}

TEST_F(KernelFrameworkPoFxBridge,
       FrameworkOwnsActivityAndRegistrationLifetime) {
  EXPECT_EQ(take(assign()), StatusSuccess);
  const auto Post = start();
  for (const auto Name :
       {"PoFxStartDevicePowerManagement", "PoFxUnregisterDevice",
        "PoFxReportDevicePoweredOn", "PoFxCompleteDevicePowerNotRequired"})
    reject(Model->call(Name, {Handle}), "owned by the framework");
  for (const auto Name : {"PoFxActivateComponent", "PoFxIdleComponent"})
    reject(Model->call(Name, {Handle, 0, pofx::FlagAsyncOnly}),
           "owned by the framework");
  reject(Model->call("PoFxSetDeviceIdleTimeout", {Handle, 0}),
         "owned by the framework");
  for (bool Write : {false, true}) {
    auto Error = Model->validateGuestAccess(Handle, 8, Write);
    ASSERT_TRUE(bool(Error));
    EXPECT_NE(llvm::toString(std::move(Error)).find("opaque PoFx"),
              std::string::npos);
  }
  finishStart(Post);
  const auto Idle = idleCallback();
  call("PoFxCompleteIdleCondition", {Handle, 0});
  finish(Idle);
}

TEST_F(KernelFrameworkPoFxBridge,
       SharedComponentDecoderRejectsAndCopiesSettings) {
  put(Component + pofx::ComponentIdleStateCount, 0, 4);
  reject(assign(), "idle-state count");
  put(Component + pofx::ComponentIdleStateCount, 2, 4);
  put(IdleStates + pofx::IdleStateTransitionLatency, 1);
  reject(assign(), "F0");
  put(IdleStates + pofx::IdleStateTransitionLatency, 0);
  EXPECT_EQ(take(assign()), StatusSuccess);
  put(Settings + PoFxSettingsIdleCondition, UINT64_MAX);
  put(Settings + PoFxSettingsContext, UINT64_MAX);
  put(Component + pofx::ComponentIdleStateCount, 0, 4);
  put(Component + pofx::ComponentIdleStates, UINT64_MAX);
  const auto Post = start();
  finishStart(Post);
  const auto Idle = idleCallback();
  call("PoFxCompleteIdleCondition", {Handle, 0});
  finish(Idle);
}

TEST_F(KernelFrameworkPoFxBridge,
       FailedLowerStartDoesNotPublishFrameworkHandle) {
  EXPECT_EQ(take(assign()), StatusSuccess);
  start(StatusUnsuccessful);
  EXPECT_EQ(Handle, 0u);
  success(Model->finalizeRequest(StartIRP));
  EXPECT_FALSE(take(Model->nextScheduled(false)));
  const auto Post = start();
  ASSERT_NE(Handle, 0u);
  finishStart(Post);
  const auto Idle = idleCallback();
  call("PoFxCompleteIdleCondition", {Handle, 0});
  finish(Idle);
}
} // namespace
} // namespace neverd::emulation
