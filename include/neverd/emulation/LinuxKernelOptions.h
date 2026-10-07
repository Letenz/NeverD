//===- LinuxKernelOptions.h - Explicit absent kernel calls ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_LINUXKERNELOPTIONS_H
#define NEVERD_EMULATION_LINUXKERNELOPTIONS_H
#include <set>
namespace neverd::emulation {
enum class LinuxUnavailableSyscall {
#define NEVERD_LINUX_UNAVAILABLE_SYSCALL(Name, Label, X64, ARM, Count) Name,
#include "neverd/emulation/LinuxUnavailableSyscalls.def"
#undef NEVERD_LINUX_UNAVAILABLE_SYSCALL
};
/// Explicit fixture observations that a selected kernel interface is absent.
/// An unlisted interface keeps its existing supported or unsupported behavior.
struct LinuxKernelOptions {
  std::set<LinuxUnavailableSyscall> UnavailableSyscalls;
};
} // namespace neverd::emulation
#endif
