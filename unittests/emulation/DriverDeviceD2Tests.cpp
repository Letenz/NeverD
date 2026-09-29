//===- DriverDeviceD2Tests.cpp - Exact D2 power transitions --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercise D2 through real power packets and independent notification state.
/// These tests do not imply USB idle permission or a USB provider contract.
///
//===----------------------------------------------------------------------===//
#include "backends/unicorn/UnicornBackend.h"
#include "fixtures/driver_kmdf_child_wake_test.h"
#include "gtest/gtest.h"
#include "os/windows/DriverImage.h"
#include "os/windows/KernelModel.h"
#include "os/windows/WindowsKernelLayout.h"

#include "neverd/emulation/DriverSession.h"

#include <algorithm>
#include <array>

namespace neverd::emulation {
namespace {
using namespace windows;
constexpr llvm::StringLiteral DeviceID = "d2-device";

enum class RequestMajor : uint8_t {
#define NEVERD_DRIVER_REQUEST_KIND(Name, Spelling, Major) Name = Major,
#include "neverd/emulation/DriverRequestKinds.def"
#undef NEVERD_DRIVER_REQUEST_KIND
};

DriverPowerOperation
operation(DevicePowerState State, uint64_t Delay = 0,
          DevicePowerRequest Minor = DevicePowerRequest::Set) {
  DriverPowerOperation Operation;
  Operation.Minor = Minor;
  Operation.State = uint32_t(State);
  Operation.BusCompletion = {StatusSuccess, Delay};
  return Operation;
}

DriverRequest pnp(DevicePnpRequest Minor) {
  DriverRequest Request;
  Request.Kind = DriverRequestKind::Pnp;
  Request.DeviceID = DeviceID.str();
  Request.Pnp = DriverPnpOperation{Minor, {StatusSuccess, 0}};
  return Request;
}

class DriverDeviceD2 : public ::testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000;
  static constexpr uint64_t DispatchPC = 0x180001000;
  static constexpr uint64_t PowerPC = 0x180001100;
  std::unique_ptr<UnicornBackend> Memory;
  std::unique_ptr<KernelModel> Model;
  DriverResult Result;
  uint64_t PDO = 0, FDO = 0;

