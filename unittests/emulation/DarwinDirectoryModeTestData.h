//===- DarwinDirectoryModeTestData.h - Independent directory providers ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TESTS_DARWINDIRECTORYMODETESTDATA_H
#define NEVERD_TESTS_DARWINDIRECTORYMODETESTDATA_H
#include "DarwinCreatedSynchronizationTestData.h"

#include "llvm/Support/JSON.h"

#include <vector>

namespace neverd::emulation::darwin_test {
inline DarwinFileOptions directoryModeOptions(bool Created = false) {
  auto O =
      Created ? createdSynchronizationOptions(false, false) : modeOptions();
  O.Synchronization.reset();
  O.DataSynchronization.reset();
  O.MutableDirectories.insert("/");
  O.DirectoryMutationPolicies["/"] = {16, {91, 92}};
  O.ModeMutations.emplace();
  O.ModeMutations->Objects.clear();
  O.ModeMutations->CreatedDirectoryObjects = Created;
  if (!Created) {
    O.Directories.insert("/dir");
    O.Metadata["/dir"] = modeMetadata(0040700, 3, 32);
    O.Metadata["/dir"].Blocks = 7;
    O.MovableDirectories.insert("/dir");
    O.DirectoryMutationPolicies["/dir"] = {16, {81, 82}};
    O.ModeMutations->Objects["/dir"] = {71, 72};
  }
  return O;
}
inline llvm::json::Value directoryModeFilesJSON(bool Created = false) {
  auto V = llvm::cantFail(llvm::json::parse(
      Created ? CreatedSynchronizationFilesJSON : ModeFilesJSON));
  auto &O = *V.getAsObject();
  O.erase("synchronization");
  O.erase("data_synchronization");
  auto &Root = *(*O.getArray("directories"))[0].getAsObject();
  Root["mutable"] = true;
  Root["mutation_policy"] = llvm::json::Object{
      {"directory_entry_size", 16},
      {"mutation_time",
       llvm::json::Object{{"seconds", 91}, {"nanoseconds", 92}}}};
  llvm::json::Array Objects;
  if (!Created) {
    auto Metadata = *Root.getObject("metadata");
    Metadata["inode"] = 3;
    Metadata["size"] = 32;
    Metadata["blocks"] = 7;
    O.getArray("directories")
        ->push_back(llvm::json::Object{
            {"path", "/dir"},
            {"metadata", std::move(Metadata)},
            {"movable", true},
            {"mutation_policy",
             llvm::json::Object{
                 {"directory_entry_size", 16},
                 {"mutation_time",
                  llvm::json::Object{{"seconds", 81}, {"nanoseconds", 82}}}}}});
    Objects.push_back(llvm::json::Object{
        {"path", "/dir"},
        {"time", llvm::json::Object{{"seconds", 71}, {"nanoseconds", 72}}}});
  }
  O["mode_mutations"] =
      llvm::json::Object{{"objects", std::move(Objects)},
                         {"created_regular_objects", false},
                         {"created_directory_objects", Created}};
  return V;
}
struct DirectoryModeService {
  uint64_t Number, Value;
  bool Error = false;
};
inline std::vector<DirectoryModeService> directoryModeServices(bool Created) {
  std::vector<DirectoryModeService> R;
  if (Created)
    R.push_back({136, 0});
  for (auto V : {DirectoryModeService{5, 3}, {41, 4}, {5, 5}, {339, 0}})
    R.push_back(V);
  for (auto Number : {15u, 124u, 467u})
    for (uint64_t High : {0ULL, 0x1234567800000000ULL, 0xffffffff00000000ULL}) {
      R.push_back({Number | High, 0});
      for (auto Stat : {339u, 338u, 339u})
        R.push_back({Stat, 0});
    }
  for (auto V : {DirectoryModeService{13, 0},
                 {15, 0},
                 {467, 0},
                 {339, 0},
                 {128, 0},
                 {339, 0},
                 {124, 0},
                 {338, 0},
                 {124, 9, true},
                 {467, 22, true},
                 {339, 0},
                 {12, 0},
                 {6, 0},
                 {124, 9, true},
                 {124, 0},
                 {339, 0},
                 {6, 0},
                 {6, 0},
                 {4, 1}})
    R.push_back(V);
  return R;
}
} // namespace neverd::emulation::darwin_test
#endif
