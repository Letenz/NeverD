//===- NodeBehavior.h - Offline Node source behavior evidence -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Fixed API syntax classes and exact package delivery relationships.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Limits.h"
#include "neverd/web/Packages.h"
#include "neverd/web/Source.h"

#include <span>

namespace neverd::web {

inline constexpr std::string_view NodeBehaviorProfile =
    "node-22.15-static-behavior-v1";

struct NodeBehaviorArgument {
  std::string NodeID;
  uint32_t Ordinal = 0;
  bool Spread = false;
};

/// Public fixed vocabulary and opaque anchors only. API names come from the
/// versioned rule table, never copied arbitrary target member strings.
struct NodeBehaviorRecord {
  std::string ID, Kind, API, NodeID, CalleeID, ReceiverID;
  std::string RequestID, RequestNodeID, ImportID, OriginEvidence;
  std::string Encoding = "not_applicable";
  uint64_t Start = 0, End = 0;
  uint32_t ArgumentCount = 0;
  bool Spread = false, Optional = false;
  std::vector<NodeBehaviorArgument> Arguments;
};

struct NodeBehavior {
  std::string ID, SourceID, ArtifactID, SourceHash, SourceType;
  std::string BindingID, ModuleID, OriginID, ValueID;
  std::string Status = "unavailable", Reason;
  std::string OriginStatus, OriginReason, ValueStatus;
  std::vector<NodeBehaviorRecord> Records;
  uint64_t Steps = 0, Calls = 0, UnattributedCalls = 0, UnmatchedCalls = 0;
  uint64_t ConstructionCalls = 0;
};

/// Uses the shared binding/module/origin/value owners. Call syntax does not
/// establish runtime API identity, reachability, successful I/O or malice.
NodeBehavior analyzeNodeBehavior(const SourceAnalysis &Source);

/// An ancestry edge supplied only from an already retained extractor result.
/// It is a delivery relation, not ownership of the embedded source module.
struct PackageBehaviorOrigin {
  std::string ArtifactID, ParentArtifactID, EvidenceID, Kind;
};
struct PackageBehaviorInput {
  const SourceAnalysis *Source = nullptr;
  const NodeBehavior *Behavior = nullptr;
  uint64_t SourceBytes = 0;
};
struct PackageBehaviorSource {
  std::string SourceID, ArtifactID, BehaviorID, Status, Reason;
  // Retained original-owner coverage, internal to comparison consumers.
  // Older publication and identity remain unchanged.
  std::string SourceType, SourceHash, OriginStatus, ValueStatus;
  uint64_t SourceBytes = 0;
  std::string FileID, DeliveryArtifactID, PackageID, Relation;
  std::vector<std::string> OriginIDs, EntryIDs;
  uint64_t EntryCount = 0;
  std::vector<NodeBehaviorRecord> Records;
};
struct PackageBehavior {
  std::string ID, PackageAnalysisID;
  std::vector<PackageBehaviorSource> Sources;
  uint64_t Steps = 0, UnavailableSources = 0, RecordCount = 0;
};

/// Join explicitly selected sources to exact inventoried files, optionally
/// through retained Bun/SEA ancestry. Unknown ownership stays unknown.
PackageBehavior
analyzePackageBehavior(const PackageAnalysis &Packages,
                       std::span<const PackageBehaviorInput> Input,
                       std::span<const PackageBehaviorOrigin> Origins);

} // namespace neverd::web
