//===- DarwinCreatedAdvisoryLockTestData.h - Created ordinary locks ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// Original creation provider fixture and independently authored raw outcomes.
#ifndef NEVERD_TESTS_DARWINCREATEDADVISORYLOCKTESTDATA_H
#define NEVERD_TESTS_DARWINCREATEDADVISORYLOCKTESTDATA_H
#include "DarwinCreatedSynchronizationTestData.h"
namespace neverd::emulation::darwin_test {
inline DarwinFileOptions createdAdvisoryLockOptions(bool Created = true) {
  auto O = createdSynchronizationOptions(false, false);
  O.Synchronization.reset();
  O.DataSynchronization.reset();
  O.AdvisoryLocks.emplace().CreatedRegularObjects = Created;
  return O;
}
struct CreatedAdvisoryService {
  uint64_t Number, Value, FD, How;
  bool Error;
};
inline constexpr CreatedAdvisoryService CreatedAdvisoryServices[] = {
    {0x5ULL, 0x3ULL, UINT64_MAX, UINT64_MAX, false},
    {0x4ULL, 0xaULL, 0x3ULL, UINT64_MAX, false},
    {0x5ULL, 0x4ULL, UINT64_MAX, UINT64_MAX, false},
    {0x29ULL, 0x5ULL, 0x3ULL, UINT64_MAX, false},
    {0x5cULL, 0xaULL, 0x3ULL, UINT64_MAX, false},
    {0x5aULL, 0x14ULL, 0x3ULL, UINT64_MAX, false},
    {0xc7ULL, 0x3ULL, 0x4ULL, UINT64_MAX, false},
    {0x5cULL, 0x0ULL, 0x3ULL, UINT64_MAX, false},
    {0x5cULL, 0x10002ULL, 0x3ULL, UINT64_MAX, false},
    {0x153ULL, 0x0ULL, 0x3ULL, UINT64_MAX, false},
    {0x83ULL, 0x0ULL, 0x3ULL, 0x6ULL, false},
    {0x5cULL, 0x14002ULL, 0x3ULL, UINT64_MAX, false},
    {0x5cULL, 0x14002ULL, 0x5ULL, UINT64_MAX, false},
    {0x5cULL, 0x0ULL, 0x4ULL, UINT64_MAX, false},
    {0x83ULL, 0x23ULL, 0x4ULL, 0x5ULL, true},
    {0x5cULL, 0x0ULL, 0x4ULL, UINT64_MAX, false},
    {0x83ULL, 0x0ULL, 0x5ULL, 0x8ULL, false},
    {0x5cULL, 0x0ULL, 0x3ULL, UINT64_MAX, false},
    {0x5cULL, 0x14002ULL, 0x3ULL, UINT64_MAX, false},
    {0x83ULL, 0x0ULL, 0x3ULL, 0x1ULL, false},
    {0x83ULL, 0x0ULL, 0x4ULL, 0x1ULL, false},
    {0x5cULL, 0x4000ULL, 0x4ULL, UINT64_MAX, false},
    {0x83ULL, 0x23ULL, 0x3ULL, 0x6ULL, true},
    {0x83ULL, 0x0ULL, 0x4ULL, 0x8ULL, false},
    {0x83ULL, 0x23ULL, 0x4ULL, 0x6ULL, true},
    {0x83ULL, 0x0ULL, 0xaULL, 0x6ULL, false},
    {0x83ULL, 0x0ULL, 0x14ULL, 0x8ULL, false},
    {0x83ULL, 0x0ULL, 0x4ULL, 0x6ULL, false},
    {0x83ULL, 0x0ULL, 0x4ULL, 0x8ULL, false},
    {0x83ULL, 0x9ULL, 0x3ULL, 0x0ULL, true},
    {0x83ULL, 0x9ULL, 0xffffffffULL, 0x6ULL, true},
    {0x153ULL, 0x0ULL, 0x3ULL, UINT64_MAX, false},
    {0x153ULL, 0x0ULL, 0x4ULL, UINT64_MAX, false},
    {0x153ULL, 0x0ULL, 0x5ULL, UINT64_MAX, false},
    {0xc7ULL, 0xaULL, 0x3ULL, UINT64_MAX, false},
    {0xc7ULL, 0x3ULL, 0x4ULL, UINT64_MAX, false},
    {0x5cULL, 0x14002ULL, 0x3ULL, UINT64_MAX, false},
    {0x5cULL, 0x4000ULL, 0x4ULL, UINT64_MAX, false},
    {0x1234567800000083ULL, 0x0ULL, 0x1234567800000003ULL,
     0x1234567800000006ULL, false},
    {0x5cULL, 0x14002ULL, 0x3ULL, UINT64_MAX, false},
    {0x5cULL, 0x14002ULL, 0x5ULL, UINT64_MAX, false},
    {0x5cULL, 0x4000ULL, 0x4ULL, UINT64_MAX, false},
    {0x1234567800000083ULL, 0x23ULL, 0x1234567800000004ULL,
     0x1234567800000005ULL, true},
    {0x5cULL, 0x4000ULL, 0x4ULL, UINT64_MAX, false},
    {0x1234567800000083ULL, 0x0ULL, 0x1234567800000005ULL,
     0x1234567800000008ULL, false},
    {0x5cULL, 0x0ULL, 0x3ULL, UINT64_MAX, false},
    {0x5cULL, 0x14002ULL, 0x3ULL, UINT64_MAX, false},
    {0x1234567800000083ULL, 0x0ULL, 0x1234567800000003ULL,
     0x1234567800000001ULL, false},
    {0x1234567800000083ULL, 0x0ULL, 0x1234567800000004ULL,
     0x1234567800000001ULL, false},
    {0x5cULL, 0x4000ULL, 0x4ULL, UINT64_MAX, false},
    {0x1234567800000083ULL, 0x23ULL, 0x1234567800000003ULL,
     0x1234567800000006ULL, true},
    {0x1234567800000083ULL, 0x0ULL, 0x1234567800000004ULL,
     0x1234567800000008ULL, false},
    {0x1234567800000083ULL, 0x23ULL, 0x1234567800000004ULL,
     0x1234567800000006ULL, true},
    {0x1234567800000083ULL, 0x0ULL, 0x123456780000000aULL,
     0x1234567800000006ULL, false},
    {0x1234567800000083ULL, 0x0ULL, 0x1234567800000014ULL,
     0x1234567800000008ULL, false},
    {0x1234567800000083ULL, 0x0ULL, 0x1234567800000004ULL,
     0x1234567800000006ULL, false},
    {0x1234567800000083ULL, 0x0ULL, 0x1234567800000004ULL,
     0x1234567800000008ULL, false},
    {0x1234567800000083ULL, 0x9ULL, 0x1234567800000003ULL,
     0x1234567800000000ULL, true},
    {0x1234567800000083ULL, 0x9ULL, 0x12345678ffffffffULL,
     0x1234567800000006ULL, true},
    {0x153ULL, 0x0ULL, 0x3ULL, UINT64_MAX, false},
    {0x153ULL, 0x0ULL, 0x4ULL, UINT64_MAX, false},
    {0x153ULL, 0x0ULL, 0x5ULL, UINT64_MAX, false},
    {0xc7ULL, 0xaULL, 0x3ULL, UINT64_MAX, false},
    {0xc7ULL, 0x3ULL, 0x4ULL, UINT64_MAX, false},
    {0x5cULL, 0x14002ULL, 0x3ULL, UINT64_MAX, false},
    {0x5cULL, 0x4000ULL, 0x4ULL, UINT64_MAX, false},
    {0xffffffff00000083ULL, 0x0ULL, 0xffffffff00000003ULL,
     0xffffffff00000006ULL, false},
    {0x5cULL, 0x14002ULL, 0x3ULL, UINT64_MAX, false},
    {0x5cULL, 0x14002ULL, 0x5ULL, UINT64_MAX, false},
    {0x5cULL, 0x4000ULL, 0x4ULL, UINT64_MAX, false},
    {0xffffffff00000083ULL, 0x23ULL, 0xffffffff00000004ULL,
     0xffffffff00000005ULL, true},
    {0x5cULL, 0x4000ULL, 0x4ULL, UINT64_MAX, false},
    {0xffffffff00000083ULL, 0x0ULL, 0xffffffff00000005ULL,
     0xffffffff00000008ULL, false},
    {0x5cULL, 0x0ULL, 0x3ULL, UINT64_MAX, false},
    {0x5cULL, 0x14002ULL, 0x3ULL, UINT64_MAX, false},
    {0xffffffff00000083ULL, 0x0ULL, 0xffffffff00000003ULL,
     0xffffffff00000001ULL, false},
    {0xffffffff00000083ULL, 0x0ULL, 0xffffffff00000004ULL,
     0xffffffff00000001ULL, false},
    {0x5cULL, 0x4000ULL, 0x4ULL, UINT64_MAX, false},
    {0xffffffff00000083ULL, 0x23ULL, 0xffffffff00000003ULL,
     0xffffffff00000006ULL, true},
    {0xffffffff00000083ULL, 0x0ULL, 0xffffffff00000004ULL,
     0xffffffff00000008ULL, false},
    {0xffffffff00000083ULL, 0x23ULL, 0xffffffff00000004ULL,
     0xffffffff00000006ULL, true},
    {0xffffffff00000083ULL, 0x0ULL, 0xffffffff0000000aULL,
     0xffffffff00000006ULL, false},
    {0xffffffff00000083ULL, 0x0ULL, 0xffffffff00000014ULL,
     0xffffffff00000008ULL, false},
    {0xffffffff00000083ULL, 0x0ULL, 0xffffffff00000004ULL,
     0xffffffff00000006ULL, false},
    {0xffffffff00000083ULL, 0x0ULL, 0xffffffff00000004ULL,
     0xffffffff00000008ULL, false},
    {0xffffffff00000083ULL, 0x9ULL, 0xffffffff00000003ULL,
     0xffffffff00000000ULL, true},
    {0xffffffff00000083ULL, 0x9ULL, 0xffffffffffffffffULL,
     0xffffffff00000006ULL, true},
    {0x153ULL, 0x0ULL, 0x3ULL, UINT64_MAX, false},
    {0x153ULL, 0x0ULL, 0x4ULL, UINT64_MAX, false},
    {0x153ULL, 0x0ULL, 0x5ULL, UINT64_MAX, false},
    {0xc7ULL, 0xaULL, 0x3ULL, UINT64_MAX, false},
    {0xc7ULL, 0x3ULL, 0x4ULL, UINT64_MAX, false},
    {0x5cULL, 0x14002ULL, 0x3ULL, UINT64_MAX, false},
    {0x5cULL, 0x4000ULL, 0x4ULL, UINT64_MAX, false},
    {0x9ULL, 0x0ULL, UINT64_MAX, UINT64_MAX, false},
    {0x83ULL, 0x0ULL, 0x3ULL, 0x6ULL, false},
    {0x83ULL, 0x23ULL, 0x4ULL, 0x6ULL, true},
    {0x5ULL, 0x6ULL, UINT64_MAX, UINT64_MAX, false},
    {0x83ULL, 0x23ULL, 0x6ULL, 0x6ULL, true},
    {0x80ULL, 0x0ULL, UINT64_MAX, UINT64_MAX, false},
    {0x83ULL, 0x23ULL, 0x4ULL, 0x6ULL, true},
    {0xaULL, 0x0ULL, UINT64_MAX, UINT64_MAX, false},
    {0x83ULL, 0x23ULL, 0x4ULL, 0x6ULL, true},
    {0xaULL, 0x0ULL, UINT64_MAX, UINT64_MAX, false},
    {0x83ULL, 0x23ULL, 0x4ULL, 0x6ULL, true},
    {0x153ULL, 0x0ULL, 0x4ULL, UINT64_MAX, false},
    {0x5ULL, 0x7ULL, UINT64_MAX, UINT64_MAX, false},
    {0x4ULL, 0xaULL, 0x7ULL, UINT64_MAX, false},
    {0x83ULL, 0x0ULL, 0x7ULL, 0x6ULL, false},
    {0x5cULL, 0x14002ULL, 0x7ULL, UINT64_MAX, false},
    {0x83ULL, 0x23ULL, 0x4ULL, 0x6ULL, true},
    {0x83ULL, 0x0ULL, 0x7ULL, 0x8ULL, false},
    {0x6ULL, 0x0ULL, 0x7ULL, UINT64_MAX, false},
    {0x6ULL, 0x0ULL, 0x3ULL, UINT64_MAX, false},
    {0x83ULL, 0x9ULL, 0x3ULL, 0x6ULL, true},
    {0x83ULL, 0x23ULL, 0x4ULL, 0x6ULL, true},
    {0x6ULL, 0x0ULL, 0x5ULL, UINT64_MAX, false},
    {0x83ULL, 0x23ULL, 0x4ULL, 0x6ULL, true},
    {0x6ULL, 0x0ULL, 0xaULL, UINT64_MAX, false},
    {0x83ULL, 0x23ULL, 0x4ULL, 0x6ULL, true},
    {0x6ULL, 0x0ULL, 0x14ULL, UINT64_MAX, false},
    {0x83ULL, 0x0ULL, 0x4ULL, 0x6ULL, false},
    {0x5cULL, 0x4000ULL, 0x4ULL, UINT64_MAX, false},
    {0x83ULL, 0x0ULL, 0x4ULL, 0x8ULL, false},
    {0x153ULL, 0x0ULL, 0x4ULL, UINT64_MAX, false},
    {0xc7ULL, 0x0ULL, 0x4ULL, UINT64_MAX, false},
    {0x3ULL, 0xaULL, 0x4ULL, UINT64_MAX, false},
    {0x6ULL, 0x0ULL, 0x6ULL, UINT64_MAX, false},
    {0x6ULL, 0x0ULL, 0x4ULL, UINT64_MAX, false},
    {0xaULL, 0x0ULL, UINT64_MAX, UINT64_MAX, false},
    {0x4ULL, 0x1ULL, 0x1ULL, UINT64_MAX, false},
};
inline constexpr char CreatedAdvisoryFilesJSON[] = R"({
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
  "advisory_locks": {
    "objects": [],
    "created_regular_objects": true
  }
})";
} // namespace neverd::emulation::darwin_test
#endif
