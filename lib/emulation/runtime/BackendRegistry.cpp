//===- BackendRegistry.cpp - Backend selection---------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../arch/aarch64/CheckedAArch64Backend.h"
#include "../arch/x86_64/CheckedX64Backend.h"
#include "../backends/MachineFactories.h"

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
namespace {
bool matchesHost(GuestArchitecture Architecture) {
#if defined(__aarch64__) || defined(_M_ARM64)
  return Architecture == GuestArchitecture::AArch64;
#elif defined(__x86_64__) || defined(_M_X64)
  return Architecture == GuestArchitecture::X64;
#else
  return false;
#endif
}
ExecutionBackendKind nativeBackend() {
#if defined(__linux__)
  return ExecutionBackendKind::KVM;
#elif defined(_WIN32)
  return ExecutionBackendKind::WHP;
#else
  return ExecutionBackendKind::Unicorn;
#endif
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
createExecutionBackend(ExecutionBackendKind Kind, ExecutionContract Contract,
                       std::shared_ptr<AddressSpace> Space,
                       GuestArchitecture Architecture) {
  if (Architecture != GuestArchitecture::X64 &&
      Architecture != GuestArchitecture::AArch64)
    return diagnostic::error(diagnostic::Architecture);
  if ((Contract == ExecutionContract::Legacy &&
       Architecture != GuestArchitecture::X64) ||
      (Contract == ExecutionContract::CheckedX64 &&
       Architecture != GuestArchitecture::X64) ||
      (Contract == ExecutionContract::CheckedAArch64 &&
       Architecture != GuestArchitecture::AArch64))
    return diagnostic::error(diagnostic::Contract);
  const bool Checked = Contract == ExecutionContract::CheckedX64 ||
                       Contract == ExecutionContract::CheckedAArch64;
  if (!Checked && Contract != ExecutionContract::Legacy &&
      Contract != ExecutionContract::Software)
    return diagnostic::error(diagnostic::Contract);
  std::string Reason = execution::ExplicitSelection;
  if (Kind == ExecutionBackendKind::Auto) {
    if (!Checked) {
      Kind = ExecutionBackendKind::Unicorn;
      Reason = Contract == ExecutionContract::Legacy
                   ? execution::LegacySelection
                   : execution::PortableSelection;
    } else if (!matchesHost(Architecture)) {
      Kind = ExecutionBackendKind::Unicorn;
      Reason = execution::CrossISASelection;
    } else {
      Kind = nativeBackend();
      Reason = Kind == ExecutionBackendKind::Unicorn
                   ? execution::PortableSelection
                   : execution::HostSelection;
    }
  }
  if (Kind != ExecutionBackendKind::Unicorn &&
      Kind != ExecutionBackendKind::KVM && Kind != ExecutionBackendKind::WHP)
    return diagnostic::error(diagnostic::BackendName);
  if (Kind != ExecutionBackendKind::Unicorn) {
    if (!Checked)
      return diagnostic::error(diagnostic::Contract);
    if (!matchesHost(Architecture))
      return diagnostic::unavailable(diagnostic::HostISA);
  }
  if (!Checked) {
#ifdef NEVERD_EMULATION_UNICORN
    auto CPU = UnicornBackend::create(std::move(Space), Architecture);
    if (!CPU)
      return CPU.takeError();
    return BackendSelection{std::move(*CPU), Kind, std::move(Reason)};
#else
    return diagnostic::unavailable(diagnostic::UnicornDisabled);
#endif
  }
  auto CPU = createCheckedBackend(Kind, std::move(Space), Architecture);
  if (!CPU)
    return CPU.takeError();
  return BackendSelection{std::move(*CPU), Kind, std::move(Reason)};
}
llvm::Expected<BackendSelection>
createExecutionBackend(ExecutionBackendKind Kind, ExecutionContract Contract,
                       uint64_t Limit, GuestArchitecture Architecture) {
  auto RAM = PhysicalMemory::create(Limit);
  if (!RAM)
    return RAM.takeError();
  auto Space = AddressSpace::create(std::move(*RAM), Limit);
  if (!Space)
    return Space.takeError();
  return createExecutionBackend(Kind, Contract, std::move(*Space),
                                Architecture);
}
} // namespace neverd::emulation
