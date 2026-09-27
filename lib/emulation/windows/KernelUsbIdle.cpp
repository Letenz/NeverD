//===- KernelUsbIdle.cpp - USB idle registration protocol ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "KernelUsbIdle.h"

#include "WindowsKernelLayout.h"

#include <limits>
#include <set>
#include <utility>

namespace neverd::emulation {
namespace {
llvm::Error idleError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "USB idle: " + Message);
}

bool validCause(UsbIdleCompletionCause Cause) {
  switch (Cause) {
  case UsbIdleCompletionCause::Cancel:
  case UsbIdleCompletionCause::SystemSleep:
  case UsbIdleCompletionCause::Remove:
  case UsbIdleCompletionCause::DeviceD0:
  case UsbIdleCompletionCause::DeviceD3:
    return true;
  }
  return false;
}
} // namespace

uint32_t KernelUsbIdle::completionStatus(UsbIdleCompletionCause Cause) {
  switch (Cause) {
  case UsbIdleCompletionCause::DeviceD0:
    return windows::StatusSuccess;
  case UsbIdleCompletionCause::DeviceD3:
    return usb_idle::StatusPowerStateInvalid;
  case UsbIdleCompletionCause::Cancel:
  case UsbIdleCompletionCause::SystemSleep:
  case UsbIdleCompletionCause::Remove:
    return windows::StatusCancelled;
  }
  llvm_unreachable("invalid USB idle completion cause");
}

llvm::Error
KernelUsbIdle::canSubmit(const UsbIdleSubmission &Submission) const {
  const auto &Key = Submission.Key;
  if (!Key.PDO || !Key.IRP || !Key.StartEpoch || !Submission.InfoAddress ||
      !Submission.Callback)
    return idleError(
        "submission requires nonzero identities, info and callback");
  if (Submission.InfoAddress >
      std::numeric_limits<uint64_t>::max() - usb_idle::CallbackInfoSize)
    return idleError("callback-info range overflows");
  if (Registrations.contains(Key.PDO) || submissionForIRP(Key.IRP) ||
      keyForDevicePower(Key.IRP))
    return idleError("device or IRP already has an idle registration");
  if (Registrations.size() >= MaxRegistrations)
    return idleError("registration bound exhausted");
  return llvm::Error::success();
}

llvm::Error KernelUsbIdle::submit(UsbIdleSubmission Submission) {
  if (auto E = canSubmit(Submission))
    return E;
  Record R;
  R.Submission = std::move(Submission);
  Registrations.emplace(R.Submission.Key.PDO, std::move(R));
  return llvm::Error::success();
}

const UsbIdleSubmission *KernelUsbIdle::submission(uint64_t PDO) const {
  const auto It = Registrations.find(PDO);
  return It == Registrations.end() ? nullptr : &It->second.Submission;
}

const UsbIdleSubmission *KernelUsbIdle::submissionForIRP(uint64_t IRP) const {
  for (const auto &[PDO, R] : Registrations)
    if (R.Submission.Key.IRP == IRP)
      return &R.Submission;
  return nullptr;
}

llvm::Expected<const KernelUsbIdle::Record *>
KernelUsbIdle::find(UsbIdleKey Key) const {
  const auto It = Registrations.find(Key.PDO);
  if (It == Registrations.end() || It->second.Submission.Key != Key)
    return idleError("idle identity or START epoch is stale");
  return &It->second;
}

llvm::Expected<std::vector<UsbIdleKey>>
KernelUsbIdle::capturePermission(llvm::ArrayRef<uint64_t> Members) const {
  std::vector<UsbIdleKey> Keys;
  Keys.reserve(Members.size());
  for (uint64_t PDO : Members) {
    const auto *Submission = submission(PDO);
    if (!Submission)
      return idleError("permission requires every member's idle registration");
    Keys.push_back(Submission->Key);
  }
  if (auto E = canQueueCallbacks(Keys))
    return std::move(E);
  return Keys;
}

llvm::Error
KernelUsbIdle::canQueueCallbacks(llvm::ArrayRef<UsbIdleKey> Keys) const {
  if (Keys.empty())
    return idleError("permission requires at least one member");
  if (Keys.size() > std::numeric_limits<uint64_t>::max() - NextToken)
    return idleError("callback identity bound exhausted");
  std::set<uint64_t> Members;
  for (auto Key : Keys) {
    if (!Members.insert(Key.PDO).second)
      return idleError("permission contains a duplicate member");
    auto Found = find(Key);
    if (!Found)
      return Found.takeError();
    const auto &R = **Found;
    if (R.State != Phase::Retained || R.CompletionCause)
      return idleError(
          "permission requires an unclaimed retained registration");
  }
  return llvm::Error::success();
}

