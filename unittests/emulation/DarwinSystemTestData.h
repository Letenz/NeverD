//===- DarwinSystemTestData.h - Explicit sysctl observations ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TESTS_DARWINSYSTEMTESTDATA_H
#define NEVERD_TESTS_DARWINSYSTEMTESTDATA_H

#include "neverd/emulation/DarwinSystemOptions.h"

namespace neverd::emulation::darwin_test {
inline DarwinSystemOptions systemOptions() {
  DarwinSystemOptions O;
  O.OSType = "Darwin";
  O.OSRelease = "24.test";
  O.OSRevision = INT32_MIN;
  O.OSVersion = "V42";
  O.KernelVersion = "NeverD virtual kernel";
  O.Machine = "virtual64";
  O.Model = "VirtualModel";
  O.CPUCount = 7;
  O.MemorySize = 0xfedcba9876543210ULL;
  return O;
}
inline constexpr char SystemJSON[] = R"({
  "os_type":"Darwin","os_release":"24.test","os_revision":-2147483648,
  "os_version":"V42","kernel_version":"NeverD virtual kernel",
  "machine":"virtual64","model":"VirtualModel","cpu_count":7,
  "memory_size":"18364758544493064720"})";
// Independent expected concatenation in native MIB order, including NULs.
inline constexpr char SystemHex[] =
    "44617277696e0032342e746573740000000080"
    "4e6576657244207669727475616c206b65726e656c0056343200"
    "7669727475616c3634005669727475616c4d6f64656c0007000000"
    "1032547698badcfe";
} // namespace neverd::emulation::darwin_test
#endif
