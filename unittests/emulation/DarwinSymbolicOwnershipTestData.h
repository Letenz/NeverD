//===- DarwinSymbolicOwnershipTestData.h - Symbolic ownership facts ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TESTS_DARWINSYMBOLICOWNERSHIPTESTDATA_H
#define NEVERD_TESTS_DARWINSYMBOLICOWNERSHIPTESTDATA_H
#include "DarwinCreatedAttributeTestData.h"
#include "DarwinDirectoryOwnershipTestData.h"

#include "llvm/ADT/StringExtras.h"

#include <optional>

namespace neverd::emulation::darwin_test {
inline constexpr const char *SymbolicOwnershipNames[] = {
    "/dir/link", "/dir/dangling", "/dir/cycle", "/dir/other"};
inline constexpr const char *SymbolicOwnershipTargets[] = {"../data", "missing",
                                                           "cycle", "../data"};
inline DarwinCredentials symbolicOwnershipCredentials() {
  auto C = modeCredentials();
  C.GroupAccessList = {20, 31};
  C.GroupMembershipUID = 4294967195u;
  return C;
}
inline DarwinFileOptions symbolicOwnershipOptions(bool Created = false) {
  auto O = ownershipOptions();
  O.Directories.insert("/dir");
  O.Metadata["/dir"] = modeMetadata(0040700, 3, 32);
  O.Metadata["/dir"].Blocks = 7;
  O.MutableDirectories = {"/", "/dir"};
  O.MovableDirectories.insert("/dir");
  O.SwapRenameDirectories.insert("/dir");
  O.DirectoryMutationPolicies["/"] = {16, {91, 92}};
  O.DirectoryMutationPolicies["/dir"] = {16, {93, 94}};
  O.InitialUmask = 022;
  O.CreationPolicy = directoryOwnershipOptions(true).CreationPolicy;
  O.OwnershipMutations->CreatedSymbolicLinkObjects = Created;
  for (unsigned I = 0; I != 4; ++I) {
    const std::string Path = SymbolicOwnershipNames[I];
    const auto Target = llvm::StringRef(SymbolicOwnershipTargets[I]);
    O.SymbolicLinks[Path] = {Target.bytes_begin(), Target.bytes_end()};
    O.Metadata[Path] = modeMetadata(0120755, 4 + I, Target.size());
    O.Metadata[Path].GID = 31;
    O.MutableSymbolicLinks.insert(Path);
    O.SymbolicLinkMutationPolicies[Path] = {{101, 102}};
    O.OwnershipMutations->Objects[Path] = {181 + 2 * I, 182 + 2 * I};
  }
  return O;
}
inline llvm::json::Value symbolicOwnershipFilesJSON(bool Created = false) {
  auto V = directoryModeFilesJSON();
  auto &F = *V.getAsObject();
  F.erase("mode_mutations");
  (*(*F.getArray("files"))[0].getAsObject()->getObject("metadata"))["gid"] = 31;
  auto &D = *(*F.getArray("directories"))[1].getAsObject();
  D["mutable"] = true;
  D["swap_rename"] = true;
  (*D.getObject("mutation_policy"))["mutation_time"] =
      llvm::json::Object{{"seconds", 93}, {"nanoseconds", 94}};
  auto Policy = llvm::cantFail(llvm::json::parse(CreatedAttributeFilesJSON));
  F["creation_policy"] = *Policy.getAsObject()->get("creation_policy");
  F["umask"] = 18;
  auto M = *(*F.getArray("files"))[0].getAsObject()->getObject("metadata");
  llvm::json::Array Links, Objects;
  Objects.push_back(llvm::json::Object{
      {"path", "/data"},
      {"time", llvm::json::Object{{"seconds", 81}, {"nanoseconds", 82}}}});
  for (unsigned I = 0; I != 4; ++I) {
    auto Metadata = M;
    Metadata["mode"] = 0120755;
    Metadata["inode"] = 4 + I;
    Metadata["size"] = llvm::StringRef(SymbolicOwnershipTargets[I]).size();
    Links.push_back(llvm::json::Object{
        {"path", SymbolicOwnershipNames[I]},
        {"target_hex",
         llvm::toHex(llvm::StringRef(SymbolicOwnershipTargets[I]))},
        {"metadata", std::move(Metadata)},
        {"mutable", true},
        {"mutation_policy",
         llvm::json::Object{
             {"mutation_time",
              llvm::json::Object{{"seconds", 101}, {"nanoseconds", 102}}}}}});
    Objects.push_back(llvm::json::Object{
        {"path", SymbolicOwnershipNames[I]},
        {"time", llvm::json::Object{{"seconds", 181 + 2 * I},
                                    {"nanoseconds", 182 + 2 * I}}}});
  }
  F["symbolic_links"] = std::move(Links);
  F["ownership_mutations"] =
      llvm::json::Object{{"objects", std::move(Objects)},
                         {"created_regular_objects", false},
                         {"created_directory_objects", false},
                         {"created_symbolic_link_objects", Created}};
  return V;
}
inline std::array<uint8_t, 160> symbolicOwnershipRecord(unsigned Index = 0,
                                                        bool Created = false) {
  // Independent literal complete stat144 and both eight-byte guards.
  const uint64_t W[20] = {0xa5a5a5a5a5a5a5a5ULL,
                          7ULL | (uint64_t(0120755) << 32) | (1ULL << 48),
                          Created ? 100ULL : 4ULL + Index,
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
                          Index == 2 ? 5ULL : 7ULL,
                          1,
                          512,
                          9,
                          0,
                          0,
                          0xa5a5a5a5a5a5a5a5ULL};
  std::array<uint8_t, 160> Out{};
  for (unsigned I = 0; I != 20; ++I)
    for (unsigned J = 0; J != 8; ++J)
      Out[I * 8 + J] = uint8_t(W[I] >> (8 * J));
  return Out;
}
inline constexpr char SymbolicOwnershipCredentialsJSON[] = R"({"credentials":{
  "real_uid":700,"effective_uid":501,"real_gid":31,"effective_gid":20,
  "groups":[20,31],"group_membership_uid":4294967195}})";
