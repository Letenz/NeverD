//===- DarwinDirectoryOwnershipTestData.h - Directory ownership data -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Independent providers and literal complete directory stat records.
#ifndef NEVERD_TESTS_DARWINDIRECTORYOWNERSHIPTESTDATA_H
#define NEVERD_TESTS_DARWINDIRECTORYOWNERSHIPTESTDATA_H
#include "DarwinDirectoryModeTestData.h"
#include "DarwinOwnershipTestData.h"
namespace neverd::emulation::darwin_test {
inline DarwinFileOptions directoryOwnershipOptions(bool Created = false) {
  auto O = directoryModeOptions(Created);
  O.ModeMutations.reset();
  O.OwnershipMutations.emplace();
  O.OwnershipMutations->CreatedDirectoryObjects = Created;
  if (!Created) {
    O.Metadata["/dir"].GID = 31;
    O.OwnershipMutations->Objects["/dir"] = {81, 82};
  }
  return O;
}
inline llvm::json::Value directoryOwnershipFilesJSON(bool Created = false) {
  auto V = directoryModeFilesJSON(Created);
  auto &F = *V.getAsObject();
  F.erase("mode_mutations");
  llvm::json::Array Objects;
  if (!Created) {
    (*(*F.getArray("directories"))[1].getAsObject()->getObject(
        "metadata"))["gid"] = 31;
    Objects.push_back(llvm::json::Object{
        {"path", "/dir"},
        {"time", llvm::json::Object{{"seconds", 81}, {"nanoseconds", 82}}}});
  }
  F["ownership_mutations"] =
      llvm::json::Object{{"objects", std::move(Objects)},
                         {"created_regular_objects", false},
                         {"created_directory_objects", Created}};
  return V;
}
inline std::array<uint8_t, 160> directoryOwnershipRecord(bool Created = false) {
  // Independently authored144-byte stat and both8-byte guards.
  const uint64_t W[20] = {0xa5a5a5a5a5a5a5a5ULL,
                          7ULL | (uint64_t(0040700) << 32) | (2ULL << 48),
                          Created ? 100ULL : 3ULL,
                          501ULL | (uint64_t(Created ? 20 : 31) << 32),
                          0,
                          Created ? 41ULL : 1ULL,
                          Created ? 42ULL : 2ULL,
                          Created ? 41ULL : 3ULL,
                          Created ? 42ULL : 4ULL,
                          Created ? 41ULL : 5ULL,
                          Created ? 42ULL : 6ULL,
                          Created ? 41ULL : 7ULL,
                          Created ? 42ULL : 8ULL,
                          Created ? 64ULL : 32ULL,
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
inline std::array<uint8_t, 160> directoryOwnershipChildRecord(unsigned Kind) {
  // Newly created regular100, directory101 and symbolic102, GID20.
  const uint64_t W[20] = {0xa5a5a5a5a5a5a5a5ULL,
                          7ULL |
                              (uint64_t(Kind == 0   ? 0100600
                                        : Kind == 1 ? 0040700
                                                    : 0120755)
                               << 32) |
                              (uint64_t(Kind == 1 ? 2 : 1) << 48),
                          100ULL + Kind,
                          501ULL | (20ULL << 32),
                          0,
                          41,
                          42,
                          41,
                          42,
                          41,
                          42,
                          41,
                          42,
                          Kind == 0   ? 0ULL
                          : Kind == 1 ? 64ULL
                                      : 4ULL,
                          Kind == 0   ? 0ULL
                          : Kind == 1 ? 7ULL
                                      : 1ULL,
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
directoryOwnershipServices(bool Created) {
  std::vector<DirectoryModeService> R;
  if (Created)
    R.push_back({136, 0});
  for (auto V : {DirectoryModeService{5, 3}, {41, 4}, {5, 5}, {339, 0}})
    R.push_back(V);
  for (auto Number : {16u, 123u, 468u})
    for (uint64_t High : {0ULL, 0x1234567800000000ULL, 0xffffffff00000000ULL})
      for (unsigned Pattern = High ? 3 : 0; Pattern != 4; ++Pattern) {
        R.push_back({High | Number, 0});
        R.push_back({339, 0});
        if (Pattern == 2) {
          R.push_back({338, 0});
          R.push_back({339, 0});
        }
      }
  for (auto V : {DirectoryModeService{123, 9, true},
                 {468, 22, true},
                 {16, 2, true},
                 {339, 0},
                 {128, 0},
                 {339, 0},
                 {123, 0},
                 {338, 0},
                 {6, 0},
                 {123, 9, true},
                 {123, 0},
                 {339, 0},
                 {6, 0},
                 {6, 0},
                 {4, 1}})
    R.push_back(V);
  return R;
}

} // namespace neverd::emulation::darwin_test
#endif
