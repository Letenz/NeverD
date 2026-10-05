//===- DarwinTimeTestData.h - Explicit time observations -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TESTS_DARWINTIMETESTDATA_H
#define NEVERD_TESTS_DARWINTIMETESTDATA_H

#include "neverd/emulation/DarwinTimeOptions.h"

namespace neverd::emulation::darwin_test {
inline DarwinTimeOptions timeOptions() {
  return {DarwinTimeOfDay{0xf1234567, 654321}, DarwinTimezone{-480, -1},
          0xfedcba9876543210ULL};
}
inline constexpr char TimeJSON[] = R"({
  "time_of_day":{"seconds":4045620583,"microseconds":654321},
  "timezone":{"minutes_west":-480,"dst_time":-1},
  "mach_absolute_time":"18364758544493064720"})";
// Independent LP64 layout expectation, including timeval's zero padding.
inline constexpr char TimeHex[] =
    "674523f100000000f1fb09000000000020feffffffffffff1032547698badcfe";
} // namespace neverd::emulation::darwin_test
#endif
