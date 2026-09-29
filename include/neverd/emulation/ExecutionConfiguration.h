//===- ExecutionConfiguration.h - Validated CPU contracts and capabilities ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_EXECUTIONCONFIGURATION_H
#define NEVERD_EMULATION_EXECUTIONCONFIGURATION_H

#include "neverd/emulation/ExecutionBackend.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/BitmaskEnum.h"

#include <cstdint>
#include <optional>

namespace neverd::emulation {
LLVM_ENABLE_BITMASK_ENUMS_IN_NAMESPACE();

/// Flat execution has no architectural user/supervisor isolation contract.
enum class ExecutionPrivilege {
#define NEVERD_EXECUTION_PRIVILEGE(Name, Text) Name,
#include "neverd/emulation/ExecutionConfiguration.def"
#undef NEVERD_EXECUTION_PRIVILEGE
};
enum class ExecutionAddressModel {
#define NEVERD_EXECUTION_ADDRESS_MODEL(Name, Text) Name,
#include "neverd/emulation/ExecutionConfiguration.def"
#undef NEVERD_EXECUTION_ADDRESS_MODEL
};
enum class ExecutionMemoryObservation {
#define NEVERD_EXECUTION_MEMORY_OBSERVATION(Name, Text) Name,
#include "neverd/emulation/ExecutionConfiguration.def"
#undef NEVERD_EXECUTION_MEMORY_OBSERVATION
};
enum class ExecutionControlPrecision {
#define NEVERD_EXECUTION_CONTROL_PRECISION(Name, Text) Name,
#include "neverd/emulation/ExecutionConfiguration.def"
#undef NEVERD_EXECUTION_CONTROL_PRECISION
};
enum class ExecutionFeature : uint64_t {
  None = 0,
#define NEVERD_EXECUTION_FEATURE(Name, Bit, Text) Name = uint64_t(1) << Bit,
#define NEVERD_EXECUTION_FEATURE_LIMIT(Name) LLVM_MARK_AS_BITMASK_ENUM(Name)
#include "neverd/emulation/ExecutionConfiguration.def"
#undef NEVERD_EXECUTION_FEATURE_LIMIT
#undef NEVERD_EXECUTION_FEATURE
};

/// Requirements are checked before allocating or attaching any CPU transport.
/// Omitted privilege/address/page fields select the contract's fixed profile;
/// explicit unsupported values fail instead of being silently substituted.
struct ExecutionConfiguration {
  ExecutionBackendKind Backend = ExecutionBackendKind::Auto;
  ExecutionContract Contract = ExecutionContract::Software;
  GuestArchitecture Architecture = GuestArchitecture::X64;
  std::optional<ExecutionPrivilege> Privilege;
  std::optional<unsigned> VirtualAddressBits;
  std::optional<uint64_t> PageSize;
  ExecutionFeature RequiredFeatures = ExecutionFeature::None;
};

/// Static semantic support, independent of build options and host availability.
/// Instruction families are only an admission inventory: operand forms and
/// instruction encodings are still validated by the selected contract.
struct ExecutionCapabilities {
  ExecutionContract Contract;
  GuestArchitecture Architecture;
  ExecutionPrivilege Privilege;
  ExecutionAddressModel AddressModel;
  unsigned VirtualAddressBits;
  uint64_t PageSize, MaxPhysicalBytes, MaxMappedBytes;
  ExecutionFeature Features;
  bool SupportsNativeExecution;
  bool HasInstructionAllowlist;
  llvm::ArrayRef<const char *> InstructionFamilies;
  /// Memory callbacks may observe a split access or partially completed
  /// instruction. Only the checked contracts preflight every admitted effect.
  ExecutionMemoryObservation MemoryObservation;
  /// Cooperative stop/deadline behavior is not a hard wall-clock bound: guest
  /// admission, native entry, and caller callbacks have distinct lifetimes.
  ExecutionControlPrecision ControlPrecision;
  bool HardWallClockBound;
  bool supports(ExecutionFeature Required) const {
    const auto Bits = static_cast<uint64_t>(Required);
    return (static_cast<uint64_t>(Features) & Bits) == Bits;
  }
};

struct ResolvedExecutionConfiguration {
  ExecutionConfiguration Configuration;
  ExecutionCapabilities Capabilities;
  std::string SelectionReason;
};

/// A live construction probe uses private temporary RAM and no caller mappings.
/// Availability establishes transport initialization, not workload
/// compatibility or native execution coverage. Host state can change after the
/// probe returns.
struct ExecutionBackendProbe {
  ResolvedExecutionConfiguration Resolved;
  BackendAvailability Availability;
  std::string Reason;
};

/// Build and compiled host ABI compatibility only; no device or host API calls.
struct ExecutionBackendBuild {
  BackendAvailability Availability;
  std::string Reason;
};

llvm::Expected<ExecutionBackendBuild>
queryExecutionBackendBuild(ExecutionBackendKind Backend,
                           GuestArchitecture Architecture);

llvm::Expected<ExecutionCapabilities>
executionCapabilities(ExecutionContract Contract,
                      GuestArchitecture Architecture);
llvm::Expected<ResolvedExecutionConfiguration>
resolveExecutionConfiguration(const ExecutionConfiguration &Configuration);
llvm::Expected<ExecutionBackendProbe>
probeExecutionBackend(const ExecutionConfiguration &Configuration);

const char *executionPrivilegeName(ExecutionPrivilege Privilege);
const char *executionAddressModelName(ExecutionAddressModel Model);
const char *executionFeatureName(ExecutionFeature Feature);
const char *backendAvailabilityName(BackendAvailability Availability);
const char *executionMemoryObservationName(ExecutionMemoryObservation Mode);
const char *executionControlPrecisionName(ExecutionControlPrecision Precision);
} // namespace neverd::emulation
#endif
