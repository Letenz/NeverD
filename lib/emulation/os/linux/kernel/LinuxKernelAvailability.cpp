//===- LinuxKernelAvailability.cpp - Explicit kernel absence --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxKernelAvailability.h"
namespace neverd::emulation::linux_model {
std::optional<uint32_t> gkiPidFDFlags(AndroidGKIKernel Kernel) {
  switch (Kernel) {
#define NEVERD_LINUX_GKI_KERNEL(Name, Label, Flags, Import, NonLeader,         \
                                NameFirst)                                     \
  case AndroidGKIKernel::Name:                                                 \
    return Flags;
#include "neverd/emulation/LinuxGKIKernels.def"
#undef NEVERD_LINUX_GKI_KERNEL
  }
  return std::nullopt;
}
std::optional<IOVectorImportKind> gkiIOVectorImport(AndroidGKIKernel Kernel) {
  switch (Kernel) {
#define NEVERD_LINUX_GKI_KERNEL(Name, Label, Flags, Import, NonLeader,         \
                                NameFirst)                                     \
  case AndroidGKIKernel::Name:                                                 \
    return IOVectorImportKind::Import;
#include "neverd/emulation/LinuxGKIKernels.def"
#undef NEVERD_LINUX_GKI_KERNEL
  }
  return std::nullopt;
}
std::optional<uint32_t> gkiPidFDNonLeaderError(AndroidGKIKernel Kernel) {
  switch (Kernel) {
#define NEVERD_LINUX_GKI_KERNEL(Name, Label, Flags, Import, NonLeader,         \
                                NameFirst)                                     \
  case AndroidGKIKernel::Name:                                                 \
    return NonLeader;
#include "neverd/emulation/LinuxGKIKernels.def"
#undef NEVERD_LINUX_GKI_KERNEL
  }
  return std::nullopt;
}
std::optional<bool> gkiXAttrNameFirst(AndroidGKIKernel Kernel) {
  switch (Kernel) {
#define NEVERD_LINUX_GKI_KERNEL(Name, Label, Flags, Import, NonLeader,         \
                                NameFirst)                                     \
  case AndroidGKIKernel::Name:                                                 \
    return NameFirst;
#include "neverd/emulation/LinuxGKIKernels.def"
#undef NEVERD_LINUX_GKI_KERNEL
  }
  return std::nullopt;
}
llvm::Error validateKernelOptions(const LinuxKernelOptions &Options) {
  if (Options.GKI &&
      (!gkiPidFDFlags(*Options.GKI) || Options.UnavailableSyscalls.contains(
                                           LinuxUnavailableSyscall::PidFDOpen)))
    return failure(KernelOptions);
  if (Options.Tasks) {
    if (!Options.GKI || Options.Tasks->size() > KernelTaskLimit)
      return failure(KernelOptions);
    for (const auto &[ID, Task] : *Options.Tasks)
      if (!ID || ID > INT32_MAX || (ID == ProcessID && !Task.GroupLeader))
        return failure(KernelOptions);
  }
  for (auto Call : Options.UnavailableSyscalls) {
    switch (Call) {
#define NEVERD_LINUX_UNAVAILABLE_SYSCALL(Name, Label, X64, ARM, Count)         \
  case LinuxUnavailableSyscall::Name:                                          \
    break;
#include "neverd/emulation/LinuxUnavailableSyscalls.def"
#undef NEVERD_LINUX_UNAVAILABLE_SYSCALL
    default:
      return failure(KernelOptions);
    }
  }
  return llvm::Error::success();
}
llvm::Error validateKernelTaskInputs(const ProcessOptions &Options) {
  if (!Options.LinuxKernel || !Options.LinuxKernel->Tasks)
    return llvm::Error::success();
  // A fixed observation cannot describe later guest thread creation or exit.
  if (Options.Android && Options.Android->ThreadLimit > 1)
    return failure(KernelOptions);
  if (Options.LinuxPriority)
    for (const auto &[ID, Nice] : Options.LinuxPriority->Tasks)
      if (ID != ProcessID && !Options.LinuxKernel->Tasks->contains(ID))
        return failure(KernelOptions);
  return llvm::Error::success();
}
bool unavailableKernelService(
    ServiceKind Kind, const std::optional<LinuxKernelOptions> &Options) {
  if (!Options)
    return false;
  switch (Kind) {
#define NEVERD_LINUX_UNAVAILABLE_SYSCALL(Name, Label, X64, ARM, Count)         \
  case ServiceKind::Name:                                                      \
    return Options->UnavailableSyscalls.contains(LinuxUnavailableSyscall::Name);
#include "neverd/emulation/LinuxUnavailableSyscalls.def"
#undef NEVERD_LINUX_UNAVAILABLE_SYSCALL
  default:
    return false;
  }
}
} // namespace neverd::emulation::linux_model
