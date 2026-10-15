//===- LinuxDeviceTestData.h - Explicit synthetic device inputs --*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared test input, independent of the fixture's byte comparison oracle.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TEST_LINUXDEVICETESTDATA_H
#define NEVERD_TEST_LINUXDEVICETESTDATA_H
#include "neverd/emulation/LinuxDeviceOptions.h"

inline neverd::emulation::LinuxDeviceOptions deviceTestInput() {
  neverd::emulation::LinuxDeviceOptions Result{
      2,
      0,
      {{0xa1000001,
        37,
        {},
        {{0, {}, {0x31, 0x32}, {0x52, 0x53}},
         {1, 8, {0x55, 0x56}, {0x90, 0x91}}}},
       {0xa1000002, -13, 0x8877665544332211ULL, {}}}};
  auto &Mapping = Result.ReadOnlyMappings.emplace_back();
  Mapping.Offset = 4096;
  Mapping.Bytes.resize(3 * 4096);
  for (size_t I = 0; I < Mapping.Bytes.size(); ++I)
    Mapping.Bytes[I] = 0x31 + I / 4096;
  return Result;
}

inline neverd::emulation::LinuxDeviceOptions updatingDeviceTestInput() {
  auto Result = deviceTestInput();
  Result.ReadOnlyMappings[0].Updates = {{0, 0, {0x61}},
                                        {1000000, 4095, {0xaa, 0xbb, 0xcc}},
                                        {1000000, 4096, {0xee}},
                                        {2000000, 4096, {0xdd}},
                                        {3000000, 8192, {0x88}}};
  return Result;
}

inline neverd::emulation::LinuxDeviceOptions contextDeviceTestInput() {
  neverd::emulation::LinuxDeviceOptions Result{2, 0, {}};
  Result.ReadOnlyMappings = {{4096,
                              std::vector<uint8_t>(4096, 0xa5),
                              {{1000000, 40, {7, 0, 0, 0}},
                               {1000000, 48, {5, 0, 0, 0}},
                               {2000000, 48, {7, 0, 0, 0}}}}};
  Result.KGSL = {4096, 3};
  return Result;
}
#endif
