//===- HvfTestPolicy.h - Required native hardware coverage ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNITTESTS_HVFTESTPOLICY_H
#define NEVERD_UNITTESTS_HVFTESTPOLICY_H
#include "neverd/emulation/ExecutionBackend.h"

#include <cstdlib>
namespace neverd::emulation {
inline bool requireHvf(ExecutionBackendKind Backend, GuestArchitecture ISA) {
#if defined(__APPLE__) && (defined(__arm64__) || defined(__aarch64__))
  constexpr auto Host = GuestArchitecture::AArch64;
#elif defined(__APPLE__) && defined(__x86_64__)
  constexpr auto Host = GuestArchitecture::X64;
#else
  return false;
#endif
#if defined(__APPLE__) &&                                                      \
    (defined(__arm64__) || defined(__aarch64__) || defined(__x86_64__))
  return Backend == ExecutionBackendKind::HVF && ISA == Host &&
         std::getenv("NEVERD_REQUIRE_HVF");
#endif
}
} // namespace neverd::emulation
#endif
