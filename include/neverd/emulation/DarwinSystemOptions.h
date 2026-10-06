//===- DarwinSystemOptions.h - Explicit system observations -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_DARWINSYSTEMOPTIONS_H
#define NEVERD_EMULATION_DARWINSYSTEMOPTIONS_H

#include <array>
#include <cstddef>
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
/// One fixed LP64 getrusage observation, never host/guest runtime accounting.
/// Microseconds must be below 1000000; seconds and counters preserve signed
/// 64-bit values. Counters use Darwin's raw units, without Linux conversions.
struct DarwinResourceUsage {
  static constexpr std::size_t CounterCount = 14;
  int64_t UserSeconds = 0;
  uint32_t UserMicroseconds = 0;
  int64_t SystemSeconds = 0;
  uint32_t SystemMicroseconds = 0;
  /// Indices 0..13: ru_maxrss, ru_ixrss, ru_idrss, ru_isrss, ru_minflt,
  /// ru_majflt, ru_nswap, ru_inblock, ru_oublock, ru_msgsnd, ru_msgrcv,
  /// ru_nsignals, ru_nvcsw, ru_nivcsw. Meanings are implementation-defined.
  std::array<int64_t, CounterCount> Counters{};
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
  /// Independent fixed observations: one query never requires the other.
  /// Missing children remain unknown even when fork/wait are unsupported.
  /// No clock advancement, budget changes or guest performance is inferred.
  std::optional<DarwinResourceUsage> ResourceUsageSelf;
  std::optional<DarwinResourceUsage> ResourceUsageChildren;
};
} // namespace neverd::emulation
#endif
