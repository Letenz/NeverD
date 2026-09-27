//===- KernelPowerPolicy.h - Explicit KMDF idle and wake policy -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_WINDOWS_KERNELPOWERPOLICY_H
#define NEVERD_EMULATION_WINDOWS_KERNELPOWERPOLICY_H

#include "neverd/emulation/DriverPnp.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace neverd::emulation {
namespace power_policy {
#define NEVERD_POWER_POLICY_VALUE(Name, Value)                                 \
  inline constexpr uint64_t Name = Value;
#include "KernelPowerPolicyValues.def"
#undef NEVERD_POWER_POLICY_VALUE
} // namespace power_policy

struct KernelPowerPolicy {
  struct Callbacks {
#define NEVERD_POWER_POLICY_CALLBACK(Name, Index, Result) uint64_t Name = 0;
#include "KernelPowerPolicyCallbacks.def"
#undef NEVERD_POWER_POLICY_CALLBACK
  } Events;
  struct IdleSettings {
    DevicePowerState DxState = DevicePowerState::D3;
    bool Enabled = false;
    bool CanWake = false;
    bool PowerUpOnSystemWake = false;
    uint64_t Timeout100ns = 0;
    uint32_t TimeoutType = power_policy::DriverManagedTimeout;
    bool systemManaged() const {
      return TimeoutType != power_policy::DriverManagedTimeout;
    }
    uint32_t ExcludeD3Cold = power_policy::True;
  };
  struct WakeSettings {
    DevicePowerState DxState = DevicePowerState::D3;
    bool Enabled = false;
    bool ArmForChildren = false;
    bool PropagateParentWake = false;
  };
  struct WakeChild {
    uint64_t PDO = 0;
    uint64_t Epoch = 0;
  };
  enum class WakeSource { None, S0, Sx };
  std::optional<IdleSettings> Idle;
  std::optional<WakeSettings> Wake;
  uint64_t References = 0;
  uint64_t Epoch = 0;
  std::optional<uint64_t> IdleSince;
  std::optional<uint64_t> Deadline;
  WakeSource Armed = WakeSource::None;
  bool ArmedForDevice = false;
  std::vector<WakeChild> ArmedChildren;
  bool WakeTriggered = false;
  bool Started = false;
  bool SystemSleeping = false;
  bool IdlePowerDown = false;
  bool PowerUpRequested = false;
  bool DevicePowerPending = false;
  bool ManagedPowerNotRequired = false;
  std::optional<uint32_t> Failure;
};
} // namespace neverd::emulation
#endif
