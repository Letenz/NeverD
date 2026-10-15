//===- DriverFileSystem.h - Declared guest filesystem state ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Explicit session-local file metadata and bytes, independent of host files.

#ifndef NEVERD_EMULATION_DRIVERFILESYSTEM_H
#define NEVERD_EMULATION_DRIVERFILESYSTEM_H

#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd::emulation {
#define NEVERD_DRIVER_FILESYSTEM_LIMIT(Name, Value)                            \
  inline constexpr size_t Name = Value;
#include "neverd/emulation/DriverFileSystemLimits.def"
#undef NEVERD_DRIVER_FILESYSTEM_LIMIT

enum class DriverFileNodeKind { File, Directory };
enum class DriverFileAccessPolicy { FullAccess };

struct DriverFileSystemNode {
  /// Rooted volume-relative ASCII path. Parents must be explicitly declared.
  std::string Path;
  std::optional<DriverFileNodeKind> Kind;
  /// Unique within this volume. Zero is a valid explicitly declared identity.
  std::optional<uint64_t> FileID;
  std::optional<uint32_t> Attributes;
  std::optional<uint64_t> AllocationSize;
  std::optional<uint64_t> CreationTime100ns;
  std::optional<uint64_t> LastAccessTime100ns;
  std::optional<uint64_t> LastWriteTime100ns;
  std::optional<uint64_t> ChangeTime100ns;
  /// Required for files, absent for directories. EOF is exactly Data.size().
  std::optional<std::vector<uint8_t>> Data;
};

struct DriverFileSystemVolume {
  /// One named device under \\Device. This first contract has no DOS aliases.
  std::string DeviceName;
  /// FullAccess excludes ACL denials, not attributes, sharing or open options.
  std::optional<DriverFileAccessPolicy> AccessPolicy;
  std::optional<uint32_t> SectorSize;
  std::optional<uint32_t> AlignmentMask;
  /// A closed tree with exactly one explicitly declared root directory.
  std::vector<DriverFileSystemNode> Nodes;
};

struct DriverFileSystemVolumeState {
  /// Current owner state, independent of the original configuration object.
  DriverFileSystemVolume Volume;
  uint64_t DeviceObject = 0;
};

/// Omission leaves filesystem availability unspecified; an empty vector is
/// an explicitly empty inventory. A root-only volume models an empty volume.
/// ASCII case folding governs identity. Metadata is required, including zero
/// values; file data is exact and no host file or metadata is consulted.
/// This validates declarations only and does not establish file API support.
llvm::Error validateDriverFileSystem(
    const std::optional<std::vector<DriverFileSystemVolume>> &Volumes);
} // namespace neverd::emulation

#endif
