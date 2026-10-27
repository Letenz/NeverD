//===- Asar.h - Bounded ASAR archive extraction ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded ASAR archive extraction.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Artifact.h"
#include "neverd/web/Limits.h"

namespace neverd::web {
inline constexpr std::string_view AsarProfile = "asar-pickle-json-v1";

/// Paths/link targets are private evidence, never ordinary output fields.
struct AsarMember {
  std::string ID, ParentID, Path, Kind, Link;
  std::string Status, IntegrityStatus = "missing", BlobHash;
  std::string StorageArtifactID;
  uint64_t Offset = 0, Size = 0;
  bool Unpacked = false, Executable = false;
  Blob Content;
  bool available() const { return Status == "available"; }
};

struct AsarExtraction {
  std::string ID, ArtifactID, UnpackedDirectoryID;
  uint64_t DataOffset = 0;
  uint64_t UnreferencedPayloadBytes = 0;
  std::vector<AsarMember> Members;
};

bool asarAvailable();
/// Select both inputs by occurrence in the same immutable snapshot. Empty
/// UnpackedDirectoryID performs packed-only inspection; it never guesses a
/// companion path. Structural failure publishes no extraction. Per-member
/// missing/integrity/link limitations remain explicit and cannot supply bytes.
AsarExtraction extractAsar(const Snapshot &Input, std::string_view ArtifactID,
                           std::string_view UnpackedDirectoryID = {});
} // namespace neverd::web
