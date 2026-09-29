//===- HostEnvironment.h - Native transport ABI selection ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_RUNTIME_HOSTENVIRONMENT_H
#define NEVERD_EMULATION_RUNTIME_HOSTENVIRONMENT_H
#include "neverd/emulation/ExecutionBackend.h"

namespace neverd::emulation::runtime {
/// Native transports use the architecture of their compiled host ABI. Their
/// live probes separately validate the hypervisor's actual capabilities.
inline bool matchesHost(GuestArchitecture Architecture) {
#if defined(__aarch64__) || defined(_M_ARM64)
  return Architecture == GuestArchitecture::AArch64;
#elif defined(__x86_64__) || defined(_M_X64)
  return Architecture == GuestArchitecture::X64;
#else
  return false;
#endif
}
inline ExecutionBackendKind nativeBackend() {
#if defined(__linux__)
  return ExecutionBackendKind::KVM;
#elif defined(_WIN32)
  return ExecutionBackendKind::WHP;
#else
  return ExecutionBackendKind::Unicorn;
#endif
}
} // namespace neverd::emulation::runtime
#endif
