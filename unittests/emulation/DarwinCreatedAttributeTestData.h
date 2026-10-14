//===- DarwinCreatedAttributeTestData.h - Created ordinary attributes ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// Independently authored provider data and raw syscall expectations.
#ifndef NEVERD_TESTS_DARWINCREATEDATTRIBUTETESTDATA_H
#define NEVERD_TESTS_DARWINCREATEDATTRIBUTETESTDATA_H
#include "DarwinCreatedSynchronizationTestData.h"

#include <optional>
namespace neverd::emulation::darwin_test {
inline DarwinCredentials createdAttributeCredentials() {
  auto C = modeCredentials();
  C.GroupAccessList = {20, 31};
  return C;
}
inline constexpr char CreatedAttributeCredentialsJSON[] = R"({"credentials":{
  "real_uid":700,"effective_uid":501,"real_gid":31,"effective_gid":20,
  "groups":[20,31]}})";
inline DarwinFileOptions createdAttributeOptions(bool Mode = true,
                                                 bool Owner = true) {
  auto O = createdSynchronizationOptions(false, false);
  O.Synchronization.reset();
  O.DataSynchronization.reset();
  O.ModeMutations.emplace().CreatedRegularObjects = Mode;
  O.OwnershipMutations.emplace().CreatedRegularObjects = Owner;
  return O;
}
struct CreatedAttributeService {
  uint64_t Number, Value;
  std::optional<uint64_t> FD;
  bool Error;
};
inline constexpr CreatedAttributeService CreatedAttributeServices[] = {
    {0x19ULL, 0x1f5ULL, std::nullopt, false},
    {0x2bULL, 0x14ULL, std::nullopt, false},
    {0x5ULL, 0x3ULL, std::nullopt, false},
    {0x4ULL, 0xaULL, 0x3ULL, false},
    {0x5ULL, 0x4ULL, std::nullopt, false},
    {0x29ULL, 0x5ULL, 0x3ULL, false},
    {0x5cULL, 0xaULL, 0x3ULL, false},
    {0x5aULL, 0x14ULL, 0x3ULL, false},
    {0xc7ULL, 0x3ULL, 0x4ULL, false},
    {0x5cULL, 0x10002ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x7cULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0xfULL, 0x0ULL, std::nullopt, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0x123456780000000fULL, 0x0ULL, std::nullopt, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0xffffffff0000000fULL, 0x0ULL, std::nullopt, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0x7cULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0x123456780000007cULL, 0x0ULL, 0x1234567800000004ULL, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0xffffffff0000007cULL, 0x0ULL, 0xffffffff00000004ULL, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0x1d3ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0x12345678000001d3ULL, 0x0ULL, 0x1234567800000004ULL, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0xffffffff000001d3ULL, 0x0ULL, 0xffffffff00000004ULL, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0x10ULL, 0x0ULL, std::nullopt, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x10ULL, 0x0ULL, std::nullopt, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0x10ULL, 0x0ULL, std::nullopt, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0x10ULL, 0x0ULL, std::nullopt, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0x1234567800000010ULL, 0x0ULL, std::nullopt, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x1234567800000010ULL, 0x0ULL, std::nullopt, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0x1234567800000010ULL, 0x0ULL, std::nullopt, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0x1234567800000010ULL, 0x0ULL, std::nullopt, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0xffffffff00000010ULL, 0x0ULL, std::nullopt, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0xffffffff00000010ULL, 0x0ULL, std::nullopt, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0xffffffff00000010ULL, 0x0ULL, std::nullopt, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0xffffffff00000010ULL, 0x0ULL, std::nullopt, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0x7bULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x7bULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0x7bULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0x7bULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0x123456780000007bULL, 0x0ULL, 0x1234567800000004ULL, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x123456780000007bULL, 0x0ULL, 0x1234567800000004ULL, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0x123456780000007bULL, 0x0ULL, 0x1234567800000004ULL, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0x123456780000007bULL, 0x0ULL, 0x1234567800000004ULL, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0xffffffff0000007bULL, 0x0ULL, 0xffffffff00000004ULL, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0xffffffff0000007bULL, 0x0ULL, 0xffffffff00000004ULL, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0xffffffff0000007bULL, 0x0ULL, 0xffffffff00000004ULL, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0xffffffff0000007bULL, 0x0ULL, 0xffffffff00000004ULL, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0x1d4ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x1d4ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0x1d4ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0x1d4ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0x12345678000001d4ULL, 0x0ULL, 0x1234567800000004ULL, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x12345678000001d4ULL, 0x0ULL, 0x1234567800000004ULL, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0x12345678000001d4ULL, 0x0ULL, 0x1234567800000004ULL, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0x12345678000001d4ULL, 0x0ULL, 0x1234567800000004ULL, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0xffffffff000001d4ULL, 0x0ULL, 0xffffffff00000004ULL, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0xffffffff000001d4ULL, 0x0ULL, 0xffffffff00000004ULL, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0xffffffff000001d4ULL, 0x0ULL, 0xffffffff00000004ULL, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0xffffffff000001d4ULL, 0x0ULL, 0xffffffff00000004ULL, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x5ULL, false},
    {0xfULL, 0xeULL, std::nullopt, true},
    {0x7cULL, 0x9ULL, 0xffffffffffffffffULL, true},
    {0x1d3ULL, 0x16ULL, 0xffffffffffffffffULL, true},
    {0x1d3ULL, 0x9ULL, 0xffffffffffffffffULL, true},
    {0x10ULL, 0xeULL, std::nullopt, true},
    {0x7bULL, 0x9ULL, 0xffffffffffffffffULL, true},
    {0x1d4ULL, 0x16ULL, 0xffffffffffffffffULL, true},
    {0x1d4ULL, 0x9ULL, 0xffffffffffffffffULL, true},
    {0x153ULL, 0x0ULL, 0x14ULL, false},
    {0x5cULL, 0x10002ULL, 0x3ULL, false},
    {0x5cULL, 0x0ULL, 0x4ULL, false},
    {0xc7ULL, 0xaULL, 0x3ULL, false},
    {0xc7ULL, 0xaULL, 0x5ULL, false},
    {0xc7ULL, 0x3ULL, 0x4ULL, false},
    {0x9ULL, 0x0ULL, std::nullopt, false},
    {0x5ULL, 0x6ULL, std::nullopt, false},
    {0x80ULL, 0x0ULL, std::nullopt, false},
    {0x7cULL, 0x0ULL, 0x6ULL, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x7bULL, 0x0ULL, 0x6ULL, false},
    {0x153ULL, 0x0ULL, 0x6ULL, false},
    {0xaULL, 0x0ULL, std::nullopt, false},
    {0xaULL, 0x0ULL, std::nullopt, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x5ULL, 0x7ULL, std::nullopt, false},
    {0x7cULL, 0x0ULL, 0x7ULL, false},
    {0x7bULL, 0x0ULL, 0x7ULL, false},
    {0x153ULL, 0x0ULL, 0x7ULL, false},
    {0x6ULL, 0x0ULL, 0x7ULL, false},
    {0x7cULL, 0x9ULL, 0x7ULL, true},
    {0x7bULL, 0x9ULL, 0x7ULL, true},
    {0x6ULL, 0x0ULL, 0x3ULL, false},
    {0x7cULL, 0x9ULL, 0x3ULL, true},
    {0x7bULL, 0x9ULL, 0x3ULL, true},
    {0x7cULL, 0x0ULL, 0x5ULL, false},
    {0x7bULL, 0x0ULL, 0x5ULL, false},
    {0x6ULL, 0x0ULL, 0x5ULL, false},
    {0x6ULL, 0x0ULL, 0xaULL, false},
    {0x6ULL, 0x0ULL, 0x14ULL, false},
    {0x7cULL, 0x0ULL, 0x4ULL, false},
    {0x7bULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, false},
    {0xc7ULL, 0x0ULL, 0x4ULL, false},
    {0x3ULL, 0xaULL, 0x4ULL, false},
    {0x6ULL, 0x0ULL, 0x6ULL, false},
    {0x6ULL, 0x0ULL, 0x4ULL, false},
    {0xaULL, 0x0ULL, std::nullopt, false},
    {0x4ULL, 0x1ULL, 0x1ULL, false},
};
inline constexpr CreatedAttributeService VirtualCreatedAttributeServices[] = {
    {0x5ULL, 0x3ULL, std::nullopt, false}, {0x153ULL, 0x0ULL, 0x3ULL, false},
    {0x7cULL, 0x0ULL, 0x3ULL, false},      {0x7bULL, 0x0ULL, 0x3ULL, false},
    {0x153ULL, 0x0ULL, 0x3ULL, false},     {0x4ULL, 0xa0ULL, 0x1ULL, false},
    {0x4ULL, 0xa0ULL, 0x1ULL, false},      {0x6ULL, 0x0ULL, 0x3ULL, false},
    {0xaULL, 0x0ULL, std::nullopt, false},
};
inline constexpr char VirtualCreatedAttributeStdoutHex[] =
    "a5a5a5a5a5a5a5a507000000808101006400000000000000f5010000140000000000000000"
    "000000"
    "29000000000000002a0000000000000029000000000000002a000000000000002900000000"
    "000000"
    "2a0000000000000029000000000000002a0000000000000000000000000000000000000000"
    "000000"
    "0002000000000000090000000000000000000000000000000000000000000000a5a5a5a5a5"
    "a5a5a5"
    "a5a5a5a5a5a5a5a507000000a08101006400000000000000f50100001f0000000000000000"
    "000000"
    "29000000000000002a0000000000000029000000000000002a000000000000006500000000"
    "000000"
    "660000000000000029000000000000002a0000000000000000000000000000000000000000"
    "000000"
    "0002000000000000090000000000000000000000000000000000000000000000a5a5a5a5a5"
    "a5a5a5";
inline constexpr char CreatedAttributeFilesJSON[] = R"({
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
  "mode_mutations": {
    "objects": [],
    "created_regular_objects": true
  },
  "ownership_mutations": {
    "objects": [],
    "created_regular_objects": true
  }
})";
} // namespace neverd::emulation::darwin_test
#endif
