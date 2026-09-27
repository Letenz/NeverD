//===- KernelFrameworkUsbPoFxBridgeTests.cpp - USB and PoFx ownership ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "KernelFrameworkUsbIdleBridgeTestSupport.h"

namespace neverd::emulation {
namespace {
using namespace framework_usb_test;

class KernelFrameworkUsbPoFxBridge : public KernelFrameworkUsbIdleBridge {
protected:
  static constexpr uint64_t Settings = Scratch + 0x600;
  static constexpr uint64_t Component = Scratch + 0x700;
  static constexpr uint64_t IdleStates = Scratch + 0x800;
  static constexpr uint64_t Context = Scratch + 0x900;
  static constexpr uint64_t PostPC = AddPC + 0x300;
  static constexpr uint64_t ActivePC = AddPC + 0x400;
  static constexpr uint64_t IdlePC = AddPC + 0x500;
  static constexpr uint64_t StatePC = AddPC + 0x600;
  static constexpr uint64_t EntryPC = AddPC + 0x700;
  uint64_t Handle = 0;

  void initializeManaged(bool Wake = false) {
    IdleTimeoutType = policy::SystemManagedTimeout;
    initialize(true, Wake);
  }
  void configurePowerFramework() override {
    put(Settings, PoFxSettingsSize, 4);
    put(Settings + PoFxSettingsPostRegister, PostPC);
    put(Settings + PoFxSettingsComponent, Component);
    put(Settings + PoFxSettingsActiveCondition, ActivePC);
    put(Settings + PoFxSettingsIdleCondition, IdlePC);
    put(Settings + PoFxSettingsIdleState, StatePC);
    put(Settings + PoFxSettingsContext, Context);
    put(Settings + PoFxSettingsDirected, policy::False, 4);
    put(Component + pofx::ComponentIdleStateCount, 2, 4);
    put(Component + pofx::ComponentDeepestWakeableState, 1, 4);
    put(Component + pofx::ComponentIdleStates, IdleStates);
    put(IdleStates + pofx::IdleStateNominalPower, 100, 4);
    put(IdleStates + pofx::IdleStateSize + pofx::IdleStateNominalPower, 25, 4);
    EXPECT_EQ(invoke(api::WdfDeviceWdmAssignPowerFrameworkSettings,
                     {Globals, Device, Settings}),
              StatusSuccess);
  }
  void finishStartRequest(uint64_t IRP) override {
    auto Call = Model->takeGuestCall();
    ASSERT_TRUE(Call);
    if (D0EntryCallback) {
      ASSERT_EQ(Call->PC, D0EntryCallback);
      success(Model->beginGuestCall(Call->Token));
      EXPECT_FALSE(take(Model->finishGuestCall(Call->Token, StatusSuccess)));
      Call = Model->takeGuestCall();
      ASSERT_TRUE(Call);
    }
    ASSERT_EQ(Call->PC, PostPC);
    ASSERT_EQ(Call->Arguments.size(), 2u);
    EXPECT_EQ(Call->Arguments.front(), Device);
    Handle = Call->Arguments.back();
    ASSERT_NE(Handle, 0u);
    success(Model->beginGuestCall(Call->Token));
    take(Model->finishGuestCall(Call->Token, StatusSuccess));
    success(Model->finalizeRequest(IRP));
  }
  KernelScheduler::Invocation scheduled(uint64_t PC, bool Advance = false,
                                        std::optional<uint32_t> State = {}) {
    auto Next = take(Model->nextScheduled(Advance));
    EXPECT_TRUE(Next);
    if (!Next)
      return {};
    EXPECT_EQ(Next->PC, PC);
    EXPECT_EQ(Next->IRQL, scheduler::PassiveLevel);
    EXPECT_EQ(Next->Kind, KernelScheduler::CallbackKind::PoFx);
    std::vector<uint64_t> Arguments{Context, 0};
    if (State)
      Arguments.push_back(*State);
    EXPECT_EQ(Next->Arguments, Arguments);
    Model->enterExecution(profile::CallbackStackBase, Next->ID,
                          Model->scheduledGuestCall(Next->ID));
    return *Next;
  }
  void finish(const KernelScheduler::Invocation &Call) {
    ASSERT_NE(Call.ID, 0u);
    EXPECT_FALSE(take(Model->continueScheduled(Call.ID, 0)));
    success(Model->finishScheduled(Call.ID));
    Model->enterForeground();
    Model->enterExecution(profile::StackBase);
  }
  void idleComponent(bool F1 = false) {
    event(DriverPowerPolicyAction::Idle);
    const auto Idle = scheduled(IdlePC);
    ASSERT_FALSE(HasFailure());
    take(Model->call("PoFxCompleteIdleCondition", {Handle, 0}));
    finish(Idle);
    if (!F1)
      return;
    DriverRequest Request;
    Request.Kind = DriverRequestKind::Create;
    Request.DeviceID = "port";
    Request.File = NextFile++;
    Request.PowerPolicyEvents.push_back(
        {0, "port", DriverPowerPolicyAction::ComponentIdleState, 0, 1});
    const auto Call = take(Model->beginRequest(Request));
    success(Model->finalizeRequest(Call.IRP));
    const auto State = scheduled(StatePC, false, 1);
    ASSERT_FALSE(HasFailure());
    take(Model->call("PoFxCompleteIdleState", {Handle, 0}));
    finish(State);
  }
  uint64_t grant() {
    event(DriverPowerPolicyAction::PowerNotRequired);
    EXPECT_FALSE(take(Model->nextScheduled(false)));
    const auto Found = std::find_if(
        Result.Requests.begin(), Result.Requests.end(), [](const auto &R) {
          return R.Origin == DriverRequestOrigin::FrameworkUsbIdle;
        });
    EXPECT_NE(Found, Result.Requests.end());
    return Found == Result.Requests.end() ? 0 : Found->IRP;
  }
  KernelModel::Wait stopAndWait() {
    EXPECT_EQ(invoke(api::WdfDeviceStopIdleNoTrack, {Globals, Device, 1}),
              StatusPending);
    auto Wait = Model->takeWait();
    EXPECT_TRUE(Wait);
    return Wait.value_or(KernelModel::Wait{});
  }
};

TEST_F(KernelFrameworkUsbPoFxBridge,
       NotRequiredAcknowledgesRetainedPacketWhileStillInD0) {
  initializeManaged();
  ASSERT_FALSE(HasFailure());
  idleComponent();
  ASSERT_FALSE(HasFailure());
  EXPECT_FALSE(take(Model->nextScheduled(true)));
  EXPECT_EQ(powerCount(), 0u);
  EXPECT_EQ(std::count_if(Result.Requests.begin(), Result.Requests.end(),
                          [](const auto &R) {
                            return R.Origin ==
                                   DriverRequestOrigin::FrameworkUsbIdle;
                          }),
            0u);
  const auto IRP = grant();
  ASSERT_NE(IRP, 0u);
  ASSERT_FALSE(HasFailure());
  ASSERT_TRUE(observation(IRP).UsbIdle);
  EXPECT_TRUE(observation(IRP).UsbIdle->BusReceivedAt100ns);
  EXPECT_FALSE(observation(IRP).UsbIdle->CallbackEnteredAt100ns);
  EXPECT_FALSE(observation(IRP).UsbIdle->D2IRP);
  EXPECT_TRUE(Model->requestPending(IRP));
  EXPECT_EQ(powerCount(), 0u);
  const auto Wait = stopAndWait();
  ASSERT_FALSE(HasFailure());
  EXPECT_TRUE(observation(IRP).Completed);
  EXPECT_EQ(observation(IRP).IOStatus, StatusCancelled);
  EXPECT_FALSE(take(Model->pollWait(Wait)));
  // Required cannot be emitted while NotRequired is unacknowledged. Reaching
  // the actual Active callback proves that both framework power responses
  // retired while the device remained in D0, without a fabricated D0 IRP.
  const auto Active = scheduled(ActivePC);
  ASSERT_FALSE(HasFailure());
  EXPECT_FALSE(take(Model->pollWait(Wait)));
  finish(Active);
  EXPECT_EQ(take(Model->pollWait(Wait)), StatusSuccess);
  EXPECT_EQ(powerCount(), 0u);
}

TEST_F(KernelFrameworkUsbPoFxBridge,
       StopBeforePermissionWaitsForF0AndActiveWithoutDevicePowerRequest) {
  initializeManaged();
  ASSERT_FALSE(HasFailure());
  idleComponent(true);
  const auto IRP = grant();
  ASSERT_NE(IRP, 0u);
  ASSERT_FALSE(HasFailure());
  const auto Wait = stopAndWait();
  const auto F0 = scheduled(StatePC, false, 0);
  ASSERT_FALSE(HasFailure());
  EXPECT_TRUE(observation(IRP).Completed);
  EXPECT_FALSE(take(Model->pollWait(Wait)));
  take(Model->call("PoFxCompleteIdleState", {Handle, 0}));
  EXPECT_FALSE(take(Model->pollWait(Wait)));
  finish(F0);
  const auto Active = scheduled(ActivePC);
  EXPECT_FALSE(take(Model->pollWait(Wait)));
  finish(Active);
  EXPECT_EQ(take(Model->pollWait(Wait)), StatusSuccess);
  EXPECT_EQ(powerCount(), 0u);
}

TEST_F(KernelFrameworkUsbPoFxBridge,
       EnteredD2CompletesBeforeRequiredD0AndComponentActivation) {
  initializeManaged();
  ASSERT_FALSE(HasFailure());
  idleComponent(true);
  const auto IRP = grant();
  ASSERT_NE(IRP, 0u);
  ASSERT_FALSE(HasFailure());
  event(DriverPowerPolicyAction::UsbIdlePermission);
  EXPECT_FALSE(take(Model->nextScheduled(false)));
  ASSERT_TRUE(observation(IRP).UsbIdle->D2IRP);
  const auto D2 = *observation(IRP).UsbIdle->D2IRP;
  EXPECT_FALSE(observation(D2).Completed);
  const auto Wait = stopAndWait();
  EXPECT_FALSE(take(Model->nextScheduled(false)));
  EXPECT_FALSE(take(Model->pollWait(Wait)));
  EXPECT_EQ(powerCount(), 1u);
  EXPECT_FALSE(observation(IRP).Completed);
  EXPECT_FALSE(take(Model->nextScheduled(true)));
  ASSERT_FALSE(HasFailure());
  EXPECT_TRUE(observation(D2).Completed);
  EXPECT_TRUE(observation(IRP).Completed);
  EXPECT_EQ(powerCount(), 2u);
  const auto D0 = Result.Requests.back().IRP;
  ASSERT_TRUE(observation(D0).Power);
  EXPECT_EQ(observation(D0).Power->State, uint32_t(DevicePowerState::D0));
  EXPECT_FALSE(observation(D0).Completed);
  EXPECT_FALSE(take(Model->pollWait(Wait)));
  const auto F0 = scheduled(StatePC, true, 0);
  ASSERT_FALSE(HasFailure());
  EXPECT_TRUE(observation(D0).Completed);
  EXPECT_FALSE(take(Model->pollWait(Wait)));
  take(Model->call("PoFxCompleteIdleState", {Handle, 0}));
  finish(F0);
  const auto Active = scheduled(ActivePC);
  EXPECT_FALSE(take(Model->pollWait(Wait)));
  finish(Active);
  EXPECT_EQ(take(Model->pollWait(Wait)), StatusSuccess);
  EXPECT_EQ(observation(D0).Power->DeviceStateAfter, DevicePowerState::D0);
}

TEST_F(KernelFrameworkUsbPoFxBridge,
       RealAllocationFailureDisarmsAndRestoresComponentActivity) {
  initializeManaged(true);
  ASSERT_FALSE(HasFailure());
  idleComponent(true);
  const auto IRP = grant();
  ASSERT_NE(IRP, 0u);
  ASSERT_FALSE(HasFailure());
  event(DriverPowerPolicyAction::UsbIdlePermission);
  auto Arm = take(Model->nextScheduled(false));
  ASSERT_TRUE(Arm);
  ASSERT_EQ(Arm->PC, ArmPC);
  const auto Token = Model->scheduledGuestCall(Arm->ID);
  success(Model->beginGuestCall(Token));
  Model->enterExecution(profile::CallbackStackBase, Arm->ID, Token);
  constexpr uint32_t Tag = 0x78665550;
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
  auto Disarm = take(Model->continueScheduled(Arm->ID, StatusSuccess));
  ASSERT_TRUE(Disarm);
  ASSERT_EQ(Disarm->PC, DisarmPC);
  success(Model->beginGuestCall(Disarm->Token));
  Model->enterExecution(profile::CallbackStackBase, Arm->ID, Disarm->Token);
  EXPECT_FALSE(take(Model->continueScheduled(Arm->ID, 0)));
  success(Model->finishScheduled(Arm->ID));
  Model->enterForeground();
  Model->enterExecution(profile::StackBase);
  EXPECT_TRUE(observation(IRP).Completed);
  EXPECT_EQ(observation(IRP).IOStatus, StatusCancelled);
  EXPECT_FALSE(observation(IRP).UsbIdle->D2IRP);
  EXPECT_EQ(powerCount(), 0u);
  const auto F0 = scheduled(StatePC, false, 0);
  ASSERT_FALSE(HasFailure());
  take(Model->call("PoFxCompleteIdleState", {Handle, 0}));
  finish(F0);
  const auto Active = scheduled(ActivePC);
  finish(Active);
  EXPECT_EQ(invoke(api::WdfDeviceStopIdleNoTrack, {Globals, Device, 1}),
            StatusSuccess);
  EXPECT_FALSE(Model->takeWait());
  EXPECT_EQ(powerCount(), 0u);
}

TEST_F(KernelFrameworkUsbPoFxBridge,
       WakeRestoresComponentWithoutReadAndPreservesD0CompletionCause) {
  ObserveWakeCallbacks = false;
  initializeManaged(true);
  ASSERT_FALSE(HasFailure());
  idleComponent(true);
  const auto IRP = grant();
  ASSERT_NE(IRP, 0u);
  ASSERT_FALSE(HasFailure());
  event(DriverPowerPolicyAction::UsbIdlePermission);
  EXPECT_FALSE(take(Model->nextScheduled(false)));
  EXPECT_FALSE(take(Model->nextScheduled(true)));
  ASSERT_FALSE(HasFailure());
  ASSERT_TRUE(observation(IRP).UsbIdle->D2IRP);
  EXPECT_TRUE(observation(*observation(IRP).UsbIdle->D2IRP).Completed);
  event(DriverPowerPolicyAction::Wake);
  EXPECT_FALSE(take(Model->nextScheduled(false)));
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(observation(IRP).UsbIdle->CompletionCause,
            UsbIdleCompletionCause::DeviceD0);
  const auto D0 = Result.Requests.back().IRP;
  ASSERT_TRUE(observation(D0).Power);
  EXPECT_FALSE(observation(D0).Completed);
  const auto F0 = scheduled(StatePC, true, 0);
  ASSERT_FALSE(HasFailure());
  EXPECT_TRUE(observation(D0).Completed);
  take(Model->call("PoFxCompleteIdleState", {Handle, 0}));
  finish(F0);
  const auto Active = scheduled(ActivePC);
  finish(Active);
  EXPECT_EQ(invoke(api::WdfDeviceStopIdleNoTrack, {Globals, Device, 0}),
            StatusSuccess);
  EXPECT_EQ(observation(IRP).UsbIdle->CompletionCause,
            UsbIdleCompletionCause::DeviceD0);
  EXPECT_EQ(powerCount(), 2u);
}

TEST_F(KernelFrameworkUsbPoFxBridge,
       RemovalPowerUpFailureAcknowledgesRequiredWithoutReleasingIdleWait) {
  D0EntryCallback = EntryPC;
  initializeManaged();
  ASSERT_FALSE(HasFailure());
  idleComponent();
  const auto IRP = grant();
  ASSERT_NE(IRP, 0u);
  ASSERT_FALSE(HasFailure());
  event(DriverPowerPolicyAction::UsbIdlePermission);
  EXPECT_FALSE(take(Model->nextScheduled(false)));
  EXPECT_FALSE(take(Model->nextScheduled(true)));
  ASSERT_FALSE(HasFailure());
  ASSERT_TRUE(observation(IRP).UsbIdle->D2IRP);
  EXPECT_TRUE(observation(*observation(IRP).UsbIdle->D2IRP).Completed);
  DriverRequest QueryRemove;
  QueryRemove.Kind = DriverRequestKind::Pnp;
  QueryRemove.DeviceID = "port";
  QueryRemove.Pnp =
      DriverPnpOperation{DevicePnpRequest::QueryRemove, {StatusSuccess, 0}};
  const auto Query = take(Model->beginRequest(QueryRemove));
  ASSERT_FALSE(HasFailure());
  EXPECT_FALSE(Model->takeGuestCall());
  success(Model->finalizeRequest(Query.IRP));
  ASSERT_TRUE(observation(Query.IRP).Pnp);
  EXPECT_EQ(observation(Query.IRP).Pnp->StateAfter,
            DevicePnpState::RemovePending);
  const auto Wait = stopAndWait();
  EXPECT_FALSE(take(Model->nextScheduled(false)));
  const auto D0 = Result.Requests.back().IRP;
  ASSERT_TRUE(observation(D0).Power);
  EXPECT_EQ(observation(D0).Power->State, uint32_t(DevicePowerState::D0));
  EXPECT_FALSE(take(Model->nextScheduled(true)));
  const auto Entry = Model->takeGuestCall();
  ASSERT_TRUE(Entry);
  ASSERT_EQ(Entry->PC, EntryPC);
  EXPECT_EQ(Entry->Arguments,
            (std::vector<uint64_t>{Device, uint64_t(DevicePowerState::D2)}));
  success(Model->beginGuestCall(Entry->Token));
  take(Model->finishGuestCall(Entry->Token, StatusUnsuccessful));
  ASSERT_FALSE(HasFailure());
  EXPECT_FALSE(take(Model->nextScheduled(false)));
  EXPECT_FALSE(Model->takeGuestCall());
  EXPECT_TRUE(observation(D0).Completed);
  EXPECT_EQ(observation(D0).IOStatus, StatusUnsuccessful);
  EXPECT_EQ(observation(D0).Power->DeviceStateAfter, DevicePowerState::D2);
  EXPECT_EQ(take(Model->pollWait(Wait)), policy::StatusPowerStateInvalid);
  EXPECT_EQ(invoke(api::WdfDeviceStopIdleNoTrack, {Globals, Device, 0}),
            policy::StatusPowerStateInvalid);
}
} // namespace
} // namespace neverd::emulation
