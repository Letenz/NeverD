//===- Wails.h - Qualified offline Wails assets -----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Stored Go embed.FS evidence without launching the application or claiming
/// which filesystem the Wails asset server uses.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Artifact.h"
#include "neverd/web/Limits.h"

namespace neverd::web {
inline constexpr std::string_view WailsProfile =
    "wails-2.11.0-go-1.25.7-linux-x64-elf-embedfs-v1";
inline constexpr std::string_view WailsPEProfile =
    "wails-2.11.0-go-1.25.7-windows-x64-pe-embedfs-v1";

struct WailsLimits {
  uint32_t Tables = 32, Members = 4096;
  uint64_t Steps = 64000000;
  uint64_t PayloadBytes = 64ULL * 1024 * 1024;
  uint64_t PrivateBytes = 2ULL * 1024 * 1024;
};

struct WailsMember {
  std::string ID, TableID, Path, Kind, BlobHash;
  uint64_t Offset = 0, NameOffset = 0;
  uint32_t Table = 0;
  Blob Content;
  bool Directory = false;
};

struct WailsTable {
  std::string ID;
  uint64_t Offset = 0, Size = 0;
  uint32_t FirstMember = 0, MemberCount = 0;
};

struct WailsExtraction {
  std::string ID, ArtifactID, BlobHash;
  std::string Profile, ContainerFormat, Platform, Architecture;
  uint64_t BuildInfoOffset = 0, BuildInfoSize = 0;
  uint64_t Steps = 0, ScannedBytes = 0, HashedBytes = 0;
  uint64_t PayloadBytes = 0, PrivateBytes = 0;
  std::vector<WailsTable> Tables;
  std::vector<WailsMember> Members;
};

bool wailsAvailable();
std::vector<std::string_view> wailsProfiles();
/// Version declarations select a qualified stored layout; they authenticate
/// neither compiler nor publisher. Every candidate table must validate.
/// Custom limits may lower the published bounds, never raise them. Failure
/// publishes nothing. The caller retains the complete original container.
WailsExtraction extractWails(const Artifact &Input, std::string_view Profile,
                             WailsLimits Limits = {});
/// Separate namespace per table: unrelated filesystems cannot satisfy an
/// HTML/module reference simply because their embedded names happen to match.
Snapshot wailsNamespace(const WailsExtraction &Extraction, uint32_t Table);
} // namespace neverd::web
