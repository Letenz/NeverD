//===- KernelUsbIdle.h - USB idle protocol ------------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_WINDOWS_KERNELUSBIDLE_H
#define NEVERD_EMULATION_WINDOWS_KERNELUSBIDLE_H

#include "neverd/emulation/DriverPnp.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace neverd::emulation {
namespace usb_idle {
#define NEVERD_KERNEL_USB_IDLE_VALUE(Name, Value)                              \
  inline constexpr uint64_t Name = Value;
#include "KernelUsbIdleValues.def"
#undef NEVERD_KERNEL_USB_IDLE_VALUE
} // namespace usb_idle

struct UsbIdleKey {
  uint64_t PDO = 0;
  uint64_t IRP = 0;
  uint64_t StartEpoch = 0;
  bool operator==(const UsbIdleKey &) const = default;
};

struct UsbIdleSubmission {
  UsbIdleKey Key;
  uint64_t InfoAddress = 0;
  uint64_t Callback = 0;
  uint64_t Context = 0;
};

using UsbIdleCompletionCause = DriverUsbIdleCompletionCause;

struct UsbIdleCompletionPlan {
  UsbIdleKey Key;
  UsbIdleCompletionCause Cause = UsbIdleCompletionCause::Cancel;
  bool DeferredUntilCallbackReturn = false;
  bool operator==(const UsbIdleCompletionPlan &) const = default;
};

/// Own only the idle notification protocol. The bridge validates topology,
/// guest access, live START and power states, and scheduler capacity. Actual
/// IRP completion, allocation and device-power transitions retain their
/// existing owners.
class KernelUsbIdle {
public:
  struct CallbackPlan {
    uint64_t Token = 0;
    UsbIdleKey Key;
    uint64_t PC = 0;
    uint64_t Context = 0;
  };

  KernelUsbIdle() = default;
  explicit KernelUsbIdle(uint64_t MaxRegistrations)
      : MaxRegistrations(MaxRegistrations) {}

  static uint32_t completionStatus(UsbIdleCompletionCause Cause);

  llvm::Error canSubmit(const UsbIdleSubmission &Submission) const;
  llvm::Error submit(UsbIdleSubmission Submission);
  const UsbIdleSubmission *submission(uint64_t PDO) const;
  const UsbIdleSubmission *submissionForIRP(uint64_t IRP) const;

  /// Members are the complete resolved group, not a partial set inferred here.
  llvm::Expected<std::vector<UsbIdleKey>>
  capturePermission(llvm::ArrayRef<uint64_t> Members) const;
  llvm::Error canQueueCallbacks(llvm::ArrayRef<UsbIdleKey> Keys) const;
  llvm::Expected<std::vector<CallbackPlan>>
  queueCallbacks(llvm::ArrayRef<UsbIdleKey> Keys);
  const CallbackPlan *callback(uint64_t Token) const;
  llvm::Error beginCallback(uint64_t Token);
  llvm::Expected<std::optional<UsbIdleCompletionPlan>>
  finishCallback(uint64_t Token);
  /// Called only after the bridge has withdrawn the exact queued invocation.
  llvm::Error withdrawCallback(uint64_t Token);

  llvm::Error canIssueDevicePower(uint64_t Token, DevicePowerRequest Minor,
                                  DevicePowerState Target) const;
  llvm::Error issuedDevicePower(uint64_t Token, uint64_t IRP);
  llvm::Error completedDevicePower(uint64_t IRP, uint32_t Status);
  llvm::Error failedDevicePowerAdmission(uint64_t Token, uint32_t Status);
  std::optional<UsbIdleKey> keyForDevicePower(uint64_t IRP) const;

  llvm::Expected<UsbIdleCompletionPlan>
  planCompletion(UsbIdleKey Key, UsbIdleCompletionCause Cause) const;
  llvm::Error claimCompletion(const UsbIdleCompletionPlan &Plan);
  /// Releases the old claim and info borrow before actual guest IoCompletion.
  /// Entered callbacks must return and queued callbacks must be withdrawn.
  llvm::Error retireCompletion(UsbIdleKey Key);

  bool isParked(uint64_t IRP) const;
  llvm::Error canReleaseRange(uint64_t Base, uint64_t Size) const;
  bool hasOutstanding(uint64_t PDO) const;

private:
  enum class Phase {
    Retained,
    CallbackQueued,
    CallbackEntered,
    CallbackReturned
  };
  struct Record {
    UsbIdleSubmission Submission;
    Phase State = Phase::Retained;
    std::optional<CallbackPlan> Call;
    std::optional<uint64_t> DevicePowerIRP;
    std::optional<uint32_t> DevicePowerStatus;
    bool AllocationFailed = false;
    std::optional<UsbIdleCompletionCause> CompletionCause;
  };

  llvm::Expected<const Record *> find(UsbIdleKey Key) const;
  llvm::Expected<const Record *> findCallback(uint64_t Token) const;
  llvm::Error canIssueDevicePower(const Record &R) const;

  uint64_t MaxRegistrations = usb_idle::DefaultMaxRegistrations;
  uint64_t NextToken = 1;
  std::map<uint64_t, Record> Registrations;
};
} // namespace neverd::emulation

#endif
