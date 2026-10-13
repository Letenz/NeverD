//===- DarwinFileSystemTestData.h - Independent mount-stat facts --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TESTS_DARWINFILESYSTEMTESTDATA_H
#define NEVERD_TESTS_DARWINFILESYSTEMTESTDATA_H
#include "DarwinCreatedSynchronizationTestData.h"
#include "DarwinModeTestData.h"

#include <algorithm>
#include <array>
#include <vector>

namespace neverd::emulation::darwin_test {
inline DarwinFileSystemSnapshot fileSystemSnapshot() {
  return {0x10203040,
          -4096,
          0xfedcba9876543210ULL,
          0x0102030405060708ULL,
          0,
          UINT64_MAX,
          9,
          {-1, INT32_MIN},
          0xfffffffe,
          26,
          0x01020304,
          7,
          "virtfs",
          "/虚拟",
          "neverd:declared",
          0x87654321};
}
inline DarwinFileOptions fileSystemOptions() {
  auto O = modeOptions();
  O.ModeMutations.reset();
  auto &F = O.FileSystemObservations.emplace();
  F.Objects["/data"] = F.Objects["/"] = fileSystemSnapshot();
  return O;
}
inline std::array<uint8_t, 2200> fileSystemRecord() {
  // Literal ABI bytes, independently authored from the pinned LP64 layout.
  // Sixteen bytes on either side remain untouched. No production serializer.
  constexpr uint8_t Prefix[] = {
      0x40, 0x30, 0x20, 0x10, 0x00, 0xf0, 0xff, 0xff, 0x10, 0x32, 0x54, 0x76,
      0x98, 0xba, 0xdc, 0xfe, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
      0,    0,    0,    0,    0,    0,    0,    0,    0xff, 0xff, 0xff, 0xff,
      0xff, 0xff, 0xff, 0xff, 9,    0,    0,    0,    0,    0,    0,    0,
      0xff, 0xff, 0xff, 0xff, 0,    0,    0,    0x80, 0xfe, 0xff, 0xff, 0xff,
      26,   0,    0,    0,    4,    3,    2,    1,    7,    0,    0,    0};
  std::array<uint8_t, 2200> R{};
  std::fill_n(R.begin(), 16, 0xa5);
  std::fill_n(R.end() - 16, 16, 0xa5);
  std::copy(std::begin(Prefix), std::end(Prefix), R.begin() + 16);
  constexpr char Type[] = "virtfs", Mount[] = "/虚拟",
                 From[] = "neverd:declared";
  std::copy(std::begin(Type), std::end(Type), R.begin() + 16 + 72);
  std::copy(std::begin(Mount), std::end(Mount), R.begin() + 16 + 88);
  std::copy(std::begin(From), std::end(From), R.begin() + 16 + 1112);
  R[16 + 2136] = 0x21;
  R[16 + 2137] = 0x43;
  R[16 + 2138] = 0x65;
  R[16 + 2139] = 0x87;
  return R;
}
inline constexpr char FileSystemSnapshotJSON[] = R"({
  "block_size":270544960,"io_size":-4096,"blocks":"18364758544493064720",
  "free_blocks":"72623859790382856","available_blocks":0,
  "files":"18446744073709551615","free_files":9,
  "filesystem_id":[-1,-2147483648],"owner":4294967294,"type":26,
  "flags":16909060,"subtype":7,"type_name":"virtfs","mount_point":"/虚拟",
  "mounted_from":"neverd:declared","extended_flags":2271560481})";
