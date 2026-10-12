//===- DarwinFullSynchronizationTestData.h - Explicit full/barrier inputs -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original independent full/barrier environments and authored service oracle.
#ifndef NEVERD_TESTS_DARWINFULLSYNCHRONIZATIONTESTDATA_H
#define NEVERD_TESTS_DARWINFULLSYNCHRONIZATIONTESTDATA_H
#include "DarwinModeTestData.h"

#include <array>
#include <optional>
namespace neverd::emulation::darwin_test {
inline DarwinFileOptions fullSynchronizationOptions() {
  auto O = modeOptions();
  O.ModeMutations.reset();
  O.FullSynchronization.emplace().Objects.insert("/data");
  O.BarrierSynchronization.emplace().Objects.insert("/data");
  return O;
}
inline constexpr char FullSynchronizationFilesJSON[] = R"({
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
  "full_synchronization":{"objects":["/data"]},
  "barrier_synchronization":{"objects":["/data"]}})";
struct FullSynchronizationService {
  uint64_t Number, Value;
  bool Error = false;
  std::optional<uint64_t> FD = std::nullopt;
  std::optional<uint64_t> Command = std::nullopt;
  std::optional<uint64_t> Argument = std::nullopt;
};
inline std::array<FullSynchronizationService, 148>
fullSynchronizationServices() {
  std::array<FullSynchronizationService, 148> R{};
  unsigned I = 0;
  const auto Add = [&](FullSynchronizationService E) { R[I++] = E; };
  Add({5, 3, false, std::nullopt, 0, 0});
  Add({5, 4, false, std::nullopt, 0, 0});
  Add({41, 5, false, 3, 0, 0});
  Add({199, 2, false, 5, 2, 0});
  Add({339, 0, false, 3, std::nullopt, 0});
  Add({92, 0, false, 3, 3, 0});
  const uint64_t High[] = {0, 0x1234567800000000ULL, 0xffffffff00000000ULL};
  for (unsigned H = 0; H != 3; ++H)
    for (unsigned N = 0; N != 2; ++N)
      for (uint64_t Command : {51u, 85u}) {
        const auto S = High[H] | (N ? 406u : 92u);
        const uint64_t Argument = H == 0 ? 0 : H == 1 ? 1 : UINT64_MAX;
        for (uint64_t FD : std::array<uint64_t, 4>{3, 5, 4, 0xffffffff})
          Add({S, FD == 0xffffffff ? 9u : 0u, FD == 0xffffffff, High[H] | FD,
               High[H] | Command, Argument});
        Add({339, 0, false, 3, std::nullopt, 0});
        Add({339, 0, false, 4, std::nullopt, 0});
        Add({339, 0, false, 5, std::nullopt, 0});
        Add({199, 2, false, 5, 0, 1});
        Add({199, 0, false, 4, 0, 1});
        Add({92, 0, false, 3, 3, 0});
        Add({92, 0, false, 4, 3, 0});
      }
  Add({6, 0, false, 3, 0, 0});
  Add({92, 9, true, 3, 51, UINT64_MAX});
  Add({406, 0, false, 5, 85, UINT64_MAX});
  Add({6, 0, false, 5, 0, 0});
  Add({92, 9, true, 5, 51, UINT64_MAX});
  Add({406, 0, false, 4, 85, UINT64_MAX});
  Add({339, 0, false, 4, std::nullopt, 0});
  Add({92, 0, false, 4, 3, 0});
  Add({6, 0, false, 4, 0, 0});
  Add({4, 1, false, 1, std::nullopt, 1});
  return R;
}
inline DarwinFileOptions
createdFullSynchronizationOptions(bool Full = true, bool Barrier = true) {
  auto O = fullSynchronizationOptions();
  O.FullSynchronization->Objects.clear();
  O.BarrierSynchronization->Objects.clear();
  O.FullSynchronization->CreatedRegularObjects = Full;
  O.BarrierSynchronization->CreatedRegularObjects = Barrier;
  O.MutableDirectories.insert("/");
  O.DirectoryMutationPolicies["/"] = {16, {91, 92}};
  O.InitialUmask = 022;
  DarwinFileCreationPolicy P;
  P.FirstInode = 100;
  P.BlockSize = 512;
  P.Generation = 9;
  P.Time = {41, 42};
  P.Mutation = {512, {101, 102}};
  O.CreationPolicy = P;
  return O;
}
inline std::array<FullSynchronizationService, 10>
createdFullSynchronizationServices() {
  return {{{5, 3, false, std::nullopt, 0x202, 0600},
           {339, 0, false, 3, std::nullopt, 0},
           {92, 0, false, 3, 51, UINT64_MAX},
           {406, 0, false, 3, 85, UINT64_MAX},
           {339, 0, false, 3, std::nullopt, 0},
           {6, 0, false, 3, 0, 0},
           {92, 9, true, 3, 51, UINT64_MAX},
           {406, 9, true, 3, 85, UINT64_MAX},
           {4, 160, false, 1, std::nullopt, 160},
           {4, 160, false, 1, std::nullopt, 160}}};
}
inline std::array<uint8_t, 160> createdFullSynchronizationRecord() {
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
  Store(8, 8, 100);
  Store(16, 4, 501);
  Store(20, 4, 20);
  for (unsigned Offset : {32u, 48u, 64u, 80u}) {
    Store(Offset, 8, 41);
    Store(Offset + 8, 8, 42);
  }
  Store(112, 4, 512);
  Store(120, 4, 9);
  return R;
}
} // namespace neverd::emulation::darwin_test
#endif
