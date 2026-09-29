//===- BackendRegistry.cpp - Backend selection---------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../arch/aarch64/CheckedAArch64Backend.h"
#include "../arch/x86_64/CheckedX64Backend.h"
#include "../backends/MachineFactories.h"
#include "HostEnvironment.h"

#include "neverd/emulation/CPU.h"
#ifdef NEVERD_EMULATION_UNICORN
#include "../backends/unicorn/UnicornBackend.h"
#endif
#include "../core/ExecutionDiagnostics.h"

#include "llvm/Support/ErrorHandling.h"

namespace neverd::emulation {
llvm::Expected<ExecutionBackendKind>
parseExecutionBackend(llvm::StringRef Name) {
#define NEVERD_EXECUTION_BACKEND(ID, Text)                                     \
  if (Name == Text)                                                            \
    return ExecutionBackendKind::ID;
#include "neverd/emulation/ExecutionBackend.def"
#undef NEVERD_EXECUTION_BACKEND
  return diagnostic::error(diagnostic::BackendName);
}

llvm::Expected<ExecutionContract> parseExecutionContract(llvm::StringRef Name) {
#define NEVERD_EXECUTION_CONTRACT(ID, Text)                                    \
  if (Name == Text)                                                            \
    return ExecutionContract::ID;
#include "neverd/emulation/ExecutionBackend.def"
#undef NEVERD_EXECUTION_CONTRACT
  return diagnostic::error(diagnostic::ContractName);
}

const char *executionBackendName(ExecutionBackendKind Kind) {
  switch (Kind) {
#define NEVERD_EXECUTION_BACKEND(ID, Text)                                     \
  case ExecutionBackendKind::ID:                                               \
    return Text;
#include "neverd/emulation/ExecutionBackend.def"
#undef NEVERD_EXECUTION_BACKEND
  }
  return execution::Auto;
}

const char *executionContractName(ExecutionContract Contract) {
  switch (Contract) {
#define NEVERD_EXECUTION_CONTRACT(ID, Text)                                    \
  case ExecutionContract::ID:                                                  \
    return Text;
#include "neverd/emulation/ExecutionBackend.def"
#undef NEVERD_EXECUTION_CONTRACT
  }
  return execution::Legacy;
}

llvm::Expected<GuestArchitecture> parseGuestArchitecture(llvm::StringRef Name) {
#define NEVERD_GUEST_ARCHITECTURE(ID, Text)                                    \
  if (Name == Text)                                                            \
    return GuestArchitecture::ID;
#include "neverd/emulation/ExecutionBackend.def"
#undef NEVERD_GUEST_ARCHITECTURE
  return diagnostic::error(diagnostic::Architecture);
}
const char *guestArchitectureName(GuestArchitecture Architecture) {
  switch (Architecture) {
#define NEVERD_GUEST_ARCHITECTURE(ID, Text)                                    \
  case GuestArchitecture::ID:                                                  \
    return Text;
#include "neverd/emulation/ExecutionBackend.def"
#undef NEVERD_GUEST_ARCHITECTURE
  }
  llvm_unreachable(diagnostic::Architecture);
}
llvm::Expected<ExecutionBackendBuild>
queryExecutionBackendBuild(ExecutionBackendKind Kind, GuestArchitecture ISA) {
  using Availability = BackendAvailability;
  if (ISA != GuestArchitecture::X64 && ISA != GuestArchitecture::AArch64)
    return diagnostic::error(diagnostic::Architecture);
  switch (Kind) {
  case ExecutionBackendKind::Unicorn:
#ifndef NEVERD_EMULATION_UNICORN
    return ExecutionBackendBuild{Availability::BuildDisabled,
                                 diagnostic::UnicornDisabled};
#else
    return ExecutionBackendBuild{Availability::Available,
                                 diagnostic::BuildReady};
#endif
  case ExecutionBackendKind::KVM:
#if !defined(__linux__)
    return ExecutionBackendBuild{Availability::HostPlatformMismatch,
                                 diagnostic::HostPlatform};
#elif !defined(NEVERD_EMULATION_KVM)
    return ExecutionBackendBuild{Availability::BuildDisabled,
                                 diagnostic::NativeDisabled};
#endif
    break;
  case ExecutionBackendKind::WHP:
#if !defined(_WIN32)
    return ExecutionBackendBuild{Availability::HostPlatformMismatch,
                                 diagnostic::HostPlatform};
#elif !defined(NEVERD_EMULATION_WHP)
    return ExecutionBackendBuild{Availability::BuildDisabled,
                                 diagnostic::NativeDisabled};
#endif
    break;
  default:
    return diagnostic::error(diagnostic::BackendName);
  }
  if (!runtime::matchesHost(ISA))
    return ExecutionBackendBuild{Availability::HostISAMismatch,
                                 diagnostic::HostISA};
  return ExecutionBackendBuild{Availability::Available, diagnostic::BuildReady};
}
namespace {
llvm::Error checkBackendBuild(ExecutionBackendKind Kind,
                              GuestArchitecture ISA) {
  auto Build = queryExecutionBackendBuild(Kind, ISA);
  if (!Build)
    return Build.takeError();
  if (Build->Availability != BackendAvailability::Available)
    return llvm::make_error<BackendUnavailableError>(Build->Reason,
                                                     Build->Availability);
  return llvm::Error::success();
}

llvm::Expected<std::unique_ptr<ExecutionBackend>>
createCheckedBackend(ExecutionBackendKind Kind,
                     std::shared_ptr<AddressSpace> Space,
                     GuestArchitecture Architecture) {
  auto Memory = MemoryProjection::create(Space);
  if (!Memory)
    return Memory.takeError();
  auto Lock = (*Memory)->lock();
  if (!Lock)
    return Lock.takeError();
  if (auto E = (*Memory)->mutableMemory())
    return E;
  if (Architecture == GuestArchitecture::X64) {
    auto Machine =
        Kind == ExecutionBackendKind::KVM   ? createKvmMachine(**Memory)
        : Kind == ExecutionBackendKind::WHP ? createWhpMachine(**Memory)
                                            : createUnicornX64Machine(**Memory);
    if (!Machine)
      return Machine.takeError();
    return CheckedX64Backend::create(std::move(*Memory), std::move(*Machine));
  }
  auto Machine = Kind == ExecutionBackendKind::KVM
                     ? createKvmAArch64Machine(**Memory)
                 : Kind == ExecutionBackendKind::WHP
                     ? createWhpAArch64Machine(**Memory)
                     : createUnicornAArch64Machine(**Memory);
  if (!Machine)
    return Machine.takeError();
  return CheckedAArch64Backend::create(std::move(*Memory), std::move(*Machine));
}
} // namespace
llvm::Expected<BackendSelection>
createExecutionBackend(const ExecutionConfiguration &Configuration,
                       std::shared_ptr<AddressSpace> Space) {
  auto Resolved = resolveExecutionConfiguration(Configuration);
  if (!Resolved)
    return Resolved.takeError();
  const auto &Config = Resolved->Configuration;
  const auto Kind = Config.Backend;
  if (auto E = checkBackendBuild(Kind, Config.Architecture))
    return E;
  if (!Resolved->Capabilities.SupportsNativeExecution) {
#ifdef NEVERD_EMULATION_UNICORN
    auto CPU = UnicornBackend::create(std::move(Space), Config.Architecture);
    if (!CPU)
      return CPU.takeError();
    return BackendSelection{std::move(*CPU), Kind,
                            std::move(Resolved->SelectionReason)};
#else
    return diagnostic::unavailable(diagnostic::UnicornDisabled,
                                   BackendAvailability::BuildDisabled);
#endif
  }
  auto CPU = createCheckedBackend(Kind, std::move(Space), Config.Architecture);
  if (!CPU)
    return CPU.takeError();
  return BackendSelection{std::move(*CPU), Kind,
                          std::move(Resolved->SelectionReason)};
}
llvm::Expected<BackendSelection>
createExecutionBackend(const ExecutionConfiguration &Configuration,
                       uint64_t Limit) {
  auto Resolved = resolveExecutionConfiguration(Configuration);
  if (!Resolved)
    return Resolved.takeError();
  if (auto E = checkBackendBuild(Resolved->Configuration.Backend,
                                 Resolved->Configuration.Architecture))
    return E;
  auto RAM = PhysicalMemory::create(Limit);
  if (!RAM)
    return RAM.takeError();
  auto Space = AddressSpace::create(std::move(*RAM), Limit);
  if (!Space)
    return Space.takeError();
  return createExecutionBackend(Configuration, std::move(*Space));
}
llvm::Expected<BackendSelection>
createExecutionBackend(ExecutionBackendKind Kind, ExecutionContract Contract,
                       std::shared_ptr<AddressSpace> Space,
                       GuestArchitecture Architecture) {
  ExecutionConfiguration Config;
  Config.Backend = Kind;
  Config.Contract = Contract;
  Config.Architecture = Architecture;
  return createExecutionBackend(Config, std::move(Space));
}
llvm::Expected<BackendSelection>
createExecutionBackend(ExecutionBackendKind Kind, ExecutionContract Contract,
                       uint64_t Limit, GuestArchitecture Architecture) {
  ExecutionConfiguration Config;
  Config.Backend = Kind;
  Config.Contract = Contract;
  Config.Architecture = Architecture;
  return createExecutionBackend(Config, Limit);
}
} // namespace neverd::emulation
