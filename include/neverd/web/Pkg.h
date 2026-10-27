//===- Pkg.h - Offline pkg virtual filesystem evidence --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit pkg layout compatibility and immutable typed store occurrences.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Artifact.h"
#include "neverd/web/Limits.h"

namespace neverd::web {
inline constexpr std::string_view PkgProfile = "pkg-5.8.1-uncompressed-vfs-v1";
inline constexpr uint32_t MaxPkgStores = 4 * MaxPkgPaths;

enum class PkgStore : uint8_t { Cache, Content, DirectoryList, Stat };

struct PkgRegion {
  std::string ID, Kind, BlobHash;
  uint64_t Offset = 0;
  Blob Content;
  bool MetadataRange = false;
  bool selectable() const {
    return Kind == "prelude_storage" && !MetadataRange;
  }
};

struct PkgMember {
  std::string ID, RangeID, BlobHash;
  /// Private virtual key, never a host output path or public identity input.
  std::string Path;
  PkgStore Store = PkgStore::Content;
  bool Directory = false, CachePresent = false, MetadataRange = false;
  uint64_t Offset = 0, DeclaredStatSize = 0;
  Blob Content;
  bool selectable() const {
    return Store == PkgStore::Content && !Directory && !MetadataRange;
  }
  bool privateHash() const {
    return MetadataRange || Store == PkgStore::DirectoryList ||
           Store == PkgStore::Stat;
  }
};

struct PkgLink {
  std::string ID;
  /// Private declarations only; no resolution or traversal is performed.
  std::string Path, Target;
};

struct PkgExtraction {
  std::string ID, ArtifactID, BlobHash;
  std::string PathStyle, LocatorLineStyle;
  /// A literal match grants no runtime membership or source/cache equivalence.
  std::string DeclaredEntry, LiteralEntryMemberID;
  bool EntrySymlinkPrefix = false, EntryCachePresent = false;
  uint64_t LocatorOffset = 0, LocatorSize = 0;
  uint64_t PayloadOffset = 0, PayloadSize = 0, ReferencedBytes = 0;
  uint64_t PrivateBytes = 0, Steps = 0, ScannedBytes = 0, HashedBytes = 0;
  uint64_t JSONNodes = 0;
  uint32_t PathCount = 0, ContentFileCount = 0, CacheOnlyFileCount = 0;
  uint32_t UnavailableFileCount = 0, DirectoryCount = 0, UniqueStoreRanges = 0;
  std::vector<PkgRegion> Regions;
  std::vector<PkgMember> Members;
  std::vector<PkgLink> Links;
};

bool pkgAvailable();
/// Reads a fixed declarative layout only; never executes loader/prelude/source,
/// authenticates a native prefix, follows virtual links or interprets V8 data.
PkgExtraction extractPkg(const Artifact &Input, std::string_view Profile);
Snapshot pkgNamespace(const PkgExtraction &E);
const char *pkgStoreKind(PkgStore Store);
} // namespace neverd::web
