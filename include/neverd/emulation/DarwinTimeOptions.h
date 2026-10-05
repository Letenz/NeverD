//===- DarwinTimeOptions.h - Explicit Darwin clock observations -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_DARWINTIMEOPTIONS_H
#define NEVERD_EMULATION_DARWINTIMEOPTIONS_H

#include <cstdint>
#include <optional>

namespace neverd::emulation {
/// The observed gettimeofday seconds are zero-extended from unsigned 32 bits.
struct DarwinTimeOfDay {
  uint32_t Seconds = 0;
  uint32_t Microseconds = 0;
};
struct DarwinTimezone {
  int32_t MinutesWest = 0;
  int32_t DSTTime = 0;
};
/// Fixed observations. Absence is unknown; explicit zero is a value. No host
/// clocks, timezone, automatic advancement or tick-to-time conversion is used.
/// Requested calendar and absolute values form one sample, before user copies.
struct DarwinTimeOptions {
  std::optional<DarwinTimeOfDay> TimeOfDay;
  std::optional<DarwinTimezone> Timezone;
  std::optional<uint64_t> MachAbsoluteTime;
};
} // namespace neverd::emulation
#endif
