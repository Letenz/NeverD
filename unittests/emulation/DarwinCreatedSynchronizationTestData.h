//===- DarwinCreatedSynchronizationTestData.h - Created completion --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// Original independent creation providers and literal raw-call expectations.
#ifndef NEVERD_TESTS_DARWINCREATEDSYNCHRONIZATIONTESTDATA_H
#define NEVERD_TESTS_DARWINCREATEDSYNCHRONIZATIONTESTDATA_H
#include "DarwinModeTestData.h"
namespace neverd::emulation::darwin_test {
inline DarwinFileOptions createdSynchronizationOptions(bool File = true,
                                                       bool Data = true) {
  auto O = modeOptions();
  O.ModeMutations.reset();
  O.MutableDirectories.insert("/");
  O.DirectoryMutationPolicies["/"] = {16, {91, 92}};
  O.InitialUmask = 022;
  O.CreationPolicy =
      DarwinFileCreationPolicy{100,
                               512,
                               9,
                               {41, 42},
                               {512, {101, 102}},
                               DarwinNamespaceCreationPolicy{512, 32, 7}};
  O.Synchronization.emplace().CreatedRegularObjects = File;
  O.DataSynchronization.emplace().CreatedRegularObjects = Data;
  return O;
}
inline constexpr uint64_t CreatedSynchronizationPrefixNumbers[] = {
    5, 4, 5, 41, 92, 90, 199, 92, 339};
inline constexpr uint64_t CreatedSynchronizationPrefixValues[] = {
    3, 10, 4, 5, 10, 20, 3, 0x10002, 0};
inline constexpr uint64_t CreatedSynchronizationTailNumbers[] = {
    201, 339, 95,  408, 187, 339, 339, 339, 6,   95, 408, 187, 95, 408, 187,
    6,   95,  408, 187, 95,  408, 187, 339, 199, 3,  6,   6,   6,  10,  4};
inline constexpr uint64_t CreatedSynchronizationTailValues[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 9, 9, 9, 0, 0, 0,
    0, 9, 9, 9, 0, 0, 0, 0, 0, 6, 0, 0, 0, 0, 1};
inline constexpr char CreatedSynchronizationFilesJSON[] = R"({
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
      }
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
      },
      "mutable": true,
      "mutation_policy": {
        "directory_entry_size": 16,
        "mutation_time": {
          "seconds": 91,
          "nanoseconds": 92
        }
      }
    }
  ],
  "working_directory": "/",
  "data_synchronization": {
    "objects": [],
    "created_regular_objects": true
  },
  "umask": 18,
  "creation_policy": {
    "first_inode": 100,
    "block_size": 512,
    "generation": 9,
    "creation_time": {
      "seconds": 41,
      "nanoseconds": 42
    },
    "mutation_policy": {
      "allocation_unit": 512,
      "mutation_time": {
        "seconds": 101,
        "nanoseconds": 102
      }
    },
    "namespace_policy": {
      "symbolic_link_allocation_unit": 512,
      "directory_entry_size": 32,
      "directory_blocks": 7
    }
  },
  "synchronization": {
    "objects": [],
    "created_regular_objects": true
  }
})";
} // namespace neverd::emulation::darwin_test
#endif
