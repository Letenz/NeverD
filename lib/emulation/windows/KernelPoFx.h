//===- KernelPoFx.h - Component power ownership and callback protocol ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Define the typed PoFx registration, state and callback ownership interface.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_WINDOWS_KERNELPOFX_H
#define NEVERD_EMULATION_WINDOWS_KERNELPOFX_H

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/Support/Error.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace neverd::emulation {
namespace pofx {
#define NEVERD_KERNEL_POFX_VALUE(Name, Value)                                  \
  inline constexpr uint64_t Name = Value;
#include "KernelPoFxValues.def"
#undef NEVERD_KERNEL_POFX_VALUE
} // namespace pofx

/// Own the PoFx v1 protocol, independently of guest storage, IRQL and D-state
/// packet execution. The bridge validates those boundaries. Idle Fx and device
/// power-down decisions are explicit inputs; this model does not infer Windows
/// power-policy heuristics. Registration copies all component descriptions.
class KernelPoFx {
public:
  struct Limits {
    uint64_t MaxDevices = pofx::DefaultMaxDevices;
    uint64_t MaxComponents = pofx::DefaultMaxComponents;
    uint64_t MaxIdleStates = pofx::DefaultMaxIdleStates;
    uint64_t MaxCallbacks = pofx::DefaultMaxCallbacks;
  };
  struct IdleState {
    uint64_t TransitionLatency = 0;
    uint64_t ResidencyRequirement = 0;
    uint32_t NominalPower = pofx::UnknownPower;
    bool operator==(const IdleState &) const = default;
  };
  struct Component {
    std::array<uint8_t, pofx::ComponentIDSize> ID{};
    uint32_t DeepestWakeableState = 0;
    std::vector<IdleState> IdleStates;
  };
  struct Callbacks {
    uint64_t ActiveCondition = 0;
    uint64_t IdleCondition = 0;
    uint64_t IdleState = 0;
    uint64_t DevicePowerRequired = 0;
    uint64_t DevicePowerNotRequired = 0;
    uint64_t PowerControl = 0;
  };
  struct Registration {
    uint64_t PDO = 0;
    uint64_t Context = 0;
    uint32_t Version = pofx::Version1;
    Callbacks Routines;
    std::vector<Component> Components;
    /// Framework-owned registration uses typed events instead of guest PCs.
    /// Zero guest callbacks otherwise mean the documented optional callback.
    bool InternalCallbacks = false;
  };
  enum class CallbackKind {
    ActiveCondition,
    IdleCondition,
    IdleState,
    DevicePowerRequired,
    DevicePowerNotRequired
  };
  struct Callback {
    uint64_t Token = 0;
    uint64_t Handle = 0;
    uint64_t PDO = 0;
    uint64_t PC = 0;
    /// Nonzero for a blocking operation's same-thread callback continuation.
    uint64_t Thread = 0;
    CallbackKind Kind = CallbackKind::ActiveCondition;
    uint32_t Component = 0;
    uint32_t State = 0;
    bool Internal = false;
    std::vector<uint64_t> Arguments;
  };
  struct ComponentSnapshot {
    uint64_t References = 0;
    uint64_t Latency = pofx::UnknownTime;
    uint64_t Residency = pofx::UnknownTime;
    uint32_t IdleState = 0;
    bool Wake = false;
    bool Active = true;
    bool TransitionPending = false;
    uint64_t ActiveGeneration = 0;
    uint64_t IdleGeneration = 0;
  };

  KernelPoFx() = default;
  explicit KernelPoFx(Limits Bounds) : Bounds(Bounds) {}
  llvm::Error canRegisterDevice(uint64_t Handle,
                                const Registration &Description) const;
  llvm::Error registerDevice(uint64_t Handle, Registration Description);
  const Registration *registration(uint64_t Handle) const;
  std::optional<uint64_t> handleForPDO(uint64_t PDO) const;
  llvm::Error start(uint64_t Handle);
  llvm::Error activate(uint64_t Handle, uint32_t Component,
                       uint64_t CallbackThread = 0);
  llvm::Error idle(uint64_t Handle, uint32_t Component,
                   uint64_t CallbackThread = 0);
  llvm::Error completeIdleCondition(uint64_t Handle, uint32_t Component);
  llvm::Error completeIdleState(uint64_t Handle, uint32_t Component);
  llvm::Error completeDevicePowerNotRequired(uint64_t Handle);
  llvm::Error reportDevicePoweredOn(uint64_t Handle);
  llvm::Error setLatency(uint64_t Handle, uint32_t Component, uint64_t Latency);
  llvm::Error setResidency(uint64_t Handle, uint32_t Component,
                           uint64_t Residency);
  llvm::Error setWake(uint64_t Handle, uint32_t Component, bool Wake);
  llvm::Error setDeviceIdleTimeout(uint64_t Handle, uint64_t Timeout100ns);

  /// Explicit decisions obey the current copied capability and hint values.
  /// A component state transition requires the completed idle condition.
  llvm::Error requestIdleState(uint64_t Handle, uint32_t Component,
                               uint32_t State);
  llvm::Error requestDevicePowerNotRequired(uint64_t Handle);
  llvm::Error process(uint64_t Now100ns);
  std::optional<uint64_t> nextDeadline() const;

  /// Admission and actual guest entry are distinct. A completion API cannot
  /// acknowledge a callback that has only been submitted to the scheduler.
  /// An acknowledged callback retains registration ownership until return.
  std::optional<Callback> nextCallback() const;
  const Callback *callback(uint64_t Token) const;
  llvm::Error submitCallback(uint64_t Token);
  llvm::Error beginCallback(uint64_t Token);
  llvm::Error finishCallback(uint64_t Token);
  bool hasPendingCallbacks() const;
  llvm::Expected<ComponentSnapshot> component(uint64_t Handle,
                                              uint32_t Component) const;
  llvm::Expected<bool> conditionReached(uint64_t Handle, uint32_t Component,
                                        bool Active) const;
  llvm::Error canUnregisterDevice(uint64_t Handle) const;
  llvm::Error unregisterDevice(uint64_t Handle);
  llvm::Error canReleasePDO(uint64_t PDO) const;

private:
  struct ComponentState : ComponentSnapshot {
    std::optional<uint64_t> CallbackToken;
    std::optional<uint32_t> RequestedIdleState;
    uint64_t CallbackThread = 0;
    uint64_t BlockingActivationThread = 0;
  };
  struct CallbackState {
    Callback Call;
    bool Submitted = false;
    bool Begun = false;
    bool Returned = false;
    bool Completed = false;
  };
  struct Device {
    Registration Description;
    std::vector<ComponentState> Components;
    std::map<uint64_t, CallbackState> Callbacks;
    bool Started = false;
    bool PowerRequired = true;
    std::optional<uint64_t> PowerCallback;
    uint64_t IdleTimeout = 0;
    std::optional<uint64_t> IdleSince;
    std::optional<uint64_t> PowerDownDeadline;
  };
  llvm::Error mutate(uint64_t Handle,
                     llvm::function_ref<llvm::Error(Device &)> Change);
  llvm::Error reconcile(uint64_t Handle, Device &D, uint64_t &Next);
  llvm::Error enqueue(uint64_t Handle, Device &D, CallbackKind Kind,
                      uint32_t Component, uint32_t State, uint64_t Thread,
                      uint64_t &Next);
  llvm::Error complete(uint64_t Handle, uint32_t Component, CallbackKind Kind);
  llvm::Error completePower(uint64_t Handle, CallbackKind Kind);
  llvm::Error retireCallback(Device &D, uint64_t Token);
  llvm::Error completeCondition(ComponentState &C, bool Active);
  llvm::Error validateIdleState(const Device &D, uint32_t Component,
                                uint32_t State) const;
  llvm::Expected<uint64_t> callbackOwner(uint64_t Token) const;
  bool allIdle(const Device &D) const;
  uint64_t callbackCount() const;

  Limits Bounds;
  uint64_t Now = 0;
  uint64_t NextToken = 1;
  std::map<uint64_t, Device> Devices;
};
} // namespace neverd::emulation
#endif
