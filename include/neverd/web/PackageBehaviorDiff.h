//===- PackageBehaviorDiff.h - Selected package behavior comparison -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Compare recognized syntax counts while retaining source coverage evidence.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Limits.h"
#include "neverd/web/NodeBehavior.h"

namespace neverd::web {

inline constexpr std::string_view PackageBehaviorDiffProfile =
    "node-selected-behavior-diff-v1";

struct PackageBehaviorDiffEndpoint {
  std::string SourceID, ArtifactID, FileID, PackageID, BehaviorID, SourceType;
  std::string Status, BehaviorStatus, OriginStatus, ValueStatus;
};

struct PackageBehaviorSourcePair {
  std::string ID, Status, Bytes;
  PackageBehaviorDiffEndpoint Before, After;
};

struct PackageBehaviorPattern {
  std::string ID, PairID, API, Kind, OriginEvidence, Encoding;
  uint64_t Before = 0, After = 0;
  uint32_t ArgumentCount = 0;
  bool Optional = false, Spread = false;
};

struct PackageBehaviorOccurrence {
  std::string ID, PairID, GroupID, Side, PackageBehaviorID;
  std::string SourceID, ArtifactID, FileID, BehaviorID, RecordID, NodeID;
  uint64_t Start = 0, End = 0;
};

struct PackageBehaviorDiff {
  std::string ID, PackageDiffID, BeforeID, AfterID;
  std::string BeforeBehaviorID, AfterBehaviorID;
  std::string RootIdentity, Coverage, Platform, InputContract;
  std::vector<PackageBehaviorSourcePair> Pairs;
  std::vector<PackageBehaviorPattern> Patterns;
  std::vector<PackageBehaviorOccurrence> Occurrences;
  uint64_t Steps = 0, ComparedPairs = 0;
};

/// Pair only exact inventory-relative paths with explicit optional selections.
/// Missing or degraded evidence never becomes a zero-count comparison. No
/// execution, remote acquisition, call correspondence or release alignment.
PackageBehaviorDiff
comparePackageBehavior(const PackageDiff &Base, const PackageAnalysis &Before,
                       const PackageAnalysis &After,
                       const PackageBehavior *BeforeBehavior,
                       const PackageBehavior *AfterBehavior);

} // namespace neverd::web
