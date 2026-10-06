//===- DarwinSystemOptions.h - Explicit system observations -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_DARWINSYSTEMOPTIONS_H
#define NEVERD_EMULATION_DARWINSYSTEMOPTIONS_H

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace neverd::emulation {
/// One fixed raw getrlimit observation, not a resource enforcement policy.
/// Both values are at most Darwin RLIM_INFINITY (INT64_MAX); Current <=
/// Maximum.
struct DarwinResourceLimit {
  uint64_t Current = 0;
  uint64_t Maximum = 0;
};
/// Fixed system observations, independent of host hardware and OS identity.
/// Missing is unknown; an empty string is an explicit value. Strings contain
/// no NUL and at most 1023 bytes. Page size comes from the guest memory policy.
struct DarwinSystemOptions {
  std::optional<std::string> OSType;
  std::optional<std::string> OSRelease;
  std::optional<std::string> OSVersion;
  std::optional<std::string> KernelVersion;
  std::optional<std::string> Machine;
  std::optional<std::string> Model;
  std::optional<int32_t> OSRevision;
  /// Positive, at most INT32_MAX. This does not add thread scheduling.
  std::optional<uint32_t> CPUCount;
  /// Reported memory, independent of the emulation allocation budget.
  std::optional<uint64_t> MemorySize;
  /// Canonical resource keys 0..8. Missing is unknown, and zero is explicit.
  /// Supplies read-only getrlimit observations without querying the host,
  /// changing execution budgets or enabling setrlimit/signal delivery.
  std::map<uint32_t, DarwinResourceLimit> ResourceLimits;
};
} // namespace neverd::emulation
#endif
