//===- ExecutionConfiguration.cpp - CPU profile validation --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/emulation/ExecutionConfiguration.h"

#include "../arch/aarch64/AArch64Machine.h"
#include "../arch/x86_64/X64Machine.h"
#include "../core/ExecutionDiagnostics.h"
#include "HostEnvironment.h"

#include "neverd/emulation/CPU.h"

#include "llvm/Support/ErrorHandling.h"

#include <limits>

namespace neverd::emulation {
namespace {
using Feature = ExecutionFeature;
#define NEVERD_EXECUTION_FEATURE_GROUP(Name, Features)                         \
  constexpr Feature Name = Features;
#include "ExecutionProfiles.def"
#undef NEVERD_EXECUTION_FEATURE_GROUP

const char *const X64Instructions[] = {
#define NEVERD_CHECKED_X64_INSTRUCTION(Name) #Name,
#include "../arch/x86_64/CheckedX64Instructions.def"
#undef NEVERD_CHECKED_X64_INSTRUCTION
};
const char *const ARMInstructions[] = {
#define NEVERD_AARCH64_INSTRUCTION(Name, Kind) #Name,
#include "../arch/aarch64/CheckedAArch64Instructions.def"
#undef NEVERD_AARCH64_INSTRUCTION
};
const char *const X64UserInstructions[] = {
#define NEVERD_CHECKED_X64_INSTRUCTION(Name) #Name,
#include "../arch/x86_64/CheckedX64Instructions.def"
#undef NEVERD_CHECKED_X64_INSTRUCTION
#define NEVERD_X64_SERVICE(Kind, Name, ...) #Name,
#include "../arch/x86_64/X64ServiceInstructions.def"
#undef NEVERD_X64_SERVICE
};
const char *const ARMUserInstructions[] = {
#define NEVERD_AARCH64_INSTRUCTION(Name, Kind) #Name,
#include "../arch/aarch64/CheckedAArch64Instructions.def"
#undef NEVERD_AARCH64_INSTRUCTION
#define NEVERD_AARCH64_SERVICE(Kind, Name, Mask, Value, Shift, ImmediateMask)  \
  #Name,
#include "../arch/aarch64/AArch64ServiceInstructions.def"
#undef NEVERD_AARCH64_SERVICE
};

ExecutionCapabilities profile(ExecutionContract Contract, GuestArchitecture ISA,
                              ExecutionPrivilege Privilege,
                              ExecutionAddressModel Model, Feature Features,
                              bool Native, bool Allowlist) {
  const bool Checked = Model != ExecutionAddressModel::Flat;
  const bool User = Privilege == ExecutionPrivilege::User;
  const unsigned Bits =
      !Checked ? std::numeric_limits<uint64_t>::digits
      : ISA == GuestArchitecture::X64
          ? x64::PageBits + x64::TableBits * x64::TableLevels
          : aarch64::PageBits + aarch64::TableBits * aarch64::TableLevels;
  const llvm::ArrayRef<const char *> Instructions =
      !Allowlist ? llvm::ArrayRef<const char *>()
      : ISA == GuestArchitecture::X64
          ? (User ? llvm::ArrayRef(X64UserInstructions)
                  : llvm::ArrayRef(X64Instructions))
          : (User ? llvm::ArrayRef(ARMUserInstructions)
                  : llvm::ArrayRef(ARMInstructions));
  return {Contract,
          ISA,
          Privilege,
          Model,
          Bits,
          memory::PageSize,
          memory::MaxRAM,
          memory::MaxRAM,
          Features,
          Native,
          Allowlist,
          Instructions,
          Checked ? ExecutionMemoryObservation::InstructionPreflight
                  : ExecutionMemoryObservation::EngineCallbacks,
          Checked ? ExecutionControlPrecision::InstructionBoundary
                  : ExecutionControlPrecision::EngineRequest,
          false};
}
} // namespace

llvm::Expected<ExecutionCapabilities>
executionCapabilities(ExecutionContract Contract,
                      GuestArchitecture Architecture) {
  if (Architecture != GuestArchitecture::X64 &&
      Architecture != GuestArchitecture::AArch64)
    return diagnostic::error(diagnostic::Architecture);
#define NEVERD_EXECUTION_PROFILE(Name, ContractID, ISA, Privilege, Model,      \
                                 Features, Native, Allowlist)                  \
  if (Contract == ExecutionContract::ContractID &&                             \
      Architecture == GuestArchitecture::ISA)                                  \
    return profile(Contract, Architecture, ExecutionPrivilege::Privilege,      \
                   ExecutionAddressModel::Model, Features, Native, Allowlist);
#include "ExecutionProfiles.def"
#undef NEVERD_EXECUTION_PROFILE
  return diagnostic::error(diagnostic::Contract);
}

