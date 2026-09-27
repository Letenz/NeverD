//===- DriverPowerPolicy.h - Idle and wake observations -----------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_DRIVERPOWERPOLICY_H
#define NEVERD_EMULATION_DRIVERPOWERPOLICY_H
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
namespace neverd::emulation {
enum class DriverPowerPolicyAction {
#define NEVERD_POWER_POLICY_ACTION(Name, Spelling) Name,
#include "neverd/emulation/DriverPowerPolicy.def"
#undef NEVERD_POWER_POLICY_ACTION
};
inline bool isPoFxPowerPolicyAction(DriverPowerPolicyAction Action) {
  return Action == DriverPowerPolicyAction::ComponentIdleState ||
         Action == DriverPowerPolicyAction::PowerNotRequired;
}
struct DriverWakeCapabilities {
  /// Explicit provider support for wake from D3hot, in S0 and Sleeping3.
  bool S0 = false;
  bool Sx = false;
};
struct DriverPowerPolicyEvent {
  uint64_t After100ns = 0;
  std::string DeviceID;
  DriverPowerPolicyAction Action = DriverPowerPolicyAction::Idle;
  std::optional<uint32_t> Component;
  std::optional<uint32_t> State;
};
struct DriverPowerPolicyResult {
  uint32_t SourceRequestIndex = 0, EventIndex = 0;
  std::string DeviceID;
  DriverPowerPolicyAction Action = DriverPowerPolicyAction::Idle;
  uint64_t DueAt100ns = 0;
  std::optional<uint64_t> OccurredAt100ns;
  uint64_t DeviceEpoch = 0;
  std::optional<uint32_t> Component;
  std::optional<uint32_t> State;
};
inline constexpr size_t DriverPowerPolicyEventLimit = 1024;
} // namespace neverd::emulation
#endif
