//===- LinuxFileTestMetadata.h - Independent status observations -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNITTESTS_EMULATION_LINUXFILETESTMETADATA_H
#define NEVERD_UNITTESTS_EMULATION_LINUXFILETESTMETADATA_H

#include "neverd/emulation/LinuxFileOptions.h"

namespace neverd::emulation {
inline constexpr char FileTestMetadataJSON[] = R"({
  "device":4262645044,"inode":"18364758544493064720","mode":33188,
  "link_count":2309737967,"uid":2271560481,"gid":4275878552,
  "size":0,"block_size":16384,"blocks":78187493520,
  "access_time":{"seconds":"-9223372036854775807","nanoseconds":123456789},
  "modification_time":{"seconds":4294967297,"nanoseconds":987654321},
  "change_time":{"seconds":"9223372036854775807","nanoseconds":999999999}})";
inline LinuxFileMetadata fileTestMetadata() {
  return {0xfe12cd34,
          0xfedcba9876543210ULL,
          0100644,
          0x89abcdef,
          0x87654321,
          0xfedcba98,
          0,
          16384,
          0x1234567890ULL,
          {-0x7fffffffffffffffLL, 123456789},
          {4294967297, 987654321},
          {0x7fffffffffffffffLL, 999999999}};
}
} // namespace neverd::emulation
#endif
