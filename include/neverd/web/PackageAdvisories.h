//===- PackageAdvisories.h - Captured OSV package version evidence --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Offline advisory declarations, original anchors and bounded version matches.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Limits.h"
#include "neverd/web/PackageEvidence.h"
#include "neverd/web/Packages.h"

namespace neverd::web {

inline constexpr std::string_view PackageAdvisoryProfile = "npm-osv-1.9.1-v1";

// All supplied strings below are private declarations. Publication uses fixed
// categories, admitted timestamps and opaque artifact/record anchors only.
struct PackageAdvisoryEvent {
  std::string Kind, Version;
};
struct PackageAdvisoryRange {
  std::string Type;
  std::vector<PackageAdvisoryEvent> Events;
};
struct PackageAdvisoryAffected {
  std::string ID, Ecosystem, Name, PURL;
  std::string IdentityStatus;
  std::vector<std::string> Versions;
  std::vector<PackageAdvisoryRange> Ranges;
  uint64_t Index = 0;
};
struct PackageAdvisorySeverity {
  std::string ID, AffectedID, Type, Score, Source;
  uint64_t Index = 0;
  // The reader validates field structure, not a CVSS vector or numeric score.
  std::string Validation = "supplied_unvalidated";
};
struct PackageAdvisory {
  std::string ID, ArtifactID, BlobHash, DeclaredID, SchemaVersion;
  std::string SchemaStatus, TemporalStatus, SeverityStatus;
  PackageEvidenceDescriptor Descriptor;
  PackageEvidenceTime Modified, Published, Withdrawn;
  std::vector<PackageAdvisoryAffected> Affected;
  std::vector<PackageAdvisorySeverity> Severities;
  uint64_t RetainedBytes = 0, Steps = 0;
};
struct PackageAdvisoryMatch {
  std::string ID, PackageID, AdvisoryID, Status;
  std::string IdentityStatus, VersionStatus;
  std::vector<std::string> MatchedAffectedIDs;
  bool SelectorCoverageComplete = false;
};
struct PackageAdvisoryMatches {
  std::string ID, PackageAnalysisID;
  std::vector<std::string> AdvisoryIDs;
  std::vector<PackageAdvisoryMatch> Records;
  uint64_t Steps = 0, MatchCount = 0, UnknownCount = 0;
};

PackageAdvisory
readPackageAdvisory(const Artifact &Input,
                    const PackageEvidenceDescriptor &Descriptor);
/// Match only the inventory's explicit declarations; installed aliases and
/// paths do not establish upstream identity. No feed completeness,
/// reachability, authentication or safe-package conclusion is inferred from a
/// negative row.
PackageAdvisoryMatches
matchPackageAdvisories(const PackageAnalysis &Package,
                       const std::vector<const PackageAdvisory *> &Advisories);

} // namespace neverd::web
