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
/// Fixed observations for Linux ELF64 and Android native workloads. Keys are
/// Linux clock IDs. Reads do not advance clocks or consult the host. An absent
/// clock/timezone has no modeled value; zero must be supplied explicitly.
struct LinuxTimeOptions {
  std::map<int32_t, LinuxTimespec> Clocks;
  std::optional<LinuxTimezone> Timezone;
};
} // namespace neverd::emulation
#endif