inline DarwinFileOptions createdFileSystemOptions() {
  auto O = createdSynchronizationOptions(false, false);
  O.Synchronization.reset();
  O.DataSynchronization.reset();
  O.FileSystemObservations.emplace().Objects["/"] = fileSystemSnapshot();
  O.FileSystemObservations->CreatedRegularObjects = true;
  return O;
}
struct FileSystemService {
  uint64_t Number, Value;
  bool Error = false;
  std::optional<uint64_t> Object = std::nullopt, Output = std::nullopt;
};
inline constexpr char FileSystemFilesJSON[] = R"({
  "files": [
    {
      "path": "/data",
      "bytes_hex": "616200ff6566",
      "metadata": {
        "device": 7,
        "inode": 2,
        "mode": 33152,
        "link_count": 1,
        "uid": 501,
        "gid": 20,
        "size": 6,
        "block_size": 512,
        "blocks": 1,
        "flags": 0,
        "generation": 9,
        "access_time": {
          "seconds": 1,
          "nanoseconds": 2
        },
        "modification_time": {
          "seconds": 3,
          "nanoseconds": 4
        },
        "change_time": {
          "seconds": 5,
          "nanoseconds": 6
        },
        "birth_time": {
          "seconds": 7,
          "nanoseconds": 8
        }
      },
      "writable": true
    }
  ],
  "directories": [
    {
      "path": "/",
      "metadata": {
        "device": 7,
        "inode": 1,
        "mode": 16832,
        "link_count": 2,
        "uid": 501,
        "gid": 20,
        "size": 0,
        "block_size": 512,
        "blocks": 0,
        "flags": 0,
        "generation": 9,
        "access_time": {
          "seconds": 1,
          "nanoseconds": 2
        },
        "modification_time": {
          "seconds": 3,
          "nanoseconds": 4
        },
        "change_time": {
          "seconds": 5,
          "nanoseconds": 6
        },
        "birth_time": {
          "seconds": 7,
          "nanoseconds": 8
        }
      }
    }
  ],
  "working_directory": "/",
  "filesystem_observations": {
    "objects": [
      {
        "path": "/data",
        "snapshot": {
          "block_size": 270544960,
          "io_size": -4096,
          "blocks": "18364758544493064720",
          "free_blocks": "72623859790382856",
          "available_blocks": 0,
          "files": "18446744073709551615",
          "free_files": 9,
          "filesystem_id": [
            -1,
            -2147483648
          ],
          "owner": 4294967294,
          "type": 26,
          "flags": 16909060,
          "subtype": 7,
          "type_name": "virtfs",
          "mount_point": "/虚拟",
          "mounted_from": "neverd:declared",
          "extended_flags": 2271560481
        }
      },
      {
        "path": "/",
        "snapshot": {
          "block_size": 270544960,
          "io_size": -4096,
          "blocks": "18364758544493064720",
          "free_blocks": "72623859790382856",
          "available_blocks": 0,
          "files": "18446744073709551615",
          "free_files": 9,
          "filesystem_id": [
            -1,
            -2147483648
          ],
          "owner": 4294967294,
          "type": 26,
          "flags": 16909060,
          "subtype": 7,
          "type_name": "virtfs",
          "mount_point": "/虚拟",
          "mounted_from": "neverd:declared",
          "extended_flags": 2271560481
        }
      }
    ]
  }
})";
inline std::vector<FileSystemService> fileSystemServices() {
  std::vector<FileSystemService> R = {{5, 3},
                                      {5, 4},
                                      {41, 5, false, 3},
                                      {5, 6},
                                      {199, 2, false, 5},
                                      {92, 0, false, 3},
                                      {339, 0, false, 3}};
  for (uint64_t H : {0ULL, 0x1234567800000000ULL, 0xffffffff00000000ULL}) {
    R.push_back({H | 345, 0});
    for (uint64_t FD : {3u, 4u, 5u, 6u})
      R.push_back({H | 346, 0, false, H | FD});
    for (const auto &S : std::vector<FileSystemService>{{339, 0, false, 3},
                                                        {199, 2, false, 5},
                                                        {199, 0, false, 4},
                                                        {92, 0, false, 3},
                                                        {92, 1, false, 4}})
      R.push_back(S);
  }
  for (const auto &S :
       std::vector<FileSystemService>{{345, 2, true, std::nullopt, 1},
                                      {346, 9, true, UINT64_MAX, 1},
                                      {346, 14, true, 3, 1},
                                      {346, 14, true, 3, 0},
                                      {345, 14, true, std::nullopt, 1},
                                      {339, 0, false, 4},
                                      {6, 0, false, 3},
                                      {346, 9, true, 3, 1},
                                      {6, 0, false, 5},
                                      {6, 0, false, 4},
                                      {6, 0, false, 6},
                                      {4, 1, false, 1}})
    R.push_back(S);
  return R;
}
inline std::vector<FileSystemService> createdFileSystemServices() {
  return {{5, 3},
          {346, 0, false, 3},
          {41, 4, false, 3},
          {346, 0, false, 4},
          {6, 0, false, 3},
          {346, 9, true, 3, 1},
          {6, 0, false, 4},
          {4, 2200, false, 1},
          {4, 2200, false, 1}};
}
inline std::array<uint8_t, 4400> createdFileSystemRecords() {
  const auto Record = fileSystemRecord();
  std::array<uint8_t, 4400> R;
  std::copy(Record.begin(), Record.end(), R.begin());
  std::copy(Record.begin(), Record.end(), R.begin() + 2200);
  return R;
}

