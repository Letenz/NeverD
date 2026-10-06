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
inline DarwinSystemOptions resourceLimitOptions() {
  DarwinSystemOptions O;
  O.ResourceLimits = {{0, {0ULL, 0ULL}},
                      {1, {9223372036854775807ULL, 9223372036854775807ULL}},
                      {2, {81985529216486895ULL, 9223372036854775807ULL}},
                      {3, {1073741824ULL, 2147483648ULL}},
                      {4, {0ULL, 9223372036854775807ULL}},
                      {5, {9223372036854775806ULL, 9223372036854775807ULL}},
                      {6, {8192ULL, 16384ULL}},
                      {7, {32ULL, 128ULL}},
                      {8, {256ULL, 1024ULL}}};
  return O;
}
inline constexpr char ResourceLimitsJSON[] =
    R"({"resource_limits":[{"resource":0,"current":0,"maximum":0},{"resource":1,"current":"9223372036854775807","maximum":"9223372036854775807"},{"resource":2,"current":"81985529216486895","maximum":"9223372036854775807"},{"resource":3,"current":1073741824,"maximum":2147483648},{"resource":4,"current":0,"maximum":"9223372036854775807"},{"resource":5,"current":"9223372036854775806","maximum":"9223372036854775807"},{"resource":6,"current":8192,"maximum":16384},{"resource":7,"current":32,"maximum":128},{"resource":8,"current":256,"maximum":1024}]})";
// Independently packed expected pairs in canonical resource order.
inline constexpr char ResourceLimitsHex[] =
    "00000000000000000000000000000000ffffffffffffff7fffffffffffffff7f"
    "efcdab8967452301ffffffffffffff7f00000040000000000000008000000000"
    "0000000000000000ffffffffffffff7ffeffffffffffff7fffffffffffffff7f"
    "0020000000000000004000000000000020000000000000008000000000000000"
    "00010000000000000004000000000000";
} // namespace neverd::emulation::darwin_test
#endif
