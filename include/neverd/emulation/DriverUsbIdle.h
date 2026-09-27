//===- DriverUsbIdle.h - USB idle provider facts ------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_DRIVERUSBIDLE_H
#define NEVERD_EMULATION_DRIVERUSBIDLE_H

#include <cstdint>
#include <optional>
#include <string>

namespace neverd::emulation {

enum class DriverUsbIdleRole {
#define NEVERD_DRIVER_USB_IDLE_ROLE(Name, Spelling) Name,
#include "neverd/emulation/DriverUsbIdle.def"
#undef NEVERD_DRIVER_USB_IDLE_ROLE
};

/// Explicit selective-idle protocol capability, independent of resource bus
/// transport. Composite parents coordinate their immediate function children;
/// only function roles can submit an idle registration.
struct DriverUsbIdleConfig {
  DriverUsbIdleRole Role = DriverUsbIdleRole::IndependentFunction;
  /// USB remote wake from D2. Generic wake capabilities do not imply this fact.
  bool RemoteWake = false;
};

enum class DriverUsbIdleCompletionCause {
#define NEVERD_DRIVER_USB_IDLE_COMPLETION_CAUSE(Name, Spelling) Name,
#include "neverd/emulation/DriverUsbIdle.def"
#undef NEVERD_DRIVER_USB_IDLE_COMPLETION_CAUSE
};

/// Actual facts on the original internal IOCTL result. An absent timestamp or
/// child identity means the corresponding protocol boundary was not observed.
struct DriverUsbIdleRequestResult {
  uint64_t StartEpoch = 0;
  std::optional<uint64_t> BusReceivedAt100ns;
  std::optional<uint64_t> CallbackEnteredAt100ns;
  std::optional<uint64_t> CallbackReturnedAt100ns;
  std::optional<uint64_t> D2IRP;
  std::optional<uint32_t> D2Status;
  std::optional<uint64_t> D2CompletedAt100ns;
  std::optional<DriverUsbIdleCompletionCause> CompletionCause;
  std::optional<uint64_t> CompletionClaimedAt100ns;
  std::optional<uint64_t> CompletedAt100ns;
};

/// Exact registration captured when a USB permission event is armed. A later
/// cancellation, replacement IRP or START cannot change this observation.
struct DriverUsbIdleMemberResult {
  std::string DeviceID;
  uint64_t PDO = 0;
  uint64_t IRP = 0;
  uint64_t StartEpoch = 0;
};

} // namespace neverd::emulation

#endif // NEVERD_EMULATION_DRIVERUSBIDLE_H
