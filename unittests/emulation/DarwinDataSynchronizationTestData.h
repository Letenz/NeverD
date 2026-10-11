//===- DarwinDataSynchronizationTestData.h - Data completion fixture -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original independent data integrity completion environment and ordinary file
/// fixture.
#ifndef NEVERD_TESTS_DARWINDATASYNCHRONIZATIONTESTDATA_H
#define NEVERD_TESTS_DARWINDATASYNCHRONIZATIONTESTDATA_H
#include "DarwinModeTestData.h"
namespace neverd::emulation::darwin_test {
inline DarwinFileOptions dataSynchronizationOptions() {
  auto O = modeOptions();
  O.ModeMutations.reset();
  O.DataSynchronization.emplace().Objects.insert("/data");
  return O;
}
inline constexpr char DataSynchronizationFilesJSON[] = R"({
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
  "data_synchronization":{"objects":["/data"]}})";
} // namespace neverd::emulation::darwin_test
#endif
