//===- DarwinFileTestData.h - Explicit file observations --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TESTS_DARWINFILETESTDATA_H
#define NEVERD_TESTS_DARWINFILETESTDATA_H

#include "neverd/emulation/DarwinFileOptions.h"

namespace neverd::emulation::darwin_test {
inline DarwinFileMetadata metadata(uint64_t Size = 10) {
  return {-123,
          0xfedcba9876543210ULL,
          0100644,
          3,
          0x89abcdef,
          0xfedcba98,
          Size,
          4096,
          8,
          0x1234,
          0x89abcdef,
          {INT64_MIN + 1, 1},
          {INT64_MAX, 999999999},
          {-3, 4},
          {-5, 6}};
}
inline DarwinDirectoryContents directoryContents() {
  return {{{".", 41, 4, 11, 0, 64},
           {"..", 41, 4, 22, 0},
           {"empty", 42, 4, 7, 0},
           {"data", 0xfedcba9876543210ULL, 8, 99, 0}},
          1};
}
inline constexpr char DirectoryContentsJSON[] = R"({
  "minimum_buffer_size":1,"entries":[
    {"name":".","inode":41,"type":4,"next_offset":11,"seek_offset":0,"minimum_buffer_size":64},
    {"name":"..","inode":41,"type":4,"next_offset":22,"seek_offset":0},
    {"name":"empty","inode":42,"type":4,"next_offset":7,"seek_offset":0},
    {"name":"data","inode":"18364758544493064720","type":8,"next_offset":99,"seek_offset":0}]})";
inline constexpr char MetadataJSON[] = R"({
  "device":-123,"inode":"18364758544493064720","mode":33188,
  "link_count":3,"uid":2309737967,"gid":4275878552,"size":10,
  "block_size":4096,"blocks":8,"flags":4660,"generation":2309737967,
  "access_time":{"seconds":"-9223372036854775807","nanoseconds":1},
  "modification_time":{"seconds":"9223372036854775807","nanoseconds":999999999},
  "change_time":{"seconds":-3,"nanoseconds":4},
  "birth_time":{"seconds":-5,"nanoseconds":6}})";
} // namespace neverd::emulation::darwin_test
#endif
