//===- DriverWdmUsbIdleTests.cpp - Genuine USB idle transactions ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "fixtures/driver_wdm_usb_idle_test.h"
#include "gtest/gtest.h"
#include "os/windows/WindowsKernelLayout.h"

#include "neverd/emulation/DriverSession.h"

#include <algorithm>
#include <array>

namespace neverd::emulation {
namespace {
#ifdef NEVERD_WDM_USB_IDLE_FIXTURE
constexpr llvm::StringLiteral DeviceID = "usb-function";
constexpr llvm::StringLiteral PeerID = "usb-peer";
constexpr llvm::StringLiteral ParentID = "usb-composite";

std::vector<const char *> images() {
  return {
      NEVERD_WDM_USB_IDLE_FIXTURE,
#ifdef NEVERD_WDM_USB_IDLE_CFG_FIXTURE
      NEVERD_WDM_USB_IDLE_CFG_FIXTURE,
#endif
  };
}

DriverRequest pnp(DevicePnpRequest Minor) {
  DriverRequest Request;
  Request.Kind = DriverRequestKind::Pnp;
  Request.DeviceID = DeviceID.str();
  Request.Pnp = DriverPnpOperation{Minor, {windows::StatusSuccess, 3}};
  return Request;
}

DriverRequest file(DriverRequestKind Kind) {
  DriverRequest Request;
  Request.Kind = Kind;
  Request.DeviceID = DeviceID.str();
  Request.File = 1;
  return Request;
}

DriverRequest command(uint8_t Command) {
  auto Request = file(DriverRequestKind::DeviceControl);
  Request.ControlCode = UsbIdleCommandIoctl;
  Request.Input = {Command};
  return Request;
}

DriverRequest snapshot(bool Permit = false) {
  auto Request = file(DriverRequestKind::DeviceControl);
  Request.ControlCode = UsbIdleSnapshotIoctl;
  Request.OutputSize = UsbIdleSnapshotWords * sizeof(uint32_t);
  if (Permit)
    Request.PowerPolicyEvents = {
        {0, DeviceID.str(), DriverPowerPolicyAction::UsbIdlePermission}};
  return Request;
}

DriverPowerOperation power(DevicePowerState State, uint64_t Delay = 11) {
  DriverPowerOperation Power;
  Power.State = uint32_t(State);
  Power.BusCompletion = {windows::StatusSuccess, Delay};
  return Power;
}

DriverOptions options(bool CallerStack = false, bool RemoteWake = false) {
  DriverOptions Options;
  Options.ServiceName = std::string("NeverDUsbIdle") +
                        (CallerStack ? char(UsbIdleCallerStackMode) : 'N');
  Options.Unload = true;
  DriverPnpDevice Device;
  Device.ID = DeviceID.str();
  Device.InitialDevicePower = DevicePowerState::D0;
  Device.InitialSystemPower = SystemPowerState::Working;
  Device.UsbIdle =
      DriverUsbIdleConfig{DriverUsbIdleRole::IndependentFunction, RemoteWake};
  Device.WakeCapabilities = DriverWakeCapabilities{RemoteWake, false};
  Options.PnpDevices.push_back(std::move(Device));
  Options.Requests = {pnp(DevicePnpRequest::Start),
                      file(DriverRequestKind::Create)};
  return Options;
}

void finish(DriverOptions &Options) {
  Options.Requests.insert(
      Options.Requests.end(),
      {file(DriverRequestKind::Cleanup), file(DriverRequestKind::Close),
       pnp(DevicePnpRequest::QueryRemove), pnp(DevicePnpRequest::Remove)});
}

DriverRequest onDevice(DriverRequest Request, llvm::StringRef ID,
                       uint32_t File = 1) {
  Request.DeviceID = ID.str();
  if (!Request.Pnp && !Request.Power)
    Request.File = File;
  return Request;
}

DriverOptions twoFunctions(bool Composite) {
  auto Options = options();
  auto Function = Options.PnpDevices.front();
  Options.PnpDevices.clear();
  Options.Requests.clear();
  if (Composite) {
    auto Parent = Function;
    Parent.ID = ParentID.str();
    Parent.UsbIdle->Role = DriverUsbIdleRole::CompositeParent;
    Options.PnpDevices.push_back(Parent);
    Options.Requests.push_back(
        onDevice(pnp(DevicePnpRequest::Start), ParentID));
    Function.ParentID = ParentID.str();
    Function.UsbIdle->Role = DriverUsbIdleRole::CompositeFunction;
  }
  Function.RequestedDevicePower = {power(DevicePowerState::D2),
                                   power(DevicePowerState::D0)};
  Options.PnpDevices.push_back(Function);
  Function.ID = PeerID.str();
  if (!Composite)
    Function.RequestedDevicePower.clear();
  Options.PnpDevices.push_back(Function);
  Options.Requests.insert(
      Options.Requests.end(),
      {pnp(DevicePnpRequest::Start),
       onDevice(pnp(DevicePnpRequest::Start), PeerID),
       file(DriverRequestKind::Create),
       onDevice(file(DriverRequestKind::Create), PeerID, 2)});
  return Options;
}

void finishTwoFunctions(DriverOptions &Options, bool Composite) {
  finish(Options);
  for (auto Request :
       {file(DriverRequestKind::Cleanup), file(DriverRequestKind::Close),
        pnp(DevicePnpRequest::QueryRemove), pnp(DevicePnpRequest::Remove)})
    Options.Requests.push_back(onDevice(std::move(Request), PeerID, 2));
  if (Composite) {
    Options.Requests.push_back(
        onDevice(pnp(DevicePnpRequest::QueryRemove), ParentID));
    Options.Requests.push_back(
        onDevice(pnp(DevicePnpRequest::Remove), ParentID));
  }
}

uint32_t word(const DriverRequestResult &Request, size_t Index) {
  uint32_t Value = 0;
  if (Request.Output.size() >= (Index + 1) * sizeof(Value))
    for (size_t I = 0; I < sizeof(Value); ++I)
      Value |= uint32_t(Request.Output[Index * sizeof(Value) + I]) << (I * 8);
  return Value;
}

const DriverRequestResult *scenario(const DriverResult &Result, size_t Index) {
  size_t Found = 0;
  for (const auto &Request : Result.Requests)
    if (Request.Origin == DriverRequestOrigin::Scenario && Found++ == Index)
      return &Request;
  return nullptr;
}

std::vector<const DriverRequestResult *>
idleRequests(const DriverResult &Result) {
  std::vector<const DriverRequestResult *> Requests;
  for (const auto &Request : Result.Requests)
    if (Request.Origin == DriverRequestOrigin::DriverAllocatedIRP &&
        Request.ControlCode == UsbIdleInternalIoctl)
      Requests.push_back(&Request);
  return Requests;
}

const DriverRequestResult *powerChild(const DriverResult &Result,
                                      size_t ResponseIndex) {
  for (const auto &Request : Result.Requests)
    if (Request.Origin == DriverRequestOrigin::PoRequestPowerIrp &&
        Request.ResponseIndex == ResponseIndex)
      return &Request;
  return nullptr;
}

void clean(const DriverResult &Result) {
  ASSERT_EQ(Result.Stop, DriverStopReason::Returned) << Result.Diagnostic;
  EXPECT_TRUE(Result.UnloadCompleted);
  EXPECT_TRUE(Result.Devices.empty());
  EXPECT_NE(std::find(Result.Messages.begin(), Result.Messages.end(),
                      "WDM USB idle: unload live 0 failures 0\n"),
            Result.Messages.end());
  for (const auto &Request : Result.Requests) {
    EXPECT_TRUE(Request.Completed);
    if (Request.Origin == DriverRequestOrigin::Scenario || Request.Power)
      EXPECT_EQ(Request.IOStatus, windows::StatusSuccess);
  }
}

void expectSnapshot(const DriverResult &Result, size_t Index, bool Active,
                    unsigned Entries, unsigned Returns, unsigned Frees) {
  const auto *Snapshot = scenario(Result, Index);
  ASSERT_NE(Snapshot, nullptr);
  ASSERT_EQ(Snapshot->Output.size(), UsbIdleSnapshotWords * sizeof(uint32_t));
  EXPECT_EQ(word(*Snapshot, UsbIdleFailures), 0u);
  EXPECT_EQ(word(*Snapshot, UsbIdleActive), unsigned(Active));
  EXPECT_EQ(word(*Snapshot, UsbIdleCallbackEntries), Entries);
  EXPECT_EQ(word(*Snapshot, UsbIdleCallbackReturns), Returns);
  EXPECT_EQ(word(*Snapshot, UsbIdleFrees), Frees);
  EXPECT_EQ(word(*Snapshot, UsbIdleCompletions), Frees);
}
#endif

TEST(DriverWdmUsbIdle,
     RetainedWithoutPermissionAndCancelledThroughRealHandler) {
#ifdef NEVERD_WDM_USB_IDLE_FIXTURE
  for (const auto *Image : images()) {
    auto Input = options();
    Input.Requests.insert(Input.Requests.end(),
                          {command(UsbIdleSubmit), snapshot(),
                           command(UsbIdleCancel), snapshot()});
    finish(Input);
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    ASSERT_FALSE(HasFatalFailure());
    expectSnapshot(*Result, 3, true, 0, 0, 0);
    expectSnapshot(*Result, 5, false, 0, 0, 1);
    const auto Idle = idleRequests(*Result);
    ASSERT_EQ(Idle.size(), 1u);
    EXPECT_EQ(Idle.front()->IOStatus, UsbIdleCancelledStatus);
    EXPECT_EQ(Idle.front()->DispatchStatus, windows::StatusPending);
    EXPECT_TRUE(Idle.front()->CancelRequestedAt100ns);
    ASSERT_TRUE(Idle.front()->UsbIdle);
    EXPECT_FALSE(Idle.front()->UsbIdle->CallbackEnteredAt100ns);
    EXPECT_FALSE(Idle.front()->UsbIdle->D2IRP);
    EXPECT_EQ(Idle.front()->UsbIdle->CompletionCause,
              DriverUsbIdleCompletionCause::Cancel);
    EXPECT_EQ(powerChild(*Result, 0), nullptr);
    const auto *Snapshot = scenario(*Result, 5);
    ASSERT_NE(Snapshot, nullptr);
    EXPECT_EQ(word(*Snapshot, UsbIdleCancelTrue), 1u);
    EXPECT_EQ(word(*Snapshot, UsbIdleDevicePower),
              uint32_t(DevicePowerState::D0));
  }
#else
  GTEST_SKIP() << "NEVERD_WDM_USB_IDLE_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverWdmUsbIdle, PermissionWaitsForD2AndD0ReceiptCompletesTheIdlePacket) {
#ifdef NEVERD_WDM_USB_IDLE_FIXTURE
  for (const auto *Image : images())
    for (uint64_t Base : {uint64_t(0), uint64_t(0x190000000)})
      for (bool CallerStack : {false, true})
        for (uint8_t Mode : {UsbIdleSubmit, UsbIdleSubmitThroughStack}) {
          SCOPED_TRACE(Image);
          SCOPED_TRACE(Base);
          SCOPED_TRACE(CallerStack);
          SCOPED_TRACE(char(Mode));
          auto Input = options(CallerStack);
          Input.LoadAddress = Base;
          Input.PnpDevices.front().RequestedDevicePower = {
              power(DevicePowerState::D2, CallerStack ? 0 : 11),
              power(DevicePowerState::D0, CallerStack ? 0 : 13)};
          Input.Requests.insert(Input.Requests.end(),
                                {command(Mode), snapshot(true), snapshot(),
                                 command(UsbIdleRequestD0), snapshot()});
          finish(Input);
          auto Result = emulateDriver(Image, Input);
          ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
          clean(*Result);
          ASSERT_FALSE(HasFatalFailure());
          expectSnapshot(*Result, 4, true, 1, 1, 0);
          expectSnapshot(*Result, 6, false, 1, 1, 1);
          const auto *Snapshot = scenario(*Result, 6);
          ASSERT_NE(Snapshot, nullptr);
          EXPECT_EQ(word(*Snapshot, UsbIdleD2Completions), 1u);
          EXPECT_EQ(word(*Snapshot, UsbIdleD0Completions), 1u);
          EXPECT_EQ(word(*Snapshot, UsbIdleCompletedBeforeD0Acknowledgement),
                    1u);
          EXPECT_EQ(word(*Snapshot, UsbIdleLastCompletionDeviceIsSelf),
                    unsigned(CallerStack));
          EXPECT_EQ(word(*Snapshot, UsbIdleInternalDispatches),
                    unsigned(Mode == UsbIdleSubmitThroughStack));
          EXPECT_LT(word(*Snapshot, UsbIdleEntryOrder),
                    word(*Snapshot, UsbIdleD2CompletionOrder));
          EXPECT_LT(word(*Snapshot, UsbIdleD2CompletionOrder),
                    word(*Snapshot, UsbIdleReturnOrder));
          EXPECT_LT(word(*Snapshot, UsbIdleReturnOrder),
                    word(*Snapshot, UsbIdleD0DispatchOrder));
          EXPECT_LT(word(*Snapshot, UsbIdleD0DispatchOrder),
                    word(*Snapshot, UsbIdleCompletionOrder));
          EXPECT_LT(word(*Snapshot, UsbIdleCompletionOrder),
                    word(*Snapshot, UsbIdleD0CompletionOrder));
          const auto Idle = idleRequests(*Result);
          ASSERT_EQ(Idle.size(), 1u);
          ASSERT_TRUE(Idle.front()->UsbIdle);
          const auto &USB = *Idle.front()->UsbIdle;
          EXPECT_EQ(Idle.front()->IOStatus, windows::StatusSuccess);
          EXPECT_EQ(USB.CompletionCause,
                    DriverUsbIdleCompletionCause::DeviceD0);
          const auto *D2 = powerChild(*Result, 0);
          const auto *D0 = powerChild(*Result, 1);
          ASSERT_NE(D2, nullptr);
          ASSERT_NE(D0, nullptr);
          ASSERT_TRUE(D2->Power);
          ASSERT_TRUE(D0->Power);
          EXPECT_EQ(USB.D2IRP, D2->IRP);
          EXPECT_EQ(USB.D2Status, windows::StatusSuccess);
          EXPECT_EQ(USB.D2CompletedAt100ns, D2->Power->BusCompletedAt100ns);
          EXPECT_EQ(D2->Power->DeviceStateAfter, DevicePowerState::D2);
          EXPECT_EQ(D0->Power->DeviceStateBefore, DevicePowerState::D2);
          EXPECT_EQ(D0->Power->DeviceStateAfter, DevicePowerState::D0);
          EXPECT_EQ(USB.CompletedAt100ns, D0->Power->BusReceivedAt100ns);
          if (!CallerStack) {
            ASSERT_TRUE(USB.CompletedAt100ns);
            ASSERT_TRUE(D0->Power->BusCompletedAt100ns);
            EXPECT_LT(*USB.CompletedAt100ns, *D0->Power->BusCompletedAt100ns);
          }
        }
#else
  GTEST_SKIP() << "NEVERD_WDM_USB_IDLE_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverWdmUsbIdle, InCallbackCancellationRetainsStorageUntilD2AndReturn) {
#ifdef NEVERD_WDM_USB_IDLE_FIXTURE
  for (const auto *Image : images()) {
    auto Input = options();
    Input.PnpDevices.front().RequestedDevicePower = {
        power(DevicePowerState::D2), power(DevicePowerState::D0)};
    Input.Requests.insert(Input.Requests.end(),
                          {command(UsbIdleSubmitWithCancelWorker),
                           snapshot(true), snapshot(),
                           command(UsbIdleRequestD0), snapshot()});
    finish(Input);
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    ASSERT_FALSE(HasFatalFailure());
    expectSnapshot(*Result, 4, false, 1, 1, 1);
    const auto *Snapshot = scenario(*Result, 4);
    ASSERT_NE(Snapshot, nullptr);
    EXPECT_EQ(word(*Snapshot, UsbIdleCancelWorkers), 1u);
    EXPECT_EQ(word(*Snapshot, UsbIdleCancelTrue), 1u);
    EXPECT_EQ(word(*Snapshot, UsbIdleRetainedAfterInCallbackCancel), 1u);
    EXPECT_EQ(word(*Snapshot, UsbIdleD0Completions), 0u);
    EXPECT_LT(word(*Snapshot, UsbIdleD2CompletionOrder),
              word(*Snapshot, UsbIdleReturnOrder));
    EXPECT_LT(word(*Snapshot, UsbIdleReturnOrder),
              word(*Snapshot, UsbIdleCompletionOrder));
    const auto Idle = idleRequests(*Result);
    ASSERT_EQ(Idle.size(), 1u);
    EXPECT_EQ(Idle.front()->IOStatus, UsbIdleCancelledStatus);
    ASSERT_TRUE(Idle.front()->UsbIdle);
    const auto &USB = *Idle.front()->UsbIdle;
    ASSERT_TRUE(USB.CallbackReturnedAt100ns);
    ASSERT_TRUE(USB.CompletionClaimedAt100ns);
    ASSERT_TRUE(USB.CompletedAt100ns);
    EXPECT_LT(*USB.CompletionClaimedAt100ns, *USB.CallbackReturnedAt100ns);
    EXPECT_EQ(*USB.CompletedAt100ns, *USB.CallbackReturnedAt100ns);
    EXPECT_EQ(USB.CompletionCause, DriverUsbIdleCompletionCause::Cancel);
  }
#else
  GTEST_SKIP() << "NEVERD_WDM_USB_IDLE_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverWdmUsbIdle, RemoteWakeUsesIndependentWaitWakeAndD0Packets) {
#ifdef NEVERD_WDM_USB_IDLE_FIXTURE
  for (const auto *Image : images()) {
    auto Input = options(false, true);
    Input.PnpDevices.front().RequestedDevicePower = {
        power(DevicePowerState::D2), power(DevicePowerState::D0)};
    auto Wake = snapshot();
    Wake.PowerPolicyEvents = {
        {0, DeviceID.str(), DriverPowerPolicyAction::Wake}};
    Input.Requests.insert(Input.Requests.end(),
                          {command(UsbIdleSubmitWithRemoteWake), snapshot(true),
                           snapshot(), Wake, snapshot()});
    finish(Input);
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    ASSERT_FALSE(HasFatalFailure());
    expectSnapshot(*Result, 4, true, 1, 1, 0);
    expectSnapshot(*Result, 6, false, 1, 1, 1);
    const auto *Snapshot = scenario(*Result, 6);
    ASSERT_NE(Snapshot, nullptr);
    EXPECT_EQ(word(*Snapshot, UsbIdleWakeSubmissions), 1u);
    EXPECT_EQ(word(*Snapshot, UsbIdleWakeCallbacks), 1u);
    EXPECT_EQ(word(*Snapshot, UsbIdleWakeActive), 0u);
    EXPECT_EQ(word(*Snapshot, UsbIdleCompletedBeforeD0Acknowledgement), 1u);
    const auto Found =
        std::find_if(Result->Requests.begin(), Result->Requests.end(),
                     [](const auto &Request) {
                       return Request.Power && Request.Power->Minor ==
                                                   DevicePowerRequest::WaitWake;
                     });
    ASSERT_NE(Found, Result->Requests.end());
    EXPECT_EQ(Found->Origin, DriverRequestOrigin::PoRequestPowerIrp);
    EXPECT_EQ(Found->Power->DeviceStateAfter, DevicePowerState::D2);
    EXPECT_EQ(Found->Power->WakeSourceDeviceID, DeviceID.str());
    EXPECT_FALSE(Found->ResponseIndex);
    ASSERT_NE(powerChild(*Result, 1), nullptr);
    EXPECT_NE(Found->IRP, powerChild(*Result, 1)->IRP);
  }
#else
  GTEST_SKIP() << "NEVERD_WDM_USB_IDLE_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverWdmUsbIdle, DuplicateDoesNotReplaceOriginalAndD3RetiresIt) {
#ifdef NEVERD_WDM_USB_IDLE_FIXTURE
  for (const auto *Image : images()) {
    auto Input = options();
    Input.PnpDevices.front().RequestedDevicePower = {
        power(DevicePowerState::D3), power(DevicePowerState::D0)};
    Input.Requests.insert(
        Input.Requests.end(),
        {command(UsbIdleSubmit), command(UsbIdleSubmitDuplicate), snapshot(),
         command(UsbIdleRequestD3), snapshot(), command(UsbIdleRequestD0)});
    finish(Input);
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    ASSERT_FALSE(HasFatalFailure());
    expectSnapshot(*Result, 4, true, 0, 0, 1);
    expectSnapshot(*Result, 6, false, 0, 0, 2);
    const auto Idle = idleRequests(*Result);
    ASSERT_EQ(Idle.size(), 2u);
    EXPECT_NE(Idle[0]->IRP, Idle[1]->IRP);
    EXPECT_EQ(Idle[0]->IOStatus, UsbIdlePowerInvalidStatus);
    EXPECT_EQ(Idle[1]->IOStatus, UsbIdleBusyStatus);
    ASSERT_TRUE(Idle[0]->UsbIdle);
    EXPECT_EQ(Idle[0]->UsbIdle->CompletionCause,
              DriverUsbIdleCompletionCause::DeviceD3);
    EXPECT_FALSE(Idle[0]->UsbIdle->CallbackEnteredAt100ns);
  }
#else
  GTEST_SKIP() << "NEVERD_WDM_USB_IDLE_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverWdmUsbIdle, CapturedPermissionCannotAuthorizeAReplacementPacket) {
#ifdef NEVERD_WDM_USB_IDLE_FIXTURE
  for (const auto *Image : images()) {
    auto Input = options();
    Input.Requests.push_back(command(UsbIdleSubmit));
    auto Replace = command(UsbIdleCancelRearm);
    Replace.PowerPolicyEvents = {
        {17, DeviceID.str(), DriverPowerPolicyAction::UsbIdlePermission}};
    Input.Requests.push_back(std::move(Replace));
    Input.Unload = false;
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
    const auto Idle = idleRequests(*Result);
    ASSERT_EQ(Idle.size(), 2u);
    EXPECT_TRUE(Idle[0]->Completed);
    EXPECT_EQ(Idle[0]->IOStatus, UsbIdleCancelledStatus);
    EXPECT_FALSE(Idle[1]->Completed);
    ASSERT_TRUE(Idle[1]->UsbIdle);
    EXPECT_FALSE(Idle[1]->UsbIdle->CallbackEnteredAt100ns);
    EXPECT_EQ(powerChild(*Result, 0), nullptr);
    ASSERT_EQ(Result->PowerPolicyEvents.size(), 1u);
    EXPECT_FALSE(Result->PowerPolicyEvents.front().OccurredAt100ns);
  }
#else
  GTEST_SKIP() << "NEVERD_WDM_USB_IDLE_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverWdmUsbIdle, StopRestartRetiresTheOriginalRegistration) {
#ifdef NEVERD_WDM_USB_IDLE_FIXTURE
  for (const auto *Image : images()) {
    auto Input = options();
    Input.Requests.insert(
        Input.Requests.end(),
        {command(UsbIdleSubmit), file(DriverRequestKind::Cleanup),
         file(DriverRequestKind::Close), pnp(DevicePnpRequest::QueryStop),
         pnp(DevicePnpRequest::Stop), pnp(DevicePnpRequest::Start),
         file(DriverRequestKind::Create), command(UsbIdleSubmit), snapshot()});
    finish(Input);
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    ASSERT_FALSE(HasFatalFailure());
    expectSnapshot(*Result, 10, true, 0, 0, 1);
    const auto Idle = idleRequests(*Result);
    ASSERT_EQ(Idle.size(), 2u);
    ASSERT_TRUE(Idle[0]->UsbIdle);
    ASSERT_TRUE(Idle[1]->UsbIdle);
    EXPECT_NE(Idle[0]->IRP, Idle[1]->IRP);
    EXPECT_NE(Idle[0]->UsbIdle->StartEpoch, Idle[1]->UsbIdle->StartEpoch);
    EXPECT_EQ(Idle[0]->IOStatus, UsbIdleCancelledStatus);
    EXPECT_EQ(Idle[1]->IOStatus, UsbIdleCancelledStatus);
  }
#else
  GTEST_SKIP() << "NEVERD_WDM_USB_IDLE_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverWdmUsbIdle, IndependentPermissionDoesNotSuspendAnotherFunction) {
#ifdef NEVERD_WDM_USB_IDLE_FIXTURE
  for (const auto *Image : images()) {
    auto Input = twoFunctions(false);
    Input.Requests.insert(Input.Requests.end(),
                          {command(UsbIdleSubmit),
                           onDevice(command(UsbIdleSubmit), PeerID, 2),
                           snapshot(true)});
    const size_t LocalIndex = Input.Requests.size();
    Input.Requests.push_back(snapshot());
    Input.Requests.push_back(onDevice(snapshot(), PeerID, 2));
    Input.Requests.push_back(command(UsbIdleRequestD0));
    Input.Requests.push_back(onDevice(command(UsbIdleCancel), PeerID, 2));
    finishTwoFunctions(Input, false);
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result);
    ASSERT_FALSE(HasFatalFailure());
    expectSnapshot(*Result, LocalIndex, true, 1, 1, 0);
    expectSnapshot(*Result, LocalIndex + 1, true, 0, 0, 0);
    ASSERT_EQ(Result->PowerPolicyEvents.size(), 1u);
    const auto &Members = Result->PowerPolicyEvents.front().UsbIdleMembers;
    ASSERT_EQ(Members.size(), 1u);
    EXPECT_EQ(Members.front().DeviceID, DeviceID.str());
    for (const auto &Request : Result->Requests)
      if (Request.Power)
        EXPECT_EQ(Request.DeviceID, DeviceID.str());
  }
#else
  GTEST_SKIP() << "NEVERD_WDM_USB_IDLE_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverWdmUsbIdle, CompositePermissionRequiresAndCapturesEveryFunction) {
#ifdef NEVERD_WDM_USB_IDLE_FIXTURE
  for (const auto *Image : images())
    for (bool AllRegistered : {false, true}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(AllRegistered);
      auto Input = twoFunctions(true);
      Input.Requests.push_back(command(UsbIdleSubmit));
      if (AllRegistered)
        Input.Requests.push_back(onDevice(command(UsbIdleSubmit), PeerID, 2));
      auto Permit = snapshot(true);
      Permit.PowerPolicyEvents.front().DeviceID = ParentID.str();
      Input.Requests.push_back(std::move(Permit));
      const size_t LocalIndex = Input.Requests.size();
      if (AllRegistered) {
        Input.Requests.push_back(snapshot());
        Input.Requests.push_back(onDevice(snapshot(), PeerID, 2));
        Input.Requests.push_back(command(UsbIdleRequestD0));
        Input.Requests.push_back(
            onDevice(command(UsbIdleRequestD0), PeerID, 2));
        finishTwoFunctions(Input, true);
      } else {
        Input.Unload = false;
      }
      auto Result = emulateDriver(Image, Input);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      if (!AllRegistered) {
        EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
        const auto Idle = idleRequests(*Result);
        ASSERT_EQ(Idle.size(), 1u);
        ASSERT_TRUE(Idle.front()->UsbIdle);
        EXPECT_FALSE(Idle.front()->UsbIdle->CallbackEnteredAt100ns);
        EXPECT_EQ(powerChild(*Result, 0), nullptr);
        continue;
      }
      clean(*Result);
      ASSERT_FALSE(HasFatalFailure());
      expectSnapshot(*Result, LocalIndex, true, 1, 1, 0);
      expectSnapshot(*Result, LocalIndex + 1, true, 1, 1, 0);
      ASSERT_EQ(Result->PowerPolicyEvents.size(), 1u);
      const auto &Members = Result->PowerPolicyEvents.front().UsbIdleMembers;
      ASSERT_EQ(Members.size(), 2u);
      const auto Idle = idleRequests(*Result);
      ASSERT_EQ(Idle.size(), 2u);
      for (const auto &Member : Members) {
        const auto Found =
            std::find_if(Idle.begin(), Idle.end(), [&](const auto *Request) {
              return Request->DeviceID == Member.DeviceID;
            });
        ASSERT_NE(Found, Idle.end());
        EXPECT_EQ(Member.IRP, (*Found)->IRP);
        ASSERT_TRUE((*Found)->UsbIdle);
        EXPECT_EQ(Member.StartEpoch, (*Found)->UsbIdle->StartEpoch);
      }
    }
#else
  GTEST_SKIP() << "NEVERD_WDM_USB_IDLE_FIXTURE requires a genuine WDK fixture";
#endif
}
} // namespace
} // namespace neverd::emulation
