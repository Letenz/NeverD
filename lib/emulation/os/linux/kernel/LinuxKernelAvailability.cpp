//===- LinuxKernelAvailability.cpp - Explicit kernel absence --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxKernelAvailability.h"
namespace neverd::emulation::linux_model {
llvm::Error validateKernelOptions(const LinuxKernelOptions &Options) {
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
