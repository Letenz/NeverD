//===- PackageInstall.h - Offline package installation evidence ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded lifecycle declaration to selected source behavior associations.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Limits.h"
#include "neverd/web/NodeBehavior.h"

#include <optional>

namespace neverd::web {

inline constexpr std::string_view PackageInstallProfile =
    "node-install-entry-traces-v1";

struct PackageInstallInput {
  const SourceAnalysis *Source = nullptr;
  uint64_t SourceBytes = 0;
};

struct PackageInstallScript {
  std::string ID, ScriptID, PackageID, ManifestID, Kind, Status;
  std::string CommandStatus, FileID, ArtifactID, SourceID;
  // Offsets within the decoded command, never manifest byte positions.
  std::optional<uint64_t> CommandStart, CommandEnd;
};

struct PackageInstallSource {
  std::string SourceID, ArtifactID, FileID, PackageID, BehaviorID, Status;
  std::string ModuleID, OriginID, LinkID;
};

struct PackageInstallRequest {
  std::string ID, SourceID, RequestID, NodeID, Kind, Status, OriginEvidence;
  std::string ArtifactID, FileID, PackageID, TargetSourceID;
};

struct PackageInstallAssociation {
  std::string ID, ScriptTraceID, ScriptID, EntryFileID;
  std::string SourceID, BehaviorID, RecordID, FileID, PackageID;
  // Original behavior metadata, usable without an independent behavior cache.
  std::string NodeID, API, Kind, OriginEvidence;
  uint64_t Start = 0, End = 0;
  std::vector<std::string> RequestIDs;
};

struct PackageInstall {
  std::string ID, PackageAnalysisID, NamespaceID, PackageBehaviorID;
  std::vector<PackageInstallScript> Scripts;
  std::vector<PackageInstallSource> Sources;
  std::vector<PackageInstallRequest> Requests;
  std::vector<PackageInstallAssociation> Associations;
  uint64_t Steps = 0, LifecycleDeclarations = 0, PresenceDeclarations = 0;
  uint64_t DirectSources = 0, EmbeddedSources = 0, UnavailableSources = 0;
  uint64_t UnresolvedRequests = 0;
};

/// Associate syntactic evidence only. No process, shell, loader, network or
/// filesystem lookup. Missing selection and unavailable files stay unknown.
/// Package behavior is optional; when supplied all its sources are retained.
/// The snapshot must be the namespace used by the package inventory owner.
PackageInstall
analyzePackageInstall(const PackageAnalysis &Packages,
                      const Snapshot &Namespace,
                      const PackageBehavior *Behavior,
                      std::span<const PackageInstallInput> Input);

} // namespace neverd::web
