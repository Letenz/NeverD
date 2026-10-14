//===- DarwinModeTestData.h - Declared ordinary mode observations ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TESTS_DARWINMODETESTDATA_H
#define NEVERD_TESTS_DARWINMODETESTDATA_H

#include "neverd/emulation/DarwinFileOptions.h"
#include "neverd/emulation/DarwinSystemOptions.h"

namespace neverd::emulation::darwin_test {
inline DarwinFileMetadata modeMetadata(uint16_t Mode, uint64_t Inode,
                                       uint64_t Size = 0) {
  DarwinFileMetadata M;
  M.Device = 7;
  M.Inode = Inode;
  M.Mode = Mode;
  M.LinkCount = (Mode & 0170000) == 0040000 ? 2 : 1;
  M.UID = 501;
  M.GID = 20;
  M.Size = Size;
  M.BlockSize = 512;
  M.Blocks = Size ? 1 : 0;
  M.Generation = 9;
  M.AccessTime = {1, 2};
  M.ModificationTime = {3, 4};
  M.ChangeTime = {5, 6};
  M.BirthTime = {7, 8};
  return M;
}
inline DarwinCredentials modeCredentials() {
  DarwinCredentials C;
  C.RealUID = 700;
  C.EffectiveUID = 501;
  C.RealGID = 31;
  C.EffectiveGID = 20;
  return C;
}
inline DarwinFileOptions modeOptions() {
  DarwinFileOptions O;
  O.Files["/data"] = {'a', 'b', 0, 255, 'e', 'f'};
  O.Metadata["/data"] = modeMetadata(0100600, 2, 6);
  O.Metadata["/"] = modeMetadata(0040700, 1);
  O.WorkingDirectory = "/";
  O.ModeMutations.emplace().Objects["/data"] = {71, 72};
  return O;
}
inline constexpr char ModeCredentialsJSON[] = R"({"credentials":{
  "real_uid":700,"effective_uid":501,"real_gid":31,"effective_gid":20}})";
inline constexpr char ModeFilesJSON[] = R"({
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
  "mode_mutations":{"objects":[{"path":"/data",
    "time":{"seconds":71,"nanoseconds":72}}]}})";
} // namespace neverd::emulation::darwin_test
#endif