llvm::Expected<std::vector<KernelUsbIdle::CallbackPlan>>
KernelUsbIdle::queueCallbacks(llvm::ArrayRef<UsbIdleKey> Keys) {
  if (auto E = canQueueCallbacks(Keys))
    return std::move(E);
  std::vector<CallbackPlan> Calls;
  Calls.reserve(Keys.size());
  for (auto Key : Keys) {
    auto &R = Registrations.at(Key.PDO);
    R.Call = CallbackPlan{NextToken++, Key, R.Submission.Callback,
                          R.Submission.Context};
    R.State = Phase::CallbackQueued;
    Calls.push_back(*R.Call);
  }
  return Calls;
}

const KernelUsbIdle::CallbackPlan *
KernelUsbIdle::callback(uint64_t Token) const {
  for (const auto &[PDO, R] : Registrations)
    if (R.Call && R.Call->Token == Token &&
        (R.State == Phase::CallbackQueued || R.State == Phase::CallbackEntered))
      return &*R.Call;
  return nullptr;
}

llvm::Expected<const KernelUsbIdle::Record *>
KernelUsbIdle::findCallback(uint64_t Token) const {
  const auto *Call = callback(Token);
  if (!Call)
    return idleError("callback token has no live owner");
  return find(Call->Key);
}

llvm::Error KernelUsbIdle::beginCallback(uint64_t Token) {
  auto Found = findCallback(Token);
  if (!Found)
    return Found.takeError();
  const auto &R = **Found;
  if (R.State != Phase::CallbackQueued || R.CompletionCause)
    return idleError("callback entry requires an unclaimed queued invocation");
  Registrations.at(R.Submission.Key.PDO).State = Phase::CallbackEntered;
  return llvm::Error::success();
}

llvm::Expected<std::optional<UsbIdleCompletionPlan>>
KernelUsbIdle::finishCallback(uint64_t Token) {
  auto Found = findCallback(Token);
  if (!Found)
    return Found.takeError();
  const auto &R = **Found;
  if (R.State != Phase::CallbackEntered)
    return idleError("callback return requires an entered invocation");
  const bool CancelledAllocationFailure =
      R.AllocationFailed && R.CompletionCause == UsbIdleCompletionCause::Cancel;
  if (!R.DevicePowerStatus && !CancelledAllocationFailure)
    return idleError("callback returned before its D2 request completed");
  Registrations.at(R.Submission.Key.PDO).State = Phase::CallbackReturned;
  if (!R.CompletionCause)
    return std::optional<UsbIdleCompletionPlan>{};
  return std::optional<UsbIdleCompletionPlan>{
      UsbIdleCompletionPlan{R.Submission.Key, *R.CompletionCause, false}};
}

llvm::Error KernelUsbIdle::withdrawCallback(uint64_t Token) {
  auto Found = findCallback(Token);
  if (!Found)
    return Found.takeError();
  const auto &R = **Found;
  if (R.State != Phase::CallbackQueued || !R.CompletionCause)
    return idleError("withdrawal requires a claimed queued callback");
  auto &Changed = Registrations.at(R.Submission.Key.PDO);
  Changed.Call.reset();
  Changed.State = Phase::Retained;
  return llvm::Error::success();
}

llvm::Error KernelUsbIdle::canIssueDevicePower(const Record &R) const {
  if (R.State != Phase::CallbackEntered)
    return idleError("D2 issuance requires an entered idle callback");
  if (R.DevicePowerIRP || R.AllocationFailed)
    return idleError("idle callback already attempted its one D2 request");
  return llvm::Error::success();
}

llvm::Error KernelUsbIdle::canIssueDevicePower(uint64_t Token,
                                               DevicePowerRequest Minor,
                                               DevicePowerState Target) const {
  auto Found = findCallback(Token);
  if (!Found)
    return Found.takeError();
  if (Minor != DevicePowerRequest::Set || Target != DevicePowerState::D2)
    return idleError("idle callback may issue only its device SET D2 request");
  return canIssueDevicePower(**Found);
}