inline std::string symbolicOwnershipOutput(unsigned Kind) {
  if (!(Kind & 1))
    return "W";
  auto Link = symbolicOwnershipRecord(0, Kind >= 2);
  auto set = [](auto &Record, unsigned Offset, uint64_t Value,
                unsigned Size = 8) {
    for (unsigned I = 0; I != Size; ++I)
      Record[Offset + I] = uint8_t(Value >> (8 * I));
  };
  set(Link, 28, 20, 4);
  set(Link, 72, Kind >= 2 ? 101 : 181);
  set(Link, 80, Kind >= 2 ? 102 : 182);
  // Independent complete regular target stat144, including both guards.
  const uint64_t Words[20] = {0xa5a5a5a5a5a5a5a5ULL,
                              7ULL | (uint64_t(0100600) << 32) | (1ULL << 48),
                              2,
                              501ULL | (31ULL << 32),
                              0,
                              1,
                              2,
                              3,
                              4,
                              5,
                              6,
                              7,
                              8,
                              6,
                              1,
                              512,
                              9,
                              0,
                              0,
                              0xa5a5a5a5a5a5a5a5ULL};
  std::array<uint8_t, 160> Target{};
  for (unsigned I = 0; I != 20; ++I)
    set(Target, I * 8, Words[I]);
  std::string Out;
  auto append = [&](const auto &Record) {
    Out.append(reinterpret_cast<const char *>(Record.data()), Record.size());
  };
  append(Link);
  append(Target);
  set(Target, 28, 20, 4);
  set(Target, 72, 81);
  set(Target, 80, 82);
  append(Target);
  set(Link, 14, 2, 2);
  append(Link);
  append(Link);
  set(Link, 14, 0, 2);
  append(Link);
  auto Replacement = symbolicOwnershipRecord(0, true);
  set(Replacement, 16, Kind >= 2 ? 101 : 100);
  set(Replacement, 72, 101);
  set(Replacement, 80, 102);
  append(Replacement);
  append(Link);
  return Out + "W";
}
struct SymbolicOwnershipService {
  uint64_t Number, Value;
  bool Error;
  std::array<std::optional<uint64_t>, 6> Arguments;
};
inline std::vector<SymbolicOwnershipService>
symbolicOwnershipServices(unsigned Kind) {
  std::vector<SymbolicOwnershipService> R;
  const uint64_t UID = 501, GID = Kind & 1 ? 20 : Kind >= 2 ? 20 : 31;
  constexpr uint64_t High = 0x13579bdf00000000ULL, None = 0xffffffffULL;
  auto add =
      [&](uint64_t Number, uint64_t Value = 0, bool Error = false,
          std::initializer_list<std::pair<unsigned, uint64_t>> Args = {}) {
        SymbolicOwnershipService S{Number, Value, Error, {}};
        for (const auto &[I, V] : Args)
          S.Arguments[I] = V;
        R.push_back(S);
      };
  auto open = [&](uint64_t FD, uint64_t Flags = 0x200000) {
    add(5, FD, false, {{1, Flags}, {2, 0}});
  };
  auto stat = [&](uint64_t FD) { add(339, 0, false, {{0, FD}}); };
  auto own = [&](uint64_t FD, uint64_t U, uint64_t G) {
    add(123, 0, false, {{0, FD}, {1, U}, {2, G}});
  };
  auto emit = [&] {
    if (Kind & 1)
      add(4, 160, false, {{0, 1}, {2, 160}});
  };
  if (Kind >= 2)
    add(57);
  open(3);
  add(41, 4, false, {{0, 3}});
  open(5);
  open(6, 0);
  stat(3);
  stat(6);
  own(3, None, None);
  stat(3);
  own(4, UID, GID);
  stat(5);
  stat(6);
  emit();
  emit();
  add(High | 468, 0, false,
      {{0, High | 0xfffffffeULL},
       {2, High | UID},
       {3, High | GID},
       {4, High | 0x20}});
  stat(3);
  add(16, 0, false, {{1, None}, {2, GID}});
  stat(6);
  emit();
  add(471, 0, false, {{0, 0xfffffffeULL}, {2, 0xfffffffeULL}, {4, 0}});
  open(7);
  add(High | 123, 0, false, {{0, High | 7}, {1, High | UID}, {2, High | GID}});
  stat(3);
  emit();
  add(128);
  add(468, 2, true, {{0, UINT64_MAX}, {2, UID}, {3, GID}, {4, 0x20}});
  add(468, 0, false, {{0, UINT64_MAX}, {2, UID}, {3, GID}, {4, 0x20}});
  stat(7);
  emit();
  add(10);
  add(10);
  own(3, UID, GID);
  stat(4);
  emit();
  add(57);
  open(8);
  stat(8);
  own(8, UID, GID);
  stat(8);
  emit();
  own(5, UID, GID);
  stat(8);
  stat(3);
  emit();
  add(57);
  open(9);
  own(9, UID, GID);
  add(16, 2, true, {{1, UID}, {2, GID}});
  add(468, 0, false, {{0, UINT64_MAX}, {2, None}, {3, None}, {4, 0x20}});
  add(57);
  add(16, 62, true, {{1, UID}, {2, GID}});
  open(10);
  own(10, None, None);
  add(123, 9, true, {{0, UINT64_MAX}, {1, UID}, {2, GID}});
  add(468, 22, true, {{0, UINT64_MAX}, {1, 1}, {2, UID}, {3, GID}, {4, 8}});
  add(6, 0, false, {{0, 3}});
  add(123, 9, true, {{0, 3}, {1, UID}, {2, GID}});
  for (uint64_t FD = 4; FD <= 10; ++FD)
    add(6, 0, false, {{0, FD}});
  add(10);
  add(10);
  add(10);
  add(4, 1, false, {{0, 1}, {2, 1}});
  return R;
}

} // namespace neverd::emulation::darwin_test
#endif
