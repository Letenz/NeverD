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
#include <set>
#include <string>
#include <vector>

namespace neverd::emulation {
namespace darwin_file_limits {
inline constexpr uint32_t DefaultDescriptors = 256;
inline constexpr uint32_t Descriptors = 4096;
inline constexpr uint32_t Files = 256;
inline constexpr uint32_t DirectoryEntries = 4096;
inline constexpr uint64_t Bytes = 16 * 1024 * 1024;
inline constexpr uint32_t Path = 1024;
inline constexpr uint32_t Name = 255;
} // namespace darwin_file_limits

struct DarwinFileTime {
  int64_t Seconds = 0;
  int64_t Nanoseconds = 0;
};

/// Fixed stat64 observations. Regular-file Size must match its bytes; directory
/// Size is an explicit nonnegative observation, not an entry count.
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

/// One observed directory record. NextOffset is the enumeration cursor after
/// this entry, distinct from the optional d_seekoff observation. Cookies are
/// local to a snapshot and need not increase; zero is reserved for rewind.
struct DarwinDirectoryEntry {
  std::string Name;
  uint64_t Inode = 0;
  uint8_t Type = 0;
  uint64_t NextOffset = 0;
  uint64_t SeekOffset = 0;
  /// Additional payload minimum when starting at this record, if any.
  uint32_t MinimumBufferSize = 0;
};

/// Complete ordered snapshot, including . and .. and every catalogue child.
/// No host inode, filesystem order or cursor is inferred from path names.
struct DarwinDirectoryContents {
  std::vector<DarwinDirectoryEntry> Entries;
  /// Explicit positive payload minimum, including at EOF. Extended syscall
  /// flags are excluded from this count; each next record must also fit whole.
  uint32_t MinimumBufferSize = 0;
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
  /// Optional metadata for existing files or directories, including ancestors.
  std::map<std::string, DarwinFileMetadata> Metadata;
  /// Explicit directories, including empty ones; root and ancestors are
  /// implicit.
  std::set<std::string> Directories;
  /// Absent means unknown, not the host CWD. Must name an existing directory.
  std::optional<std::string> WorkingDirectory;
  /// Absent snapshots remain unknown, even for an explicit empty directory.
  std::map<std::string, DarwinDirectoryContents> DirectoryContents;
};
} // namespace neverd::emulation
#endif
