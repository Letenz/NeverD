//===- LinuxTimeOptions.h - Explicit guest clock observations ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_LINUXTIMEOPTIONS_H
#define NEVERD_EMULATION_LINUXTIMEOPTIONS_H

#include <cstdint>
#include <map>
#include <optional>

namespace neverd::emulation {
struct LinuxTimespec {
  int64_t Seconds = 0;
  int64_t Nanoseconds = 0;
};
struct LinuxTimezone {
  int32_t MinutesWest = 0;
  int32_t DSTTime = 0;
};
/// Explicit observations for Linux ELF64 and Android native workloads. Keys
/// are Linux clock IDs. An absent clock/timezone has no modeled value; zero
/// must be supplied explicitly. Reads never consult or advance host time.
struct LinuxTimeOptions {
  std::map<int32_t, LinuxTimespec> Clocks;
  std::optional<LinuxTimezone> Timezone;
  /// Opt in to relative nanosleep without signals. Time advances to the next
  /// sleep deadline only when no guest thread can run. Only realtime,
  /// monotonic and boottime observations may accompany this policy.
  bool AdvanceOnIdle = false;
};
} // namespace neverd::emulation
#endif
