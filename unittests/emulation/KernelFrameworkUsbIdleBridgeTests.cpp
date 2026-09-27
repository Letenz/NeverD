//===- KernelFrameworkUsbIdleBridgeTests.cpp - Native USB idle packets ----===//
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

namespace neverd::emulation {
namespace {
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
    if (Wake) {
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
    put(IdleConfig + policy::IdleTimeoutType, policy::DriverManagedTimeout, 4);
    put(IdleConfig + policy::IdleExcludeD3Cold, policy::True, 4);
    EXPECT_EQ(invoke(api::WdfDeviceAssignS0IdleSettings,
                     {Globals, Device, IdleConfig}),
              StatusSuccess);
    DriverRequest Start;
    Start.Kind = DriverRequestKind::Pnp;
    Start.DeviceID = "port";
    Start.Pnp = DriverPnpOperation{DevicePnpRequest::Start, {StatusSuccess, 0}};
    const auto Call = take(Model->beginRequest(Start));
    EXPECT_FALSE(Model->takeGuestCall());
    success(Model->finalizeRequest(Call.IRP));
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

TEST_F(KernelFrameworkUsbIdleBridge, TimeoutOwnsARealPacketAndCallbackInfo) {
  initialize();
  ASSERT_FALSE(HasFailure());
  const auto IRP = submit();
  ASSERT_NE(IRP, 0u);
  ASSERT_FALSE(HasFailure());
  const auto Info = info(IRP);
  ASSERT_NE(Info, 0u);
  const auto PC = get(Info + usb_idle::CallbackOffset);
  const auto *Entry = Exports.lookup(PC);
  ASSERT_NE(Entry, nullptr);
  EXPECT_EQ(Entry->Kind, KernelExportRegistry::ExportKind::ProviderFunction);
  EXPECT_EQ(Entry->Binding, PDO);
  EXPECT_EQ(get(Info + usb_idle::ContextOffset), IRP);
  EXPECT_EQ(get(IRP + IRPStackCountOffset, 1), 2u);
  EXPECT_EQ(get(IRP + IRPCancelRoutineOffset), 0u);
  EXPECT_EQ(observation(IRP).Kind, DriverRequestKind::InternalDeviceControl);
  EXPECT_EQ(observation(IRP).DispatchStatus, StatusPending);
  ASSERT_TRUE(observation(IRP).UsbIdle);
  EXPECT_EQ(observation(IRP).UsbIdle->BusReceivedAt100ns,
            policy::TicksPerMillisecond);
  EXPECT_FALSE(observation(IRP).UsbIdle->CallbackEnteredAt100ns);
  EXPECT_FALSE(observation(IRP).UsbIdle->D2IRP);
  EXPECT_EQ(powerCount(), 0u);
  EXPECT_TRUE(Model->requestPending(IRP));
  EXPECT_FALSE(Model->requestPending(
      IRP, KernelModel::PendingRequestScope::ExcludePowerParked));
  rejected(Model->call("IoFreeIrp", {IRP}), "allocated");
  rejected(Model->call("IoCompleteRequest", {IRP, 0}), "framework owns");
  rejected(Model->call("IoCancelIrp", {IRP}), "WDM-owned");
  EXPECT_EQ(get(IRP + IRPCancelOffset, 1), 0u);
  success(Model->validateGuestAccess(Info, usb_idle::CallbackInfoSize, false));
  EXPECT_FALSE(take(Model->nextScheduled(true)));
  EXPECT_EQ(powerCount(), 0u);
}

TEST_F(KernelFrameworkUsbIdleBridge,
       RetainedStopIdleReleasesOnlyNativeStorage) {
  initialize();
  ASSERT_FALSE(HasFailure());
  const auto IRP = submit();
  ASSERT_NE(IRP, 0u);
  ASSERT_FALSE(HasFailure());
  const auto Info = info(IRP);
  EXPECT_EQ(invoke(api::WdfDeviceStopIdleNoTrack, {Globals, Device, 1}),
            StatusSuccess);
  EXPECT_TRUE(observation(IRP).Completed);
  EXPECT_EQ(observation(IRP).IOStatus, StatusCancelled);
  EXPECT_EQ(observation(IRP).UsbIdle->CompletionCause,
            UsbIdleCompletionCause::Cancel);
  EXPECT_FALSE(Model->requestPending(IRP));
  rejected(Model->validateGuestAccess(IRP, 1, false), "freed");
  rejected(Model->validateGuestAccess(Info, 1, false), "freed");
  EXPECT_EQ(powerCount(), 0u);
  success(Model->validateGuestAccess(Scratch, 1, false));
}

TEST_F(KernelFrameworkUsbIdleBridge,
       NativePermissionWaitsForRealD2AndCancellationPrecedesD0Ack) {
  initialize();
  ASSERT_FALSE(HasFailure());
  const auto IRP = submit();
  ASSERT_NE(IRP, 0u);
  ASSERT_FALSE(HasFailure());
  const auto Info = info(IRP);
  event(DriverPowerPolicyAction::UsbIdlePermission);
  EXPECT_FALSE(take(Model->nextScheduled(false)));
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(powerCount(), 1u);
  ASSERT_TRUE(observation(IRP).UsbIdle->D2IRP);
  const uint64_t D2 = *observation(IRP).UsbIdle->D2IRP;
  EXPECT_EQ(observation(D2).Power->State, uint32_t(DevicePowerState::D2));
  EXPECT_TRUE(observation(IRP).UsbIdle->CallbackEnteredAt100ns);
  EXPECT_FALSE(observation(IRP).UsbIdle->CallbackReturnedAt100ns);
  EXPECT_FALSE(observation(D2).Completed);
  EXPECT_EQ(invoke(api::WdfDeviceStopIdleNoTrack, {Globals, Device, 0}),
            StatusPending);
  EXPECT_FALSE(observation(IRP).Completed);
  success(Model->validateGuestAccess(Info, 1, false));
  EXPECT_FALSE(take(Model->nextScheduled(true)));
  ASSERT_FALSE(HasFailure());
  EXPECT_TRUE(observation(D2).Completed);
  EXPECT_EQ(observation(IRP).UsbIdle->D2Status, StatusSuccess);
  EXPECT_EQ(observation(IRP).UsbIdle->CallbackReturnedAt100ns,
            policy::TicksPerMillisecond + D2Delay);
  EXPECT_TRUE(observation(IRP).Completed);
  EXPECT_EQ(observation(IRP).UsbIdle->CompletionCause,
            UsbIdleCompletionCause::Cancel);
  rejected(Model->validateGuestAccess(IRP, 1, false), "freed");
  rejected(Model->validateGuestAccess(Info, 1, false), "freed");
  EXPECT_EQ(powerCount(), 2u);
  const uint64_t D0 = Result.Requests.back().IRP;
  ASSERT_TRUE(observation(D0).Power);
  EXPECT_EQ(observation(D0).Power->State, uint32_t(DevicePowerState::D0));
  EXPECT_FALSE(observation(D0).Completed);
  EXPECT_FALSE(take(Model->nextScheduled(true)));
  EXPECT_TRUE(observation(D0).Completed);
  EXPECT_EQ(observation(D0).Power->DeviceStateAfter, DevicePowerState::D0);
  EXPECT_FALSE(Model->requestPending());
}

TEST_F(KernelFrameworkUsbIdleBridge,
       MissingD2ResponseRejectsBeforeEnteringNativeCallback) {
  initialize(false);
  ASSERT_FALSE(HasFailure());
  const auto IRP = submit();
  ASSERT_NE(IRP, 0u);
  ASSERT_FALSE(HasFailure());
  const auto Info = info(IRP);
  event(DriverPowerPolicyAction::UsbIdlePermission);
  rejected(Model->nextScheduled(false), "D2");
  EXPECT_FALSE(observation(IRP).UsbIdle->CallbackEnteredAt100ns);
  EXPECT_FALSE(observation(IRP).UsbIdle->CallbackReturnedAt100ns);
  EXPECT_FALSE(observation(IRP).UsbIdle->D2IRP);
  EXPECT_FALSE(observation(IRP).Completed);
  EXPECT_EQ(powerCount(), 0u);
  success(Model->validateGuestAccess(IRP + IRPCancelOffset, 1, false));
  success(Model->validateGuestAccess(Info, 1, false));
}
TEST_F(KernelFrameworkUsbIdleBridge,
       AllocationExhaustedDuringArmCancelsWithoutConsumingD2Response) {
  initialize(true, true);
  ASSERT_FALSE(HasFailure());
  const auto IRP = submit();
  ASSERT_NE(IRP, 0u);
  ASSERT_FALSE(HasFailure());
  const auto Info = info(IRP);
  event(DriverPowerPolicyAction::UsbIdlePermission);
  auto Next = take(Model->nextScheduled(false));
  ASSERT_TRUE(Next);
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(Next->PC, ArmPC);
  EXPECT_EQ(Next->Kind, KernelScheduler::CallbackKind::FrameworkDeferred);
  EXPECT_EQ(Next->Arguments, (std::vector<uint64_t>{Device}));
  const auto Token = Model->scheduledGuestCall(Next->ID);
  success(Model->beginGuestCall(Token));
  Model->enterExecution(profile::CallbackStackBase, Next->ID, Token);
  constexpr uint32_t Tag = 0x4d726141;
  const auto Probe =
      take(Model->call("ExAllocatePoolWithTag", {0, PoolAlignment, Tag}));
  ASSERT_NE(Probe, 0u);
  const auto NextPage = (Probe + PoolAlignment + profile::PageSize - 1) &
                        ~(profile::PageSize - 1);
  const auto Remaining =
      profile::KernelArenaBase + profile::KernelArenaSize - NextPage;
  ASSERT_GT(Remaining, 0u);
  EXPECT_NE(take(Model->call("ExAllocatePoolWithTag", {0, Remaining, Tag})),
            0u);
  auto Continued = take(Model->continueScheduled(Next->ID, StatusSuccess));
  ASSERT_TRUE(Continued);
  EXPECT_EQ(Continued->PC, DisarmPC);
  EXPECT_EQ(Continued->Arguments, (std::vector<uint64_t>{Device}));
  success(Model->beginGuestCall(Continued->Token));
  Model->enterExecution(profile::CallbackStackBase, Next->ID, Continued->Token);
  EXPECT_FALSE(take(Model->continueScheduled(Next->ID, 0)));
  success(Model->finishScheduled(Next->ID));
  Model->enterExecution(profile::StackBase);
  EXPECT_EQ(powerCount(), 0u);
  EXPECT_FALSE(observation(IRP).UsbIdle->D2IRP);
  EXPECT_FALSE(observation(IRP).UsbIdle->D2Status);
  EXPECT_TRUE(observation(IRP).UsbIdle->CallbackReturnedAt100ns);
  EXPECT_TRUE(observation(IRP).Completed);
  EXPECT_EQ(observation(IRP).UsbIdle->CompletionCause,
            UsbIdleCompletionCause::Cancel);
  rejected(Model->validateGuestAccess(IRP, 1, false), "freed");
  rejected(Model->validateGuestAccess(Info, 1, false), "freed");
  const auto Wake = std::find_if(
      Result.Requests.begin(), Result.Requests.end(), [](const auto &R) {
        return R.Origin == DriverRequestOrigin::FrameworkWaitWake;
      });
  ASSERT_NE(Wake, Result.Requests.end());
  EXPECT_TRUE(Wake->Completed);
  EXPECT_EQ(Wake->IOStatus, StatusCancelled);
  EXPECT_EQ(invoke(api::WdfDeviceStopIdleNoTrack, {Globals, Device, 1}),
            StatusSuccess);
}
} // namespace
} // namespace neverd::emulation