  void ok(llvm::Error Error) {
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
    std::array<uint8_t, StackCompletionOffset> Prefix;
    ok(Model->validateGuestAccess(Stack, Prefix.size(), false));
    ok(Memory->read(Stack, Prefix));
    ok(Model->validateGuestAccess(Stack - StackSize, Prefix.size(), true));
    ok(Memory->write(Stack - StackSize, Prefix));
    put(Stack - StackSize + StackControlOffset, 0, 1);
  }
  void initialize(std::vector<DriverPowerOperation> Responses) {
    Memory = take(UnicornBackend::create(8 * 1024 * 1024));
    ASSERT_TRUE(Memory);
    ok(Memory->map(Scratch, 0x10000, Read | Write));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = DispatchPC;
    Image.Size = 0x3000;
    DriverOptions Options;
    DriverPnpDevice Device;
    Device.ID = DeviceID.str();
    Device.InitialDevicePower = DevicePowerState::D0;
    Device.InitialSystemPower = SystemPowerState::Working;
    Device.InitialReportedDevicePower = DevicePowerState::D0;
    Device.RequestedDevicePower = std::move(Responses);
    Options.PnpDevices.push_back(std::move(Device));
    ok(Model->initialize(Image, Options));
    const uint64_t Extension =
        get(Model->driverObject() + DriverExtensionOffset);
    put(Extension + DriverAddDeviceOffset, DispatchPC);
    for (RequestMajor Major : {RequestMajor::Pnp, RequestMajor::Power})
      put(Model->driverObject() + DriverDispatchOffset +
              uint8_t(Major) * sizeof(uint64_t),
          DispatchPC);
    ok(Model->finishEntry());
    ok(Model->preparePnpDevices());
    PDO = take(Model->beginAddDevice(DeviceID)).Argument1;
    ASSERT_NE(PDO, 0u);
    EXPECT_EQ(call("IoCreateDevice", {Model->driverObject(), 0, 0,
                                      UnknownDeviceType, 0, 0, Scratch}),
              0u);
    FDO = get(Scratch);
    put(FDO + DeviceFlagsOffset, DeviceBufferedIO | DevicePowerPageable, 4);
    EXPECT_EQ(call("IoAttachDeviceToDeviceStack", {FDO, PDO}), PDO);
    ok(Model->finishAddDevice(DeviceID, 0));
    const uint64_t IRP =
        take(Model->beginRequest(pnp(DevicePnpRequest::Start))).IRP;
    copyDown(IRP);
    EXPECT_EQ(call("IofCallDriver", {PDO, IRP}), 0u);
    ok(Model->recordDispatchReturn(IRP, 0));
    ok(Model->finalizeRequest(IRP));
  }
  void completeChild(DevicePowerState State, bool Delayed = false,
                     DevicePowerRequest Minor = DevicePowerRequest::Set) {
    EXPECT_EQ(call("PoRequestPowerIrp",
                   {FDO, uint8_t(Minor), uint32_t(State), PowerPC, Scratch, 0}),
              StatusPending);
    auto Dispatch = Model->takeGuestCall();
    ASSERT_TRUE(Dispatch);
    ASSERT_EQ(Dispatch->Arguments.size(), 2u);
    const uint64_t IRP = Dispatch->Arguments[1];
    const uint64_t Stack = get(IRP + IRPStackPointerOffset);
    EXPECT_EQ(get(Stack + StackPowerStateOffset, 4), uint32_t(State));
    EXPECT_EQ(get(Stack + StackPowerTypeOffset, 4),
              uint32_t(DriverPowerType::Device));
    copyDown(IRP);
    EXPECT_EQ(call("PoCallDriver", {PDO, IRP}),
              Delayed ? StatusPending : StatusSuccess);
    if (Delayed) {
      auto Returned =
          take(Model->finishGuestCall(Dispatch->Token, StatusPending));
      ASSERT_TRUE(Returned);
      EXPECT_EQ(*Returned, StatusPending);
      auto Next = take(Model->nextScheduled(true));
      if (!Next)
        Next = take(Model->nextScheduled(false));
      ASSERT_TRUE(Next);
      EXPECT_EQ(Next->PC, PowerPC);
      ASSERT_EQ(Next->Arguments.size(), 5u);
      EXPECT_EQ(Next->Arguments[0], FDO);
      EXPECT_EQ(Next->Arguments[1], uint8_t(Minor));
      EXPECT_EQ(Next->Arguments[2], uint32_t(State));
      EXPECT_FALSE(take(Model->continueScheduled(Next->ID, 0)));
      ok(Model->finishScheduled(Next->ID));
    } else {
      auto Completion = Model->takeGuestCall();
      ASSERT_TRUE(Completion);
      EXPECT_EQ(Completion->PC, PowerPC);
      ASSERT_EQ(Completion->Arguments.size(), 5u);
      EXPECT_EQ(Completion->Arguments[0], FDO);
      EXPECT_EQ(Completion->Arguments[1], uint8_t(Minor));
      EXPECT_EQ(Completion->Arguments[2], uint32_t(State));
      EXPECT_EQ(get(Completion->Arguments[4], 4), StatusSuccess);
      auto Completed = take(Model->finishGuestCall(Completion->Token, 0));
      ASSERT_TRUE(Completed);
      auto Returned =
          take(Model->finishGuestCall(Dispatch->Token, StatusSuccess));
      ASSERT_TRUE(Returned);
      EXPECT_EQ(*Returned, StatusPending);
    }
    ok(Model->finalizeRequest(IRP));
    const auto &Observation = Result.Requests.back();
    EXPECT_TRUE(Observation.Completed);
    EXPECT_EQ(Observation.Origin, DriverRequestOrigin::PoRequestPowerIrp);
    EXPECT_EQ(Observation.IOStatus, StatusSuccess);
    ASSERT_TRUE(Observation.Power);
    EXPECT_EQ(Observation.Power->State, uint32_t(State));
  }
};

TEST_F(DriverDeviceD2, RealPowerFifoPreservesD2AndD0CallbackStates) {
  for (uint64_t Delay : {uint64_t(0), uint64_t(7)}) {
    SCOPED_TRACE(Delay);
    Model.reset();
    Memory.reset();
    Result = {};
    initialize({operation(DevicePowerState::D2, Delay),
                operation(DevicePowerState::D0, Delay),
                operation(DevicePowerState::D3, Delay),
                operation(DevicePowerState::D0, Delay)});
    ASSERT_FALSE(HasFatalFailure());
    for (DevicePowerState State :
         {DevicePowerState::D2, DevicePowerState::D0, DevicePowerState::D3,
          DevicePowerState::D0}) {
      completeChild(State, Delay != 0);
      ASSERT_FALSE(HasFatalFailure());
      EXPECT_EQ(Result.Requests.back().Power->DeviceStateAfter, State);
    }
    ASSERT_EQ(Result.Requests.size(), 5u);
    for (size_t I = 1; I < Result.Requests.size(); ++I)
      EXPECT_EQ(Result.Requests[I].ResponseIndex, I - 1);
  }
}

TEST_F(DriverDeviceD2, QueryD2LeavesPhysicalStateAndNotificationIndependent) {
  initialize({operation(DevicePowerState::D2, 0, DevicePowerRequest::Query)});
  ASSERT_FALSE(HasFatalFailure());
  EXPECT_EQ(call("PoSetPowerState", {FDO, uint32_t(DriverPowerType::Device),
                                     uint32_t(DevicePowerState::D2)}),
            uint32_t(DevicePowerState::D0));
  completeChild(DevicePowerState::D2, false, DevicePowerRequest::Query);
  ASSERT_FALSE(HasFatalFailure());
  EXPECT_EQ(Result.Requests.back().Power->DeviceStateBefore,
            DevicePowerState::D0);
  EXPECT_EQ(Result.Requests.back().Power->DeviceStateAfter,
            DevicePowerState::D0);
  ok(Model->snapshot());
  ASSERT_EQ(Result.PnpDevices.size(), 1u);
  EXPECT_EQ(Result.PnpDevices.front().DevicePower, DevicePowerState::D0);
  const auto Object = std::find_if(
      Result.Devices.begin(), Result.Devices.end(),
      [this](const auto &Device) { return Device.Address == FDO; });
  ASSERT_NE(Object, Result.Devices.end());
  EXPECT_EQ(Object->ReportedDevicePower, DevicePowerState::D2);
}

TEST_F(DriverDeviceD2, LowPowerToLowPowerRequestCannotConsumeItsFifoEntry) {
  initialize(
      {operation(DevicePowerState::D2), operation(DevicePowerState::D3)});
  ASSERT_FALSE(HasFatalFailure());
  completeChild(DevicePowerState::D2);
  ASSERT_FALSE(HasFatalFailure());
  const size_t Count = Result.Requests.size();
  for (unsigned Attempt = 0; Attempt < 2; ++Attempt) {
    auto Rejected =
        Model->call("PoRequestPowerIrp",
                    {FDO, uint8_t(DevicePowerRequest::Set),
                     uint32_t(DevicePowerState::D3), PowerPC, Scratch, 0});
    ASSERT_FALSE(bool(Rejected));
    EXPECT_NE(llvm::toString(Rejected.takeError()).find("D0"),
              std::string::npos);
    EXPECT_EQ(Result.Requests.size(), Count);
    EXPECT_FALSE(Model->takeGuestCall());
  }
  ok(Model->snapshot());
  EXPECT_EQ(Result.PnpDevices.front().DevicePower, DevicePowerState::D2);
}

#ifdef NEVERD_KMDF_CHILD_WAKE_FIXTURE
std::vector<const char *> images() {
  return {
      NEVERD_KMDF_CHILD_WAKE_FIXTURE,
#ifdef NEVERD_KMDF_CHILD_WAKE_CFG_FIXTURE
      NEVERD_KMDF_CHILD_WAKE_CFG_FIXTURE,
#endif
  };
}
DriverRequest file(DriverRequestKind Kind) {
  DriverRequest Request;
  Request.Kind = Kind;
  Request.DeviceID = DeviceID.str();
  Request.File = 1;
  if (Kind == DriverRequestKind::DeviceControl) {
    Request.ControlCode = KmdfChildWakeSnapshotIoctl;
    Request.OutputSize = KmdfChildWakeSnapshotWords * sizeof(uint32_t);
  }
  return Request;
}
DriverOptions options(bool VisitD3) {
  DriverOptions Options;
  Options.ServiceName = "NeverDKmdfD2";
  Options.Unload = true;
  DriverPnpDevice Device;
  Device.ID = DeviceID.str();
  Device.InitialDevicePower = DevicePowerState::D0;
  Device.InitialSystemPower = SystemPowerState::Working;
  Device.D3Cold = DriverD3ColdCapabilities{true, true, false, false};
  Options.PnpDevices.push_back(std::move(Device));
  Options.Requests = {pnp(DevicePnpRequest::Start),
                      file(DriverRequestKind::Create)};
  const auto Power = [&](DevicePowerState State) {
    DriverRequest Request;
    Request.Kind = DriverRequestKind::Power;
    Request.DeviceID = DeviceID.str();
    Request.Power = operation(State, 5);
    Options.Requests.push_back(std::move(Request));
  };
  Power(DevicePowerState::D2);
  Options.Requests.push_back(file(DriverRequestKind::DeviceControl));
  Power(DevicePowerState::D0);
  if (VisitD3) {
    Power(DevicePowerState::D3);
    Power(DevicePowerState::D0);
  }
  Options.Requests.push_back(file(DriverRequestKind::DeviceControl));
  Options.Requests.push_back(file(DriverRequestKind::Cleanup));
  Options.Requests.push_back(file(DriverRequestKind::Close));
  Options.Requests.push_back(pnp(DevicePnpRequest::QueryRemove));
  Options.Requests.push_back(pnp(DevicePnpRequest::Remove));
  return Options;
}
uint32_t word(const DriverRequestResult &Request, size_t Index) {
  uint32_t Value = 0;
  if (Request.Output.size() >= (Index + 1) * sizeof(Value))
    for (size_t I = 0; I < sizeof(Value); ++I)
      Value |= uint32_t(Request.Output[Index * sizeof(Value) + I]) << (I * 8);
  return Value;
}

const DriverRequestResult *scenarioRequest(const DriverResult &Result,
                                           size_t Index) {
  size_t Found = 0;
  for (const auto &Request : Result.Requests)
    if (Request.Origin == DriverRequestOrigin::Scenario && Found++ == Index)
      return &Request;
  return nullptr;
}

DriverRequest systemPower(SystemPowerState State) {
  DriverRequest Request;
  Request.Kind = DriverRequestKind::Power;
  Request.DeviceID = DeviceID.str();
  auto Operation = operation(DevicePowerState::D0, 5);
  Operation.Type = DriverPowerType::System;
  Operation.State = uint32_t(State);
  Operation.Action = State == SystemPowerState::Working
                         ? DriverPowerAction::None
                         : DriverPowerAction::Sleep;
  Request.Power = Operation;
  return Request;
}

DriverOptions systemSleepOptions() {
  auto Options = options(false);
  auto &Device = Options.PnpDevices.front();
  // This driver never assigns wake settings. Moving from its existing D2
  // state to the default S3 target still requires an intermediate D0 IRP.
  Device.RequestedDevicePower = {operation(DevicePowerState::D0, 7),
                                 operation(DevicePowerState::D3, 11),
                                 operation(DevicePowerState::D0, 13)};
  Device.RequestedDevicePower[1].Action = DriverPowerAction::Sleep;
  Options.Requests[4] = systemPower(SystemPowerState::Sleeping3);
  Options.Requests.insert(Options.Requests.begin() + 6,
                          systemPower(SystemPowerState::Working));
  Options.Requests.insert(Options.Requests.begin() + 7,
                          file(DriverRequestKind::DeviceControl));
  return Options;
}
#endif

TEST(DriverDeviceD2Genuine, D0CallbacksBracketD2AndSeparateD3Cycles) {
#ifdef NEVERD_KMDF_CHILD_WAKE_FIXTURE
  for (const auto *Image : images())
    for (bool VisitD3 : {false, true})
      for (uint64_t Base : {uint64_t(0), uint64_t(0x190000000)}) {
        SCOPED_TRACE(Image);
        SCOPED_TRACE(VisitD3);
        auto Input = options(VisitD3);
        Input.LoadAddress = Base;
        auto Result = emulateDriver(Image, Input);
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        ASSERT_EQ(Result->Stop, DriverStopReason::Returned)
            << Result->Diagnostic;
        EXPECT_TRUE(Result->UnloadCompleted);
        ASSERT_EQ(Result->Requests.size(), Input.Requests.size());
        for (const auto &Request : Result->Requests) {
          EXPECT_TRUE(Request.Completed);
          EXPECT_EQ(Request.IOStatus, StatusSuccess);
        }
        const auto &Sleeping = Result->Requests[3];
        ASSERT_EQ(Sleeping.Output.size(),
                  KmdfChildWakeSnapshotWords * sizeof(uint32_t));
        EXPECT_EQ(word(Sleeping, KmdfChildWakeFailures), 0u);
        EXPECT_EQ(word(Sleeping, KmdfChildWakeD0Entries), 1u);
        EXPECT_EQ(word(Sleeping, KmdfChildWakeD0Exits), 1u);
        EXPECT_EQ(word(Sleeping, KmdfChildWakeInD0), 0u);
        const auto &Awake = Result->Requests[VisitD3 ? 7 : 5];
        ASSERT_EQ(Awake.Output.size(),
                  KmdfChildWakeSnapshotWords * sizeof(uint32_t));
        EXPECT_EQ(word(Awake, KmdfChildWakeFailures), 0u);
        EXPECT_EQ(word(Awake, KmdfChildWakeD0Entries), VisitD3 ? 3u : 2u);
        EXPECT_EQ(word(Awake, KmdfChildWakeD0Exits), VisitD3 ? 2u : 1u);
        EXPECT_EQ(word(Awake, KmdfChildWakeInD0), 1u);
        ASSERT_TRUE(Result->Requests[2].Power);
        EXPECT_EQ(Result->Requests[2].Power->DeviceStateAfter,
                  DevicePowerState::D2);
        ASSERT_TRUE(Result->Requests[4].Power);
        EXPECT_EQ(Result->Requests[4].Power->DeviceStateBefore,
                  DevicePowerState::D2);
        EXPECT_EQ(Result->Requests[4].Power->DeviceStateAfter,
                  DevicePowerState::D0);
        EXPECT_NE(std::find(Result->Messages.begin(), Result->Messages.end(),
                            "KMDF child wake: unload live 0 failures 0\n"),
                  Result->Messages.end());
      }
#else
  GTEST_SKIP()
      << "NEVERD_KMDF_CHILD_WAKE_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverDeviceD2Genuine, DirectD2ToD3FailsBeforeASecondD0Exit) {
#ifdef NEVERD_KMDF_CHILD_WAKE_FIXTURE
  for (const auto *Image : images()) {
    auto Input = options(false);
    Input.Requests[4].Power = operation(DevicePowerState::D3, 5);
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
    EXPECT_NE(Result->Diagnostic.find("D0"), std::string::npos)
        << Result->Diagnostic;
    ASSERT_FALSE(Result->PnpDevices.empty());
    EXPECT_EQ(Result->PnpDevices.front().DevicePower, DevicePowerState::D2);
  }
#else
  GTEST_SKIP()
      << "NEVERD_KMDF_CHILD_WAKE_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverDeviceD2Genuine, SystemSleepFromD2RequiresASeparateD0Transaction) {
#ifdef NEVERD_KMDF_CHILD_WAKE_FIXTURE
  for (const auto *Image : images())
    for (uint64_t Base : {uint64_t(0), uint64_t(0x190000000)}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Base);
      auto Input = systemSleepOptions();
      Input.LoadAddress = Base;
      auto Result = emulateDriver(Image, Input);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
      EXPECT_TRUE(Result->UnloadCompleted);
      ASSERT_EQ(Result->Requests.size(), Input.Requests.size() + 3);
      std::vector<const DriverRequestResult *> Children;
      for (const auto &Request : Result->Requests) {
        EXPECT_TRUE(Request.Completed);
        EXPECT_EQ(Request.IOStatus, StatusSuccess);
        if (Request.Origin == DriverRequestOrigin::FrameworkPowerPolicy)
          Children.push_back(&Request);
        EXPECT_NE(Request.Origin, DriverRequestOrigin::FrameworkWaitWake);
      }
      ASSERT_EQ(Children.size(), 3u);
      const std::array Before{DevicePowerState::D2, DevicePowerState::D0,
                              DevicePowerState::D3};
      const std::array After{DevicePowerState::D0, DevicePowerState::D3,
                             DevicePowerState::D0};
      for (size_t I = 0; I < Children.size(); ++I) {
        const auto &Child = *Children[I];
        EXPECT_EQ(Child.ResponseIndex, I);
        ASSERT_TRUE(Child.Power);
        EXPECT_EQ(Child.Power->State, uint32_t(After[I]));
        EXPECT_EQ(Child.Power->DeviceStateBefore, Before[I]);
        EXPECT_EQ(Child.Power->DeviceStateAfter, After[I]);
        EXPECT_EQ(Child.Power->SystemStateBefore,
                  I == 2 ? SystemPowerState::Sleeping3
                         : SystemPowerState::Working);
        EXPECT_EQ(Child.Power->SystemStateAfter, SystemPowerState::Working);
        ASSERT_TRUE(Child.Power->BusReceivedAt100ns);
        ASSERT_TRUE(Child.Power->BusCompletedAt100ns);
        EXPECT_EQ(*Child.Power->BusCompletedAt100ns -
                      *Child.Power->BusReceivedAt100ns,
                  Input.PnpDevices.front()
                      .RequestedDevicePower[I]
                      .BusCompletion.Delay100ns);
        if (I)
          EXPECT_GE(*Child.Power->BusReceivedAt100ns,
                    *Children[I - 1]->Power->BusCompletedAt100ns);
      }
      const auto *Sleep = scenarioRequest(*Result, 4);
      ASSERT_NE(Sleep, nullptr);
      ASSERT_TRUE(Sleep->Power);
      EXPECT_EQ(Sleep->Power->DeviceStateBefore, DevicePowerState::D2);
      EXPECT_EQ(Sleep->Power->DeviceStateAfter, DevicePowerState::D3);
      EXPECT_EQ(Sleep->Power->SystemStateBefore, SystemPowerState::Working);
      EXPECT_EQ(Sleep->Power->SystemStateAfter, SystemPowerState::Sleeping3);
      const auto *Resume = scenarioRequest(*Result, 6);
      ASSERT_NE(Resume, nullptr);
      ASSERT_TRUE(Resume->Power);
      // The S3 parent remains pending through both device transitions. The S0
      // parent completes before its independent delayed D0 child finishes.
      EXPECT_EQ(Resume->Power->DeviceStateBefore, DevicePowerState::D3);
      EXPECT_EQ(Resume->Power->DeviceStateAfter, DevicePowerState::D3);
      EXPECT_EQ(Resume->Power->SystemStateBefore, SystemPowerState::Sleeping3);
      EXPECT_EQ(Resume->Power->SystemStateAfter, SystemPowerState::Working);
      for (const size_t Index : {size_t(5), size_t(7)}) {
        const auto *Snapshot = scenarioRequest(*Result, Index);
        ASSERT_NE(Snapshot, nullptr);
        ASSERT_EQ(Snapshot->Output.size(),
                  KmdfChildWakeSnapshotWords * sizeof(uint32_t));
        EXPECT_EQ(word(*Snapshot, KmdfChildWakeFailures), 0u);
        EXPECT_EQ(word(*Snapshot, KmdfChildWakeD0Entries),
                  Index == 5 ? 2u : 3u);
        EXPECT_EQ(word(*Snapshot, KmdfChildWakeD0Exits), 2u);
        EXPECT_EQ(word(*Snapshot, KmdfChildWakeInD0), Index == 5 ? 0u : 1u);
        EXPECT_EQ(word(*Snapshot, KmdfChildWakeArms), 0u);
      }
    }
#else
  GTEST_SKIP()
      << "NEVERD_KMDF_CHILD_WAKE_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverDeviceD2Genuine, SystemSleepCannotSkipTheExplicitD0Response) {
#ifdef NEVERD_KMDF_CHILD_WAKE_FIXTURE
  for (const auto *Image : images()) {
    SCOPED_TRACE(Image);
    auto Input = systemSleepOptions();
    auto &Responses = Input.PnpDevices.front().RequestedDevicePower;
    Responses.erase(Responses.begin());
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
    EXPECT_NE(Result->Diagnostic.find("requested_device_power"),
              std::string::npos)
        << Result->Diagnostic;
    ASSERT_FALSE(Result->PnpDevices.empty());
    EXPECT_EQ(Result->PnpDevices.front().DevicePower, DevicePowerState::D2);
    EXPECT_EQ(Result->PnpDevices.front().SystemPower,
              SystemPowerState::Working);
    for (const auto &Request : Result->Requests)
      EXPECT_NE(Request.Origin, DriverRequestOrigin::FrameworkPowerPolicy);
    const auto *Sleep = scenarioRequest(*Result, 4);
    ASSERT_NE(Sleep, nullptr);
    EXPECT_FALSE(Sleep->Completed);
  }
#else
  GTEST_SKIP()
      << "NEVERD_KMDF_CHILD_WAKE_FIXTURE requires a genuine WDK fixture";
#endif
}
} // namespace
} // namespace neverd::emulation