llvm::Expected<ResolvedExecutionConfiguration>
resolveExecutionConfiguration(const ExecutionConfiguration &Configuration) {
  auto Capabilities =
      executionCapabilities(Configuration.Contract, Configuration.Architecture);
  if (!Capabilities)
    return Capabilities.takeError();
  if (Configuration.Privilege &&
      *Configuration.Privilege != Capabilities->Privilege)
    return diagnostic::error(diagnostic::ConfigurationPrivilege);
  if (Configuration.VirtualAddressBits &&
      *Configuration.VirtualAddressBits != Capabilities->VirtualAddressBits)
    return diagnostic::error(diagnostic::ConfigurationAddressWidth);
  if (Configuration.PageSize &&
      *Configuration.PageSize != Capabilities->PageSize)
    return diagnostic::error(diagnostic::ConfigurationPageSize);
  if (!Capabilities->supports(Configuration.RequiredFeatures))
    return diagnostic::error(diagnostic::ConfigurationFeatures);
  auto Resolved = Configuration;
  Resolved.Privilege = Capabilities->Privilege;
  Resolved.VirtualAddressBits = Capabilities->VirtualAddressBits;
  Resolved.PageSize = Capabilities->PageSize;
  std::string Reason = execution::ExplicitSelection;
  if (Resolved.Backend == ExecutionBackendKind::Auto) {
    if (!Capabilities->SupportsNativeExecution) {
      Resolved.Backend = ExecutionBackendKind::Unicorn;
      Reason = Resolved.Contract == ExecutionContract::Legacy
                   ? execution::LegacySelection
                   : execution::PortableSelection;
    } else if (!runtime::matchesHost(Resolved.Architecture)) {
      Resolved.Backend = ExecutionBackendKind::Unicorn;
      Reason = execution::CrossISASelection;
    } else {
      Resolved.Backend = runtime::nativeBackend();
      Reason = Resolved.Backend == ExecutionBackendKind::Unicorn
                   ? execution::PortableSelection
                   : execution::HostSelection;
    }
  }
  if (Resolved.Backend != ExecutionBackendKind::Unicorn &&
      Resolved.Backend != ExecutionBackendKind::KVM &&
      Resolved.Backend != ExecutionBackendKind::WHP)
    return diagnostic::error(diagnostic::BackendName);
  if (Resolved.Backend != ExecutionBackendKind::Unicorn &&
      !Capabilities->SupportsNativeExecution)
    return diagnostic::error(diagnostic::Contract);
  return ResolvedExecutionConfiguration{Resolved, *Capabilities,
                                        std::move(Reason)};
}

const char *executionPrivilegeName(ExecutionPrivilege Privilege) {
  switch (Privilege) {
#define NEVERD_EXECUTION_PRIVILEGE(Name, Text)                                 \
  case ExecutionPrivilege::Name:                                               \
    return Text;
#include "neverd/emulation/ExecutionConfiguration.def"
#undef NEVERD_EXECUTION_PRIVILEGE
  }
  return diagnostic::UnknownPrivilege;
}
const char *executionAddressModelName(ExecutionAddressModel Model) {
  switch (Model) {
#define NEVERD_EXECUTION_ADDRESS_MODEL(Name, Text)                             \
  case ExecutionAddressModel::Name:                                            \
    return Text;
#include "neverd/emulation/ExecutionConfiguration.def"
#undef NEVERD_EXECUTION_ADDRESS_MODEL
  }
  return diagnostic::UnknownAddressModel;
}
const char *executionFeatureName(ExecutionFeature Feature) {
  switch (Feature) {
#define NEVERD_EXECUTION_FEATURE(Name, Bit, Text)                              \
  case ExecutionFeature::Name:                                                 \
    return Text;
#include "neverd/emulation/ExecutionConfiguration.def"
#undef NEVERD_EXECUTION_FEATURE
  default:
    return diagnostic::UnknownFeature;
  }
}
const char *backendAvailabilityName(BackendAvailability Availability) {
  switch (Availability) {
#define NEVERD_EXECUTION_AVAILABILITY(Name, Text)                              \
  case BackendAvailability::Name:                                              \
    return Text;
#include "neverd/emulation/ExecutionBackend.def"
#undef NEVERD_EXECUTION_AVAILABILITY
  }
  return diagnostic::UnknownAvailability;
}

const char *executionMemoryObservationName(ExecutionMemoryObservation Mode) {
  switch (Mode) {
#define NEVERD_EXECUTION_MEMORY_OBSERVATION(Name, Text)                        \
  case ExecutionMemoryObservation::Name:                                       \
    return Text;
#include "neverd/emulation/ExecutionConfiguration.def"
#undef NEVERD_EXECUTION_MEMORY_OBSERVATION
  }
  llvm_unreachable(diagnostic::UnknownObservation);
}
const char *executionControlPrecisionName(ExecutionControlPrecision Precision) {
  switch (Precision) {
#define NEVERD_EXECUTION_CONTROL_PRECISION(Name, Text)                         \
  case ExecutionControlPrecision::Name:                                        \
    return Text;
#include "neverd/emulation/ExecutionConfiguration.def"
#undef NEVERD_EXECUTION_CONTROL_PRECISION
  }
  llvm_unreachable(diagnostic::UnknownControlPrecision);
}

llvm::Expected<ExecutionBackendProbe>
probeExecutionBackend(const ExecutionConfiguration &Configuration) {
  auto Resolved = resolveExecutionConfiguration(Configuration);
  if (!Resolved)
    return Resolved.takeError();
  auto Result =
      createExecutionBackend(Configuration, Resolved->Capabilities.PageSize);
  BackendAvailability Availability = BackendAvailability::Available;
  std::string Reason = diagnostic::ProbeReady;
  if (!Result) {
    Availability = BackendAvailability::InitializationFailed;
    auto E = llvm::handleErrors(
        Result.takeError(),
        [&](const BackendUnavailableError &Unavailable) -> llvm::Error {
          Availability = Unavailable.availability();
          return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                         Unavailable.reason());
        });
    Reason = llvm::toString(std::move(E));
  }
  return ExecutionBackendProbe{std::move(*Resolved), Availability,
                               std::move(Reason)};
}
} // namespace neverd::emulation
