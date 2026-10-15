//===- DarwinSymbolicLinkFileSystemTestData.h - Symbolic mount facts ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TESTS_DARWINSYMBOLICLINKFILESYSTEMTESTDATA_H
#define NEVERD_TESTS_DARWINSYMBOLICLINKFILESYSTEMTESTDATA_H
#include "DarwinCreatedDirectoryFileSystemTestData.h"
namespace neverd::emulation::darwin_test {
inline DarwinFileOptions symbolicLinkFileSystemOptions(bool Created = false) {
  auto O = createdDirectoryFileSystemOptions(false, false);
  O.FileSystemObservations->Objects["/data"] = fileSystemSnapshot();
  O.FileSystemObservations->CreatedSymbolicLinkObjects = true;
  if (!Created) {
    O.SymbolicLinks["/link"] = {'d', 'a', 't', 'a'};
    O.Metadata["/link"] = modeMetadata(0120777, 10, 4);
    O.MutableSymbolicLinks.insert("/link");
    O.SymbolicLinkMutationPolicies["/link"] = {{81, 82}};
    O.FileSystemObservations->Objects["/link"] = fileSystemSnapshot();
  }
  return O;
}
inline llvm::json::Value symbolicLinkFileSystemFilesJSON(bool Created = false) {
  auto V = createdDirectoryFileSystemFilesJSON(false, false);
  auto &F = *V.getAsObject();
  auto &P = *F.getObject("filesystem_observations");
  P["created_symbolic_link_objects"] = true;
  P.getArray("objects")->push_back(llvm::json::Object{
      {"path", "/data"},
      {"snapshot", llvm::cantFail(llvm::json::parse(FileSystemSnapshotJSON))}});
  if (!Created) {
    auto M = *(*F.getArray("files"))[0].getAsObject()->getObject("metadata");
    M["mode"] = 41471;
    M["inode"] = 10;
    M["size"] = 4;
    F["symbolic_links"] = llvm::json::Array{llvm::json::Object{
        {"path", "/link"},
        {"target_hex", "64617461"},
        {"metadata", std::move(M)},
        {"mutable", true},
        {"mutation_policy",
         llvm::json::Object{
             {"mutation_time",
              llvm::json::Object{{"seconds", 81}, {"nanoseconds", 82}}}}}}};
    P.getArray("objects")->push_back(llvm::json::Object{
        {"path", "/link"},
        {"snapshot",
         llvm::cantFail(llvm::json::parse(FileSystemSnapshotJSON))}});
  }
  return V;
}
inline std::vector<FileSystemService>
symbolicFileSystemServices(unsigned Kind) {
  std::vector<FileSystemService> R;
  if (Kind == 1)
    R.push_back({57, 0});
  for (auto V : {FileSystemService{5, 3},
                 {41, 4, false, 3},
                 {346, 0, false, 3},
                 {345, 0},
                 {339, 0, false, 3}})
    R.push_back(V);
  for (uint64_t H : {0x1234567800000000ULL, 0xffffffff00000000ULL}) {
    R.push_back({H | 346, 0, false, H | 4});
    R.push_back({H | 345, 0});
  }
  R.push_back({339, 0, false, 3});
  if (Kind == 2) {
    R.push_back({4, 2200, false, 1});
    R.push_back({4, 2200, false, 1});
  }
  for (auto V : {FileSystemService{346, 9, true, UINT64_MAX, 1},
                 {346, 14, true, 3, 1},
                 {345, 2, true, std::nullopt, 1},
                 {128, 0},
                 {345, 2, true, std::nullopt, 1},
                 {346, 0, false, 4},
                 {345, 0},
                 {10, 0},
                 {345, 2, true, std::nullopt, 1},
                 {346, 0, false, 4},
                 {57, 0},
                 {5, 5},
                 {346, 0, false, 5},
                 {339, 0, false, 5}})
    R.push_back(V);
  if (Kind == 2)
    R.push_back({4, 2200, false, 1});
  for (auto V : {FileSystemService{346, 0, false, 4},
                 {345, 2, true, std::nullopt, 1},
                 {6, 0, false, 3},
                 {346, 9, true, 3, 1},
                 {6, 0, false, 4},
                 {6, 0, false, 5},
                 {10, 0},
                 {4, 1, false, 1}})
    R.push_back(V);
  return R;
}
inline std::string symbolicFileSystemOutput(unsigned Kind) {
  if (Kind != 2)
    return "Y";
  const auto Link = directoryFileSystemRecord(17), Target = fileSystemRecord();
  return std::string(reinterpret_cast<const char *>(Link.data()), Link.size()) +
         std::string(reinterpret_cast<const char *>(Target.data()),
                     Target.size()) +
         std::string(reinterpret_cast<const char *>(Target.data()),
                     Target.size()) +
         "Y";
}

} // namespace neverd::emulation::darwin_test
#endif