inline DarwinFileOptions mountEnumerationOptions() {
  DarwinFileOptions O;
  const auto S = fileSystemSnapshot();
  O.FileSystemEnumeration.emplace().Mounts = {{S, DarwinFileSystemSnapshot{}},
                                              {DarwinFileSystemSnapshot{}, S},
                                              {S, DarwinFileSystemSnapshot{}}};
  return O;
}
inline std::string mountEnumerationFilesJSON() {
  const std::string S = FileSystemSnapshotJSON;
  const std::string Z = R"({"block_size":0,"io_size":0,"blocks":0,
    "free_blocks":0,"available_blocks":0,"files":0,"free_files":0,
    "filesystem_id":[0,0],"owner":0,"type":0,"flags":0,"subtype":0,
    "type_name":"","mount_point":"","mounted_from":"","extended_flags":0})";
  return "{\"files\":[],\"filesystem_enumeration\":{\"mounts\":[{\"cached\":" +
         S + ",\"refreshed\":" + Z + "},{\"cached\":" + Z +
         ",\"refreshed\":" + S + "},{\"cached\":" + S + ",\"refreshed\":" + Z +
         "}]}}";
}
inline std::string mountEnumerationOutput() {
  const auto Literal = fileSystemRecord();
  std::string Result;
  for (bool Refreshed : {false, true, true}) {
    std::array<uint8_t, 6536> B{};
    std::fill_n(B.begin(), 16, 0xa5);
    std::fill_n(B.end() - 16, 16, 0xa5);
    for (unsigned I = 0; I != 3; ++I)
      if ((I == 1) == Refreshed)
        std::copy(Literal.begin() + 16, Literal.end() - 16,
                  B.begin() + 16 + I * 2168);
    Result.append(reinterpret_cast<const char *>(B.data()), B.size());
  }
  return Result + "M";
}
struct MountEnumerationService {
  uint64_t Number, Value;
  bool Error;
  std::array<std::optional<uint64_t>, 6> Arguments;
};
inline std::vector<MountEnumerationService>
mountEnumerationServices(bool Virtual) {
  std::vector<MountEnumerationService> R;
  const auto Call = [&](uint64_t Number, std::optional<uint64_t> Out,
                        uint64_t Size, uint64_t Flags, uint64_t Value,
                        bool Error = false) {
    R.push_back({Number,
                 Value,
                 Error,
                 {Out, Size, Flags, UINT64_MAX, UINT64_MAX, UINT64_MAX}});
  };
  const auto Emit = [&] {
    if (Virtual)
      R.push_back({4, 6536, false, {1, std::nullopt, 6536, 0, 0, 0}});
  };
  Call(347, 0, 0, 2, 3);
  Call(347, 0, UINT64_MAX, 0, 3);
  Call(347, 1, 0, 0, 0);
  Call(347, 1, 2167, 4, 0);
  Call(347, std::nullopt, 4335, 2, 1);
  Call(347, std::nullopt, 4336, 2, 2);
  Call(347, std::nullopt, 6504, 2, 3);
  Emit();
  Call(347, 1, 0xffffffff, 2, 14, true);
  Call(347, std::nullopt, 6504, 1, 3);
  Emit();
  Call(347, std::nullopt, 6504, 2, 3);
  Emit();
  for (uint64_t H : {0ULL, 0x1234567800000000ULL, 0xffffffff00000000ULL})
    Call(H | 347, std::nullopt, H | 4336, H | 2, 2);
  Call(347, 1, 2168, 1, 14, true);
  Call(347, 0, 2168, 2, 3);
  R.push_back({4, 1, false, {1, std::nullopt, 1, 0, 0, 0}});
  return R;
}

} // namespace neverd::emulation::darwin_test
#endif
