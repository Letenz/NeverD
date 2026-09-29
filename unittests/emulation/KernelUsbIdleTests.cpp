//===- KernelUsbIdleTests.cpp - USB idle ownership contracts --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "os/windows/KernelUsbIdle.h"
#include "os/windows/WindowsKernelLayout.h"

#include <limits>
#include <utility>

namespace neverd::emulation {
namespace {
using Cause = UsbIdleCompletionCause;

void success(llvm::Error E) {
  if (E)
    ADD_FAILURE() << llvm::toString(std::move(E));
}

template <class T> T take(llvm::Expected<T> Result) {
  if (!Result) {
    ADD_FAILURE() << llvm::toString(Result.takeError());
    return {};
  }
  return std::move(*Result);
}

void expectError(llvm::Error E, llvm::StringRef Text) {
  ASSERT_TRUE(bool(E));
  EXPECT_NE(llvm::toString(std::move(E)).find(Text.str()), std::string::npos);
}

template <class T>
void expectError(llvm::Expected<T> Result, llvm::StringRef Text) {
  ASSERT_FALSE(bool(Result));
  EXPECT_NE(llvm::toString(Result.takeError()).find(Text.str()),
            std::string::npos);
}

UsbIdleSubmission submission(uint64_t PDO = 0x1000, uint64_t IRP = 0x2000,
                             uint64_t Epoch = 1) {
  return {{PDO, IRP, Epoch}, IRP + 0x100, 0x180001000, 0};
}

KernelUsbIdle::CallbackPlan enter(KernelUsbIdle &Idle,
                                  const UsbIdleSubmission &Submission) {
  auto Calls = take(Idle.queueCallbacks({Submission.Key}));
  if (Calls.empty())
    return {};
  success(Idle.beginCallback(Calls.front().Token));
  return Calls.front();
}

void finishD2(KernelUsbIdle &Idle, uint64_t Token, uint64_t IRP = 0x9000,
              uint32_t Status = windows::StatusSuccess) {
  success(Idle.canIssueDevicePower(Token, DevicePowerRequest::Set,
                                   DevicePowerState::D2));
  success(Idle.issuedDevicePower(Token, IRP));
  success(Idle.completedDevicePower(IRP, Status));
}

void complete(KernelUsbIdle &Idle, UsbIdleKey Key, Cause Reason) {
  success(Idle.claimCompletion(take(Idle.planCompletion(Key, Reason))));
  success(Idle.retireCompletion(Key));
}

TEST(DriverKernelUsbIdle, RetainedRegistrationBorrowsOnlyItsInfoAndMayPark) {
  KernelUsbIdle Idle;
  auto S = submission();
  S.Context = 0xffffffffffffffff;
  success(Idle.submit(S));
  ASSERT_NE(Idle.submission(S.Key.PDO), nullptr);
  EXPECT_EQ(Idle.submissionForIRP(S.Key.IRP)->Context, S.Context);
  EXPECT_TRUE(Idle.isParked(S.Key.IRP));
  EXPECT_FALSE(Idle.isParked(S.Key.IRP + 1));
  success(Idle.canReleaseRange(S.InfoAddress - 1, 1));
  success(Idle.canReleaseRange(S.InfoAddress + usb_idle::CallbackInfoSize, 1));
  success(Idle.canReleaseRange(S.InfoAddress, 0));
  expectError(Idle.canReleaseRange(S.InfoAddress, 1), "borrowed");
  expectError(Idle.canReleaseRange(S.InfoAddress - 1, 2), "borrowed");
  expectError(
      Idle.canReleaseRange(S.InfoAddress + usb_idle::CallbackInfoSize - 1, 1),
      "borrowed");
  expectError(Idle.canReleaseRange(0, S.InfoAddress + 1), "borrowed");
  expectError(Idle.canReleaseRange(std::numeric_limits<uint64_t>::max(), 1),
              "overflows");
  complete(Idle, S.Key, Cause::Cancel);
  success(Idle.canReleaseRange(S.InfoAddress, usb_idle::CallbackInfoSize));
  EXPECT_FALSE(Idle.hasOutstanding(S.Key.PDO));
}

TEST(DriverKernelUsbIdle, InvalidAndDuplicateSubmissionsDoNotConsumeBound) {
  KernelUsbIdle Idle(1);
  auto S = submission();
  auto Bad = S;
  Bad.Key.StartEpoch = 0;
  expectError(Idle.submit(Bad), "nonzero");
  Bad = S;
  Bad.Callback = 0;
  expectError(Idle.submit(Bad), "nonzero");
  Bad = S;
  Bad.InfoAddress = std::numeric_limits<uint64_t>::max();
  expectError(Idle.submit(Bad), "overflows");
  success(Idle.submit(S));
  expectError(Idle.submit(S), "already");
  expectError(Idle.submit(submission(S.Key.PDO + 1, S.Key.IRP)), "already");
  expectError(Idle.submit(submission(S.Key.PDO + 1, S.Key.IRP + 1)), "bound");
  EXPECT_TRUE(Idle.isParked(S.Key.IRP));
  complete(Idle, S.Key, Cause::Remove);
  success(Idle.submit(submission(S.Key.PDO + 1, S.Key.IRP + 1)));
}

TEST(DriverKernelUsbIdle,
     CompositePermissionRequiresEveryExactMemberAtomically) {
  KernelUsbIdle Idle;
  const auto A = submission(), B = submission(0x3000, 0x4000);
  success(Idle.submit(A));
  expectError(Idle.capturePermission({A.Key.PDO, B.Key.PDO}), "every member");
  success(Idle.submit(B));
  expectError(Idle.capturePermission({A.Key.PDO, A.Key.PDO}), "duplicate");
  expectError(Idle.capturePermission({}), "at least one");
  auto Keys = take(Idle.capturePermission({A.Key.PDO, B.Key.PDO}));
  auto Stale = Keys;
  ++Stale.back().StartEpoch;
  expectError(Idle.queueCallbacks(Stale), "stale");
  EXPECT_TRUE(Idle.isParked(A.Key.IRP));
  EXPECT_TRUE(Idle.isParked(B.Key.IRP));
  auto Calls = take(Idle.queueCallbacks(Keys));
  ASSERT_EQ(Calls.size(), 2u);
  EXPECT_EQ(Calls[0].Key, A.Key);
  EXPECT_EQ(Calls[1].Key, B.Key);
  EXPECT_EQ(Calls[0].Token, 1u);
  EXPECT_EQ(Calls[1].Token, 2u);
  EXPECT_EQ(Calls[0].Context, 0u);
  EXPECT_FALSE(Idle.isParked(A.Key.IRP));
  EXPECT_FALSE(Idle.isParked(B.Key.IRP));
  expectError(Idle.queueCallbacks(Keys), "unclaimed retained");
}

TEST(DriverKernelUsbIdle, IndependentPermissionDoesNotCaptureAnotherPort) {
  KernelUsbIdle Idle;
  const auto A = submission(), B = submission(0x3000, 0x4000);
  success(Idle.submit(A));
  success(Idle.submit(B));
  auto Keys = take(Idle.capturePermission({A.Key.PDO}));
  ASSERT_EQ(Keys.size(), 1u);
  success(Idle.canQueueCallbacks(Keys));
  auto Calls = take(Idle.queueCallbacks(Keys));
  ASSERT_EQ(Calls.size(), 1u);
  EXPECT_FALSE(Idle.isParked(A.Key.IRP));
  EXPECT_TRUE(Idle.isParked(B.Key.IRP));
}

TEST(DriverKernelUsbIdle,
     CapturedPermissionNeverRetargetsReplacementOrRestart) {
  KernelUsbIdle Idle;
  auto Old = submission();
  success(Idle.submit(Old));
  auto Keys = take(Idle.capturePermission({Old.Key.PDO}));
  complete(Idle, Old.Key, Cause::Cancel);
  auto New = submission(Old.Key.PDO, 0x3000);
  success(Idle.submit(New));
  expectError(Idle.queueCallbacks(Keys), "stale");
  EXPECT_TRUE(Idle.isParked(New.Key.IRP));
  complete(Idle, New.Key, Cause::Remove);
  ++Old.Key.StartEpoch;
  success(Idle.submit(Old));
  expectError(Idle.queueCallbacks(Keys), "stale");
  EXPECT_TRUE(Idle.isParked(Old.Key.IRP));
}

TEST(DriverKernelUsbIdle, CallbackRequiresEntryAndItsExactTerminalD2Request) {
  KernelUsbIdle Idle;
  const auto S = submission();
  success(Idle.submit(S));
  auto Calls = take(Idle.queueCallbacks({S.Key}));
  ASSERT_EQ(Calls.size(), 1u);
  const uint64_t Token = Calls.front().Token;
  expectError(Idle.finishCallback(Token), "entered");
  expectError(Idle.issuedDevicePower(Token, 0x9000), "entered");
  success(Idle.beginCallback(Token));
  expectError(Idle.beginCallback(Token), "queued");
  expectError(Idle.finishCallback(Token), "before its D2");
  expectError(Idle.canIssueDevicePower(Token, DevicePowerRequest::Set,
                                       DevicePowerState::D0),
              "only");
  expectError(Idle.canIssueDevicePower(Token, DevicePowerRequest::Query,
                                       DevicePowerState::D2),
              "only");
  expectError(Idle.issuedDevicePower(Token, S.Key.IRP), "distinct");
  success(Idle.issuedDevicePower(Token, 0x9000));
  EXPECT_EQ(Idle.keyForDevicePower(0x9000), S.Key);
  expectError(Idle.issuedDevicePower(Token, 0xa000), "one D2");
  expectError(Idle.finishCallback(Token), "before its D2");
  expectError(Idle.completedDevicePower(0xa000, windows::StatusSuccess),
              "exact");
  expectError(Idle.completedDevicePower(0x9000, windows::StatusPending),
              "terminal");
  success(Idle.completedDevicePower(0x9000, windows::StatusSuccess));
  expectError(Idle.completedDevicePower(0x9000, windows::StatusSuccess),
              "duplicate");
  EXPECT_FALSE(take(Idle.finishCallback(Token)));
  EXPECT_EQ(Idle.callback(Token), nullptr);
  EXPECT_TRUE(Idle.isParked(S.Key.IRP));
  expectError(Idle.finishCallback(Token), "live owner");
  expectError(Idle.queueCallbacks({S.Key}), "unclaimed retained");
  complete(Idle, S.Key, Cause::DeviceD0);
}

TEST(DriverKernelUsbIdle, D2RequestCannotBelongToTwoCallbacks) {
  KernelUsbIdle Idle;
  const auto A = submission(), B = submission(0x3000, 0x4000);
  success(Idle.submit(A));
  success(Idle.submit(B));
  auto First = enter(Idle, A), Second = enter(Idle, B);
  success(Idle.issuedDevicePower(First.Token, 0x9000));
  expectError(Idle.issuedDevicePower(Second.Token, 0x9000), "distinct");
  finishD2(Idle, Second.Token, 0xa000);
  EXPECT_FALSE(take(Idle.finishCallback(Second.Token)));
  expectError(Idle.finishCallback(First.Token), "before its D2");
}

TEST(DriverKernelUsbIdle, FailedD2IsStillARealTerminalOutcome) {
  KernelUsbIdle Idle;
  const auto S = submission();
  success(Idle.submit(S));
  auto Call = enter(Idle, S);
  finishD2(Idle, Call.Token, 0x9000, windows::StatusNotSupported);
  EXPECT_FALSE(take(Idle.finishCallback(Call.Token)));
  EXPECT_TRUE(Idle.isParked(S.Key.IRP));
}

TEST(DriverKernelUsbIdle, CancelBeforeEntryRequiresExactQueueWithdrawal) {
  KernelUsbIdle Idle;
  const auto S = submission();
  success(Idle.submit(S));
  auto Calls = take(Idle.queueCallbacks({S.Key}));
  ASSERT_EQ(Calls.size(), 1u);
  const uint64_t Token = Calls.front().Token;
  expectError(Idle.withdrawCallback(Token), "claimed queued");
  auto Plan = take(Idle.planCompletion(S.Key, Cause::Cancel));
  EXPECT_FALSE(Plan.DeferredUntilCallbackReturn);
  success(Idle.claimCompletion(Plan));
  expectError(Idle.beginCallback(Token), "unclaimed queued");
  expectError(Idle.retireCompletion(S.Key), "queued or entered");
  expectError(Idle.canReleaseRange(S.InfoAddress, 1), "borrowed");
  success(Idle.withdrawCallback(Token));
  EXPECT_EQ(Idle.callback(Token), nullptr);
  EXPECT_FALSE(Idle.isParked(S.Key.IRP));
  success(Idle.retireCompletion(S.Key));
  success(Idle.canReleaseRange(S.InfoAddress, usb_idle::CallbackInfoSize));
}

TEST(DriverKernelUsbIdle, EnteredCancellationKeepsBorrowUntilD2AndReturn) {
  KernelUsbIdle Idle;
  const auto S = submission();
  success(Idle.submit(S));
  auto Call = enter(Idle, S);
  success(Idle.issuedDevicePower(Call.Token, 0x9000));
  auto Cancel = take(Idle.planCompletion(S.Key, Cause::Cancel));
  EXPECT_TRUE(Cancel.DeferredUntilCallbackReturn);
  success(Idle.claimCompletion(Cancel));
  expectError(Idle.withdrawCallback(Call.Token), "claimed queued");
  expectError(Idle.retireCompletion(S.Key), "queued or entered");
  expectError(Idle.finishCallback(Call.Token), "before its D2");
  expectError(Idle.canReleaseRange(S.InfoAddress, 1), "borrowed");
  auto Later = take(Idle.planCompletion(S.Key, Cause::DeviceD3));
  EXPECT_EQ(Later.Cause, Cause::Cancel);
  success(Idle.claimCompletion(Later));
  success(Idle.completedDevicePower(0x9000, windows::StatusSuccess));
  auto Ready = take(Idle.finishCallback(Call.Token));
  ASSERT_TRUE(Ready);
  EXPECT_EQ(Ready->Cause, Cause::Cancel);
  EXPECT_FALSE(Ready->DeferredUntilCallbackReturn);
  expectError(Idle.claimCompletion(Cancel), "changed");
  success(Idle.claimCompletion(*Ready));
  expectError(Idle.canReleaseRange(S.InfoAddress, 1), "borrowed");
  success(Idle.retireCompletion(S.Key));
  success(Idle.canReleaseRange(S.InfoAddress, 1));
}

TEST(DriverKernelUsbIdle, FirstCompletionCauseSurvivesLaterReceiptOrCancel) {
  for (Cause First : {Cause::DeviceD0, Cause::DeviceD3, Cause::SystemSleep,
                      Cause::Remove, Cause::Cancel}) {
    KernelUsbIdle Idle;
    const auto S = submission();
    success(Idle.submit(S));
    auto Call = enter(Idle, S);
    success(Idle.claimCompletion(take(Idle.planCompletion(S.Key, First))));
    for (Cause Later : {Cause::Cancel, Cause::DeviceD0, Cause::DeviceD3}) {
      auto Plan = take(Idle.planCompletion(S.Key, Later));
      EXPECT_EQ(Plan.Cause, First);
      success(Idle.claimCompletion(Plan));
    }
    finishD2(Idle, Call.Token);
    auto Plan = take(Idle.finishCallback(Call.Token));
    ASSERT_TRUE(Plan);
    EXPECT_EQ(Plan->Cause, First);
    success(Idle.retireCompletion(S.Key));
  }
}

TEST(DriverKernelUsbIdle, AllocationFailureEscapeRequiresRealFailureAndCancel) {
  KernelUsbIdle Idle;
  const auto S = submission();
  success(Idle.submit(S));
  auto Call = enter(Idle, S);
  expectError(
      Idle.failedDevicePowerAdmission(Call.Token, windows::StatusNotSupported),
      "insufficient resources");
  success(Idle.failedDevicePowerAdmission(
      Call.Token, windows::StatusInsufficientResources));
  expectError(Idle.finishCallback(Call.Token), "before its D2");
  expectError(Idle.issuedDevicePower(Call.Token, 0x9000), "one D2");
  success(
      Idle.claimCompletion(take(Idle.planCompletion(S.Key, Cause::Cancel))));
  auto Plan = take(Idle.finishCallback(Call.Token));
  ASSERT_TRUE(Plan);
  EXPECT_EQ(Plan->Cause, Cause::Cancel);
  success(Idle.retireCompletion(S.Key));
}

TEST(DriverKernelUsbIdle,
     CancellationAloneDoesNotAuthorizeAnEarlyCallbackReturn) {
  KernelUsbIdle Idle;
  const auto S = submission();
  success(Idle.submit(S));
  auto Call = enter(Idle, S);
  success(
      Idle.claimCompletion(take(Idle.planCompletion(S.Key, Cause::Cancel))));
  expectError(Idle.finishCallback(Call.Token), "before its D2");
  finishD2(Idle, Call.Token);
  ASSERT_TRUE(take(Idle.finishCallback(Call.Token)));
}

TEST(DriverKernelUsbIdle, CompletionPlansRevalidatePhaseCauseAndExactIdentity) {
  KernelUsbIdle Idle;
  const auto S = submission();
  success(Idle.submit(S));
  expectError(Idle.retireCompletion(S.Key), "claimed cause");
  auto BeforeEntry = take(Idle.planCompletion(S.Key, Cause::DeviceD0));
  auto Call = enter(Idle, S);
  expectError(Idle.claimCompletion(BeforeEntry), "changed");
  EXPECT_EQ(take(Idle.planCompletion(S.Key, Cause::Cancel)).Cause,
            Cause::Cancel);
  auto Plan = take(Idle.planCompletion(S.Key, Cause::DeviceD3));
  success(Idle.claimCompletion(Plan));
  Plan.Cause = Cause::Cancel;
  expectError(Idle.claimCompletion(Plan), "changed");
  Plan.Key.IRP++;
  expectError(Idle.claimCompletion(Plan), "stale");
  expectError(Idle.planCompletion(S.Key, static_cast<Cause>(255)), "invalid");
  finishD2(Idle, Call.Token);
  ASSERT_TRUE(take(Idle.finishCallback(Call.Token)));
  success(Idle.retireCompletion(S.Key));
  auto Replacement = submission(S.Key.PDO, S.Key.IRP + 1);
  success(Idle.submit(Replacement));
  expectError(Idle.retireCompletion(S.Key), "stale");
  EXPECT_TRUE(Idle.hasOutstanding(Replacement.Key.PDO));
  EXPECT_TRUE(Idle.isParked(Replacement.Key.IRP));
}

TEST(DriverKernelUsbIdle, MixedPermissionRetainsTypedCallbackOwnership) {
  KernelUsbIdle Idle;
  const auto Guest = submission();
  auto Framework = submission(0x3000, 0x4000);
  Framework.Owner = UsbIdleCallbackOwner::Framework;
  auto Invalid = Framework;
  Invalid.Owner = static_cast<UsbIdleCallbackOwner>(255);
  expectError(Idle.submit(Invalid), "invalid callback owner");
  success(Idle.submit(Guest));
  success(Idle.submit(Framework));
  EXPECT_EQ(Idle.callbackForIRP(Guest.Key.IRP), nullptr);
  const auto Preview = take(Idle.previewCallbacks({Guest.Key, Framework.Key}));
  EXPECT_EQ(take(Idle.previewCallbacks({Guest.Key, Framework.Key})), Preview);
  EXPECT_TRUE(Idle.isParked(Guest.Key.IRP));
  EXPECT_TRUE(Idle.isParked(Framework.Key.IRP));
  EXPECT_EQ(Idle.callbackForIRP(Framework.Key.IRP), nullptr);
  auto Calls = take(Idle.queueCallbacks({Guest.Key, Framework.Key}));
  EXPECT_EQ(Calls, Preview);
  ASSERT_EQ(Calls.size(), 2u);
  EXPECT_EQ(Calls[0].Owner, UsbIdleCallbackOwner::Guest);
  EXPECT_EQ(Calls[1].Owner, UsbIdleCallbackOwner::Framework);
  EXPECT_EQ(Calls[0].Token, 1u);
  for (const auto &Call : Calls) {
    ASSERT_NE(Idle.callbackForIRP(Call.Key.IRP), nullptr);
    EXPECT_EQ(Idle.callbackForIRP(Call.Key.IRP)->Token, Call.Token);
    success(Idle.beginCallback(Call.Token));
    EXPECT_EQ(Idle.callbackForIRP(Call.Key.IRP)->Owner, Call.Owner);
    finishD2(Idle, Call.Token, Call.Key.IRP + 0x10000);
    EXPECT_FALSE(take(Idle.finishCallback(Call.Token)));
    EXPECT_EQ(Idle.callbackForIRP(Call.Key.IRP), nullptr);
    EXPECT_TRUE(Idle.isParked(Call.Key.IRP));
  }
}

TEST(DriverKernelUsbIdle, NativeArmFailureRequiresActualCancellation) {
  using Failure = KernelUsbIdle::FrameworkCallbackFailure;
  KernelUsbIdle Idle;
  auto S = submission();
  S.Owner = UsbIdleCallbackOwner::Framework;
  success(Idle.submit(S));
  auto Call = enter(Idle, S);
  for (uint32_t Status : {windows::StatusSuccess, windows::StatusPending})
    expectError(
        Idle.failFrameworkCallback(Call.Token, Failure::ArmWake, Status),
        "failing callback result");
  expectError(Idle.failFrameworkCallback(Call.Token, static_cast<Failure>(255),
                                         windows::StatusNotSupported),
              "invalid native");
  expectError(Idle.cancelFrameworkCallback(Call.Token), "claimed entered");
  success(Idle.failFrameworkCallback(Call.Token, Failure::ArmWake,
                                     windows::StatusNotSupported));
  expectError(Idle.finishCallback(Call.Token), "before its D2");
  expectError(Idle.issuedDevicePower(Call.Token, 0x9000), "one D2");
  success(
      Idle.claimCompletion(take(Idle.planCompletion(S.Key, Cause::Cancel))));
  expectError(Idle.finishCallback(Call.Token), "before its D2");
  success(Idle.cancelFrameworkCallback(Call.Token));
  success(Idle.cancelFrameworkCallback(Call.Token));
  auto Plan = take(Idle.finishCallback(Call.Token));
  ASSERT_TRUE(Plan);
  EXPECT_EQ(Plan->Cause, Cause::Cancel);
  EXPECT_FALSE(Plan->DeferredUntilCallbackReturn);
  expectError(Idle.canReleaseRange(S.InfoAddress, 1), "borrowed");
  success(Idle.retireCompletion(S.Key));
  EXPECT_EQ(Idle.callbackForIRP(S.Key.IRP), nullptr);
}

TEST(DriverKernelUsbIdle, NativeAllocationFailureCannotUseRawEscape) {
  using Failure = KernelUsbIdle::FrameworkCallbackFailure;
  KernelUsbIdle Idle;
  auto S = submission();
  S.Owner = UsbIdleCallbackOwner::Framework;
  success(Idle.submit(S));
  auto Call = enter(Idle, S);
  expectError(Idle.failedDevicePowerAdmission(
                  Call.Token, windows::StatusInsufficientResources),
              "guest callback owner");
  expectError(Idle.failFrameworkCallback(Call.Token, Failure::PowerAllocation,
                                         windows::StatusNotSupported),
              "insufficient resources");
  success(Idle.failFrameworkCallback(Call.Token, Failure::PowerAllocation,
                                     windows::StatusInsufficientResources));
  expectError(Idle.failFrameworkCallback(Call.Token, Failure::ArmWake,
                                         windows::StatusNotSupported),
              "one D2");
  success(
      Idle.claimCompletion(take(Idle.planCompletion(S.Key, Cause::Cancel))));
  success(Idle.cancelFrameworkCallback(Call.Token));
  ASSERT_TRUE(take(Idle.finishCallback(Call.Token)));
  success(Idle.retireCompletion(S.Key));
}

TEST(DriverKernelUsbIdle, NativeFailurePreservesEarlierReceiptCause) {
  KernelUsbIdle Idle;
  auto S = submission();
  S.Owner = UsbIdleCallbackOwner::Framework;
  success(Idle.submit(S));
  auto Call = enter(Idle, S);
  success(
      Idle.claimCompletion(take(Idle.planCompletion(S.Key, Cause::DeviceD0))));
  success(Idle.failFrameworkCallback(
      Call.Token, KernelUsbIdle::FrameworkCallbackFailure::ArmWake,
      windows::StatusNotSupported));
  auto Cancellation = take(Idle.planCompletion(S.Key, Cause::Cancel));
  EXPECT_EQ(Cancellation.Cause, Cause::DeviceD0);
  success(Idle.claimCompletion(Cancellation));
  success(Idle.cancelFrameworkCallback(Call.Token));
  auto Plan = take(Idle.finishCallback(Call.Token));
  ASSERT_TRUE(Plan);
  EXPECT_EQ(Plan->Cause, Cause::DeviceD0);
  success(Idle.retireCompletion(S.Key));
}

TEST(DriverKernelUsbIdle, NativeCancellationCannotBypassAnIssuedD2) {
  KernelUsbIdle Idle;
  auto S = submission();
  S.Owner = UsbIdleCallbackOwner::Framework;
  success(Idle.submit(S));
  auto Call = enter(Idle, S);
  success(
      Idle.claimCompletion(take(Idle.planCompletion(S.Key, Cause::Cancel))));
  success(Idle.cancelFrameworkCallback(Call.Token));
  expectError(Idle.finishCallback(Call.Token), "before its D2");
  success(Idle.issuedDevicePower(Call.Token, 0x9000));
  expectError(Idle.failFrameworkCallback(
                  Call.Token, KernelUsbIdle::FrameworkCallbackFailure::ArmWake,
                  windows::StatusNotSupported),
              "one D2");
  expectError(Idle.finishCallback(Call.Token), "before its D2");
  success(Idle.completedDevicePower(0x9000, windows::StatusSuccess));
  ASSERT_TRUE(take(Idle.finishCallback(Call.Token)));
  success(Idle.retireCompletion(S.Key));
}

TEST(DriverKernelUsbIdle, GuestCallbackCannotUseNativeFailureEvidence) {
  KernelUsbIdle Idle;
  const auto S = submission();
  success(Idle.submit(S));
  auto Call = enter(Idle, S);
  success(
      Idle.claimCompletion(take(Idle.planCompletion(S.Key, Cause::Cancel))));
  expectError(Idle.cancelFrameworkCallback(Call.Token), "framework callback");
  expectError(Idle.failFrameworkCallback(
                  Call.Token, KernelUsbIdle::FrameworkCallbackFailure::ArmWake,
                  windows::StatusNotSupported),
              "framework callback owner");
  expectError(Idle.finishCallback(Call.Token), "before its D2");
  finishD2(Idle, Call.Token);
  ASSERT_TRUE(take(Idle.finishCallback(Call.Token)));
}

TEST(DriverKernelUsbIdle, NativeRetirementExemptsOnlyItsExactBorrow) {
  KernelUsbIdle Idle;
  auto Native = submission();
  Native.Owner = UsbIdleCallbackOwner::Framework;
  success(Idle.submit(Native));
  expectError(Idle.canReleaseRange(Native.InfoAddress, 1), "borrowed");
  success(Idle.canReleaseRange(Native.InfoAddress, usb_idle::CallbackInfoSize,
                               Native.Key));
  for (auto Stale : {UsbIdleKey{Native.Key.PDO + 1, Native.Key.IRP, 1},
                     UsbIdleKey{Native.Key.PDO, Native.Key.IRP + 1, 1},
                     UsbIdleKey{Native.Key.PDO, Native.Key.IRP, 2}})
    expectError(Idle.canReleaseRange(Native.InfoAddress, 1, Stale), "borrowed");
  auto Guest = submission(0x3000, 0x4000);
  Guest.InfoAddress = Native.InfoAddress;
  success(Idle.submit(Guest));
  expectError(Idle.canReleaseRange(Native.InfoAddress, 1, Native.Key),
              "borrowed");
  complete(Idle, Native.Key, Cause::Cancel);
  expectError(Idle.canReleaseRange(Guest.InfoAddress, 1, Guest.Key),
              "borrowed");
  expectError(Idle.canReleaseRange(Guest.InfoAddress, 1, Native.Key),
              "borrowed");
  complete(Idle, Guest.Key, Cause::Cancel);
  success(Idle.canReleaseRange(Guest.InfoAddress, 1, Native.Key));
}
} // namespace
} // namespace neverd::emulation
