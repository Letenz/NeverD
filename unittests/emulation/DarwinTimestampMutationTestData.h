//===- DarwinTimestampMutationTestData.h - Explicit timestamp data --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TESTS_DARWINTIMESTAMPMUTATIONTESTDATA_H
#define NEVERD_TESTS_DARWINTIMESTAMPMUTATIONTESTDATA_H
#include "DarwinCreatedSynchronizationTestData.h"
#include "DarwinModeTestData.h"

#include <algorithm>
#include <array>
#include <optional>
#include <vector>
namespace neverd::emulation::darwin_test {
inline DarwinFileOptions timestampOptions() {
  auto O = modeOptions();
  O.ModeMutations.reset();
  O.TimestampMutations.emplace().Objects["/data"] = {111, 112};
  return O;
}
// Independently authored LP64 stat64 record: eight-byte guards, all 144 bytes
// including reserved fields. This helper is never called by production code.
inline std::array<uint8_t, 160> timestampRecord() {
  std::array<uint8_t, 160> R{};
  for (unsigned I = 0; I != 8; ++I)
    R[I] = R[152 + I] = 0xa5;
  const auto Store = [&](unsigned Offset, unsigned Width, uint64_t Value) {
    for (unsigned I = 0; I != Width; ++I)
      R[8 + Offset + I] = uint8_t(Value >> (I * 8));
  };
  Store(0, 4, 7);
  Store(4, 2, 0100600);
  Store(6, 2, 1);
  Store(8, 8, 2);
  Store(16, 4, 501);
  Store(20, 4, 20);
  Store(32, 8, 1);
  Store(40, 8, 2);
  Store(48, 8, 3);
  Store(56, 8, 4);
  Store(64, 8, 5);
  Store(72, 8, 6);
  Store(80, 8, 7);
  Store(88, 8, 8);
  Store(96, 8, 6);
  Store(104, 8, 1);
  Store(112, 4, 512);
  Store(120, 4, 9);
  return R;
}
inline constexpr char TimestampFilesJSON[] = R"({
  "files":[{"path":"/data","bytes_hex":"616200ff6566","metadata":{
    "device":7,"inode":2,"mode":33152,"link_count":1,"uid":501,"gid":20,
    "size":6,"block_size":512,"blocks":1,"flags":0,"generation":9,
    "access_time":{"seconds":1,"nanoseconds":2},
    "modification_time":{"seconds":3,"nanoseconds":4},
    "change_time":{"seconds":5,"nanoseconds":6},
    "birth_time":{"seconds":7,"nanoseconds":8}}}],
  "directories":[{"path":"/","metadata":{
    "device":7,"inode":1,"mode":16832,"link_count":2,"uid":501,"gid":20,
    "size":0,"block_size":512,"blocks":0,"flags":0,"generation":9,
    "access_time":{"seconds":1,"nanoseconds":2},
    "modification_time":{"seconds":3,"nanoseconds":4},
    "change_time":{"seconds":5,"nanoseconds":6},
    "birth_time":{"seconds":7,"nanoseconds":8}}}],
  "working_directory":"/",
  "timestamp_mutations":{"objects":[{"path":"/data",
    "time":{"seconds":111,"nanoseconds":112}}]}})";
struct TimestampService {
  uint64_t Number, Value;
  bool Error = false;
  std::optional<uint64_t> Object = std::nullopt, Input = std::nullopt;
};
inline std::vector<TimestampService> timestampServices() {
  std::vector<TimestampService> R = {{5, 3},
                                     {5, 4},
                                     {41, 5, false, 3},
                                     {199, 0, false, 5},
                                     {199, 2, false, 5},
                                     {92, 0, false, 3}};
  const uint64_t High[] = {0, 0x1234567800000000ULL, 0xffffffff00000000ULL};
  for (unsigned N = 0; N != 6; ++N)
    for (bool FD : {false, true}) {
      R.push_back({339, 0, false, 3});
      R.push_back(
          {High[N % 3] | (FD ? 139u : 138u), 0, false,
           FD ? std::optional<uint64_t>(High[N % 3] | 5) : std::nullopt});
      for (uint64_t F : {3u, 4u, 5u})
        R.push_back({339, 0, false, F});
      R.push_back({199, 2, false, 5});
      R.push_back({199, 0, false, 4});
      R.push_back({92, 0, false, 3});
      R.push_back({92, 0, false, 4});
    }
  for (const auto &V :
       std::vector<TimestampService>{{339, 0, false, 4},
                                     {138, 2, true, std::nullopt, 1},
                                     {139, 14, true, UINT64_MAX, 1},
                                     {139, 9, true, UINT64_MAX},
                                     {339, 0, false, 4},
                                     {6, 0, false, 3},
                                     {139, 9, true, 3},
                                     {6, 0, false, 5},
                                     {6, 0, false, 4},
                                     {4, 1, false, 1}})
    R.push_back(V);
  return R;
}
inline std::vector<TimestampService> createdTimestampServices() {
  return {{5, 3},
          {339, 0, false, 3},
          {139, 0, false, 3},
          {339, 0, false, 3},
          {139, 0, false, 3, 0},
          {339, 0, false, 3},
          {6, 0, false, 3},
          {139, 9, true, 3},
          {4, 160, false, 1},
          {4, 160, false, 1}};
}
inline DarwinFileOptions createdTimestampOptions() {
  auto O = createdSynchronizationOptions(false, false);
  O.Synchronization.reset();
  O.DataSynchronization.reset();
  O.TimestampMutations.emplace().CreatedRegularObjects = true;
  return O;
}
inline std::array<uint8_t, 320> createdTimestampRecords() {
  auto Before = timestampRecord();
  const auto Store = [&](unsigned Offset, uint64_t V) {
    for (unsigned I = 0; I != 8; ++I)
      Before[8 + Offset + I] = uint8_t(V >> (I * 8));
  };
  Store(8, 100);
  Store(96, 0);
  Store(104, 0);
  for (unsigned O : {32u, 48u, 64u, 80u}) {
    Store(O, 41);
    Store(O + 8, 42);
  }
  const auto First = Before;
  for (unsigned O : {32u, 48u, 80u}) {
    Store(O, 0);
    Store(O + 8, 0);
  }
  Store(64, 101);
  Store(72, 102);
  std::array<uint8_t, 320> R{};
  std::copy(First.begin(), First.end(), R.begin());
  std::copy(Before.begin(), Before.end(), R.begin() + 160);
  return R;
}

} // namespace neverd::emulation::darwin_test
#endif
