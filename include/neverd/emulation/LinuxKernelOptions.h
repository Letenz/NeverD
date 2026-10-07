//===- LinuxKernelOptions.h - Explicit Linux kernel contracts ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_LINUXKERNELOPTIONS_H
#define NEVERD_EMULATION_LINUXKERNELOPTIONS_H
#include <optional>
#include <set>
namespace neverd::emulation {
enum class AndroidGKIKernel {
#define NEVERD_LINUX_GKI_KERNEL(Name, Label, Flags) Name,
#include "neverd/emulation/LinuxGKIKernels.def"
#undef NEVERD_LINUX_GKI_KERNEL
};
enum class LinuxUnavailableSyscall {
#define NEVERD_LINUX_UNAVAILABLE_SYSCALL(Name, Label, X64, ARM, Count) Name,
#include "neverd/emulation/LinuxUnavailableSyscalls.def"
#undef NEVERD_LINUX_UNAVAILABLE_SYSCALL
};
/// Explicit released GKI contracts and observed absent kernel interfaces.
/// Unmodeled interfaces retain their unsupported execution boundary.
struct LinuxKernelOptions {
  std::set<LinuxUnavailableSyscall> UnavailableSyscalls;
  /// Explicit released Android common kernel contract. Android API levels
  /// alone do not select a kernel. Only implemented services use this value.
  std::optional<AndroidGKIKernel> GKI;
};
} // namespace neverd::emulation
#endif