llvm::Error KernelUsbIdle::issuedDevicePower(uint64_t Token, uint64_t IRP) {
  auto Found = findCallback(Token);
  if (!Found)
    return Found.takeError();
  if (auto E = canIssueDevicePower(**Found))
    return E;
  if (!IRP || submissionForIRP(IRP) || keyForDevicePower(IRP))
    return idleError("D2 request requires a distinct nonzero IRP identity");
  Registrations.at((**Found).Submission.Key.PDO).DevicePowerIRP = IRP;
  return llvm::Error::success();
}

std::optional<UsbIdleKey> KernelUsbIdle::keyForDevicePower(uint64_t IRP) const {
  for (const auto &[PDO, R] : Registrations)
    if (R.DevicePowerIRP == IRP)
      return R.Submission.Key;
  return std::nullopt;
}

llvm::Error KernelUsbIdle::completedDevicePower(uint64_t IRP, uint32_t Status) {
  const auto Key = keyForDevicePower(IRP);
  if (!Key)
    return idleError("D2 completion has no exact idle callback owner");
  auto &R = Registrations.at(Key->PDO);
  if (R.State != Phase::CallbackEntered || R.DevicePowerStatus)
    return idleError("D2 completion is duplicate or outside its callback");
  if (Status == windows::StatusPending)
    return idleError("D2 completion requires a terminal status");
  R.DevicePowerStatus = Status;
  return llvm::Error::success();
}

llvm::Error KernelUsbIdle::failedDevicePowerAdmission(uint64_t Token,
                                                      uint32_t Status) {
  auto Found = findCallback(Token);
  if (!Found)
    return Found.takeError();
  if (auto E = canIssueDevicePower(**Found))
    return E;
  if (Status != windows::StatusInsufficientResources)
    return idleError("D2 allocation escape requires insufficient resources");
  Registrations.at((**Found).Submission.Key.PDO).AllocationFailed = true;
  return llvm::Error::success();
}

llvm::Expected<UsbIdleCompletionPlan>
KernelUsbIdle::planCompletion(UsbIdleKey Key,
                              UsbIdleCompletionCause Cause) const {
  auto Found = find(Key);
  if (!Found)
    return Found.takeError();
  if (!validCause(Cause))
    return idleError("invalid idle completion cause");
  const auto &R = **Found;
  return UsbIdleCompletionPlan{Key, R.CompletionCause.value_or(Cause),
                               R.State == Phase::CallbackEntered};
}

llvm::Error KernelUsbIdle::claimCompletion(const UsbIdleCompletionPlan &Plan) {
  auto Current = planCompletion(Plan.Key, Plan.Cause);
  if (!Current)
    return Current.takeError();
  if (*Current != Plan)
    return idleError("completion plan changed before its claim");
  Registrations.at(Plan.Key.PDO).CompletionCause = Plan.Cause;
  return llvm::Error::success();
}

llvm::Error KernelUsbIdle::retireCompletion(UsbIdleKey Key) {
  auto Found = find(Key);
  if (!Found)
    return Found.takeError();
  const auto &R = **Found;
  if (!R.CompletionCause)
    return idleError("completion requires a claimed cause");
  if (R.State == Phase::CallbackQueued || R.State == Phase::CallbackEntered)
    return idleError("completion still owns a queued or entered callback");
  Registrations.erase(Key.PDO);
  return llvm::Error::success();
}

bool KernelUsbIdle::isParked(uint64_t IRP) const {
  const auto *Submission = submissionForIRP(IRP);
  if (!Submission)
    return false;
  const auto &R = Registrations.at(Submission->Key.PDO);
  return !R.CompletionCause &&
         (R.State == Phase::Retained || R.State == Phase::CallbackReturned);
}

llvm::Error KernelUsbIdle::canReleaseRange(uint64_t Base, uint64_t Size) const {
  if (Size > std::numeric_limits<uint64_t>::max() - Base)
    return idleError("released range overflows");
  if (!Size)
    return llvm::Error::success();
  for (const auto &[PDO, R] : Registrations) {
    const uint64_t Info = R.Submission.InfoAddress;
    if (Base < Info + usb_idle::CallbackInfoSize && Info < Base + Size)
      return idleError(
          "range is borrowed by an outstanding callback-info record");
  }
  return llvm::Error::success();
}

bool KernelUsbIdle::hasOutstanding(uint64_t PDO) const {
  return Registrations.contains(PDO);
}
} // namespace neverd::emulation
