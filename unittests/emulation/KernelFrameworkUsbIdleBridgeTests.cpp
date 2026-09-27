//===- KernelFrameworkUsbIdleBridgeTests.cpp - Native USB idle packets ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "KernelFrameworkUsbIdleBridgeTestSupport.h"

namespace neverd::emulation {
namespace {
using namespace framework_usb_test;

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
