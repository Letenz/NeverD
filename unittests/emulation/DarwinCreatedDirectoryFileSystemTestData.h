//===- DarwinCreatedDirectoryFileSystemTestData.h - Created mount facts --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TESTS_DARWINCREATEDDIRECTORYFILESYSTEMTESTDATA_H
#define NEVERD_TESTS_DARWINCREATEDDIRECTORYFILESYSTEMTESTDATA_H
#include "DarwinFileSystemTestData.h"

#include "llvm/Support/JSON.h"

namespace neverd::emulation::darwin_test {
inline DarwinFileOptions
createdDirectoryFileSystemOptions(bool Directory = true, bool Regular = false) {
  auto O = createdFileSystemOptions();
  O.FileSystemObservations->CreatedDirectoryObjects = Directory;
  O.FileSystemObservations->CreatedRegularObjects = Regular;
  return O;
}
inline llvm::json::Value
createdDirectoryFileSystemFilesJSON(bool Directory = true,
                                    bool Regular = false) {
  auto V = llvm::cantFail(llvm::json::parse(CreatedSynchronizationFilesJSON));
  auto &F = *V.getAsObject();
  F.erase("synchronization");
  F.erase("data_synchronization");
  F["filesystem_observations"] = llvm::json::Object{
      {"objects", llvm::json::Array{llvm::json::Object{
                      {"path", "/"},
                      {"snapshot", llvm::cantFail(llvm::json::parse(
                                       FileSystemSnapshotJSON))}}}},
      {"created_directory_objects", Directory},
      {"created_regular_objects", Regular}};
  return V;
}
inline std::array<uint8_t, 2200> directoryFileSystemRecord(uint32_t Owner) {
  auto R = fileSystemRecord();
  // Only the declared filesystem owner differs; not the directory stat UID.
  for (unsigned I = 0; I != 4; ++I)
    R[16 + 56 + I] = uint8_t(Owner >> (I * 8));
  return R;
}
inline std::vector<FileSystemService>
createdDirectoryFileSystemServices(bool Literal = false) {
  std::vector<FileSystemService> R = {{136, 0},          {5, 3},
                                      {41, 4, false, 3}, {346, 0, false, 3},
                                      {345, 0},          {339, 0, false, 3}};
  for (uint64_t H : {0x1234567800000000ULL, 0xffffffff00000000ULL}) {
    R.push_back({H | 346, 0, false, H | 4});
    R.push_back({H | 345, 0});
  }
  R.push_back({339, 0, false, 3});
  R.push_back({92, 0, false, 3});
  if (Literal)
    R.push_back({4, 2200, false, 1});
  for (auto V : {FileSystemService{346, 9, true, UINT64_MAX, 1},
                 {346, 14, true, 3, 1},
                 {345, 2, true, std::nullopt, 1},
                 {128, 0},
                 {345, 2, true, std::nullopt, 1},
                 {346, 0, false, 4},
                 {345, 0},
                 {137, 0},
                 {345, 2, true, std::nullopt, 1},
                 {346, 0, false, 4},
                 {136, 0},
                 {5, 5},
                 {346, 0, false, 5},
                 {339, 0, false, 5}})
    R.push_back(V);
  if (Literal)
    R.push_back({4, 2200, false, 1});
  for (auto V : {FileSystemService{346, 0, false, 4},
                 {6, 0, false, 3},
                 {346, 9, true, 3, 1},
                 {6, 0, false, 4},
                 {6, 0, false, 5},
                 {137, 0},
                 {4, 1, false, 1}})
    R.push_back(V);
  return R;
}
inline std::string createdDirectoryFileSystemOutput(bool Literal = false) {
  if (!Literal)
    return "D";
  const auto R = fileSystemRecord();
  const std::string Bytes(reinterpret_cast<const char *>(R.data()), R.size());
  return Bytes + Bytes + "D";
}

} // namespace neverd::emulation::darwin_test
#endif
