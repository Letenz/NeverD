//===- DarwinDirectoryTimestampTestData.h - Directory timestamp data -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Independent virtual directory records and provider inputs.
#ifndef NEVERD_TESTS_DARWINDIRECTORYTIMESTAMPTESTDATA_H
#define NEVERD_TESTS_DARWINDIRECTORYTIMESTAMPTESTDATA_H
#include "DarwinDirectoryModeTestData.h"
#include "DarwinTimestampMutationTestData.h"
namespace neverd::emulation::darwin_test {
inline DarwinFileOptions directoryTimestampOptions(bool Created = false) {
  auto O = directoryModeOptions(Created);
  O.ModeMutations.reset();
  O.TimestampMutations.emplace();
  O.TimestampMutations->CreatedDirectoryObjects = Created;
  if (!Created)
    O.TimestampMutations->Objects["/dir"] = {111, 112};
  return O;
}
inline llvm::json::Value directoryTimestampFilesJSON(bool Created = false) {
  auto V = directoryModeFilesJSON(Created);
  auto &F = *V.getAsObject();
  F.erase("mode_mutations");
  llvm::json::Array Objects;
  if (!Created)
    Objects.push_back(llvm::json::Object{
        {"path", "/dir"},
        {"time", llvm::json::Object{{"seconds", 111}, {"nanoseconds", 112}}}});
  F["timestamp_mutations"] =
      llvm::json::Object{{"objects", std::move(Objects)},
                         {"created_regular_objects", false},
                         {"created_directory_objects", Created}};
  return V;
}
inline std::array<uint8_t, 160> directoryTimestampRecord() {
  // All144 stat bytes and both8-byte guards, authored independently of stat64.
  const uint64_t W[20] = {0xa5a5a5a5a5a5a5a5ULL,
                          7ULL | (uint64_t(0040700) << 32) | (2ULL << 48),
                          3,
                          501ULL | (20ULL << 32),
                          0,
                          1,
                          2,
                          3,
                          4,
                          5,
                          6,
                          7,
                          8,
                          32,
                          7,
                          512,
                          9,
                          0,
                          0,
                          0xa5a5a5a5a5a5a5a5ULL};
  std::array<uint8_t, 160> R{};
  for (unsigned I = 0; I != 20; ++I)
    for (unsigned J = 0; J != 8; ++J)
      R[I * 8 + J] = uint8_t(W[I] >> (J * 8));
  return R;
}
inline std::vector<DirectoryModeService>
directoryTimestampServices(bool Created) {
  std::vector<DirectoryModeService> R;
  if (Created)
    R.push_back({136, 0});
  for (auto V : {DirectoryModeService{5, 3}, {41, 4}, {5, 5}, {339, 0}})
    R.push_back(V);
  for (auto Number : {138u, 139u})
    for (uint64_t High : {0ULL, 0x1234567800000000ULL, 0xffffffff00000000ULL}) {
      R.push_back({Number | High, 0});
      for (auto Stat : {339u, 338u, 339u})
        R.push_back({Stat, 0});
    }
  for (auto V : {DirectoryModeService{139, 0},
                 {339, 0},
                 {138, 2, true},
                 {139, 14, true},
                 {139, 9, true},
                 {139, 14, true},
                 {339, 0},
                 {128, 0},
                 {339, 0},
                 {139, 0},
                 {338, 0},
                 {6, 0},
                 {139, 9, true},
                 {139, 0},
                 {339, 0},
                 {6, 0},
                 {6, 0},
                 {4, 1}})
    R.push_back(V);
  return R;
}

} // namespace neverd::emulation::darwin_test
#endif
