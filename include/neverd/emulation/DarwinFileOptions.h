//===- DarwinFileOptions.h - Explicit Darwin file inputs --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_DARWINFILEOPTIONS_H
#define NEVERD_EMULATION_DARWINFILEOPTIONS_H

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace neverd::emulation {
namespace darwin_file_limits {
inline constexpr uint32_t DefaultDescriptors = 256;
inline constexpr uint32_t Descriptors = 4096;
inline constexpr uint32_t Files = 256;
inline constexpr uint64_t Bytes = 16 * 1024 * 1024;
inline constexpr uint32_t Path = 1024;
inline constexpr uint32_t Name = 255;
} // namespace darwin_file_limits

struct DarwinFileTime {
  int64_t Seconds = 0;
  int64_t Nanoseconds = 0;
};

/// Fixed stat64 observations for a regular file. Size must match its bytes.
/// These observations do not grant or revoke catalogue access, and reads do
/// not advance timestamps. Unknown metadata is not synthesized from the host.
struct DarwinFileMetadata {
  int32_t Device = 0;
  uint64_t Inode = 0;
  uint16_t Mode = 0;
  uint16_t LinkCount = 0;
  uint32_t UID = 0;
  uint32_t GID = 0;
  uint64_t Size = 0;
  uint32_t BlockSize = 0;
  uint64_t Blocks = 0;
  uint32_t Flags = 0;
  uint32_t Generation = 0;
  DarwinFileTime AccessTime;
  DarwinFileTime ModificationTime;
  DarwinFileTime ChangeTime;
  DarwinFileTime BirthTime;
};

/// Closed immutable catalogue, with canonical absolute guest paths. No host
/// filesystem is consulted. Separate opens have independent offsets; dup
/// shares an open description. Ancestor directories are implicit.
struct DarwinFileOptions {
  std::map<std::string, std::vector<uint8_t>> Files;
  /// Absent means unknown input; an explicitly empty stream is EOF.
  std::optional<std::vector<uint8_t>> StandardInput;
  /// Exclusive FD ceiling, including the initially open descriptors 0/1/2.
  uint32_t DescriptorLimit = darwin_file_limits::DefaultDescriptors;
  /// Optional metadata, keyed only by existing Files paths.
  std::map<std::string, DarwinFileMetadata> Metadata;
};
} // namespace neverd::emulation
#endif
