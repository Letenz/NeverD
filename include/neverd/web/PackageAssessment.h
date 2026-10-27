//===- PackageAssessment.h - Typed package review evidence --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Immutable findings and bounded analyst assertions, independent of verdicts.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Artifact.h"
#include "neverd/web/Limits.h"

#include <span>

namespace neverd::web {
struct PackageAnalysis;
struct PackageArchive;
struct PackageIntegrityResult;
struct PackageAdvisoryMatches;
struct PackageAdvisory;
struct PackageAttestation;
struct PackageAttestationDigestMatch;
struct PackageBehavior;
struct PackageInstall;
struct PackageDiff;
struct PackageBehaviorDiff;
inline constexpr std::string_view PackageAssessmentProfile =
    "offline-package-assessment-v1";
inline constexpr std::string_view PackageReviewJournalProfile =
    "offline-package-review-journal-v1";

enum class PackageFindingKind {
  Coverage,
  Inventory,
  Integrity,
  Advisory,
  Attestation,
  Behavior,
  Installation,
  Comparison
};
struct PackageFinding {
  std::string ID;
  PackageFindingKind Kind = PackageFindingKind::Coverage;
  // Status is an owner-produced fixed state, never supplied prose or a name.
  std::string Status, Binding, Confidence, OwnerID, RecordID, PackageID;
  std::string ArtifactID, BlobHash, SourceID, NodeID;
  std::string DeclarationArtifactID, DeclarationHash;
  std::string DigestAlgorithm;
  // Fixed rule-table classifications, never arbitrary command/argument text.
  std::string API, PatternKind, OriginEvidence;
  std::vector<std::string> RelatedIDs;
  uint64_t Start = 0, End = 0;
  bool HasSourceSpan = false;
  uint64_t BeforeCount = 0, AfterCount = 0;
  bool HasPatternCounts = false;
};
struct PackageAssessmentTime {
  std::string Kind, Status, Value;
};
struct PackageAssessmentFeed {
  std::string OwnerID, DescriptorID, ArtifactID, BlobHash;
  std::string Validation, TemporalStatus;
  // Only admitted UTC timestamps are copied. Private source labels are absent.
  std::vector<PackageAssessmentTime> Times;
};
struct PackageAssessment {
  std::string ID, PackageAnalysisID, NamespaceID;
  std::vector<std::string> SelectionIDs;
  std::vector<PackageFinding> Findings;
  std::vector<PackageAssessmentFeed> Feeds;
  uint64_t Steps = 0, RetainedBytes = 0;
};

struct PackageAssessmentSelection {
  // Originals and Archives come from retained session owners, not caller JSON.
  const Snapshot *Originals = nullptr;
  std::span<const PackageArchive *const> Archives;
  std::span<const PackageIntegrityResult *const> Integrity;
  const PackageAdvisoryMatches *AdvisoryMatches = nullptr;
  std::span<const PackageAdvisory *const> Advisories;
  std::span<const PackageAttestation *const> Attestations;
  std::span<const PackageAttestationDigestMatch *const> DigestMatches;
  const PackageBehavior *Behavior = nullptr;
  const PackageInstall *Installation = nullptr;
  const PackageDiff *Comparison = nullptr;
  const PackageBehaviorDiff *BehaviorComparison = nullptr;
};

/// Compose retained authoritative results without parsing target text or
/// reinterpreting digest, advisory, JavaScript or loader semantics. Registry
/// identity and archive membership are independent relationships. An archive
/// digest never verifies an installed dependency's tree or a publisher.
PackageAssessment
composePackageAssessment(const PackageAnalysis &Packages,
                         const Snapshot &Namespace,
                         const PackageAssessmentSelection &Selection);
std::string_view packageFindingKindName(PackageFindingKind Kind);

enum class PackageDisposition {
  Unreviewed,
  NeedsReview,
  AcceptedRisk,
  FalsePositive
};
PackageDisposition parsePackageDisposition(std::string_view Value);
std::string_view packageDispositionName(PackageDisposition Value);

struct PackageReviewDecision {
  std::string ID, AssessmentID, FindingID, ParentHead;
  PackageDisposition Disposition = PackageDisposition::Unreviewed;
  uint64_t Index = 0;
};
struct PackageReviewState {
  std::string AssessmentID, RootID, HeadID;
  std::vector<PackageReviewDecision> Decisions;
  uint64_t RetainedBytes = 0;
};
struct PackageReviewCapacity {
  // Remaining shared session capacity, not per-base-assessment allowances.
  uint64_t Events = MaxPackageReviewDecisions;
  uint64_t Bytes = MaxPackageReviewBytes;
};
struct PackageReviewUpdate {
  PackageReviewState State;
  uint64_t AddedEvents = 0, AddedBytes = 0;
  bool Replay = false;
};

PackageReviewState createPackageReview(const PackageAssessment &Assessment);
void validatePackageReview(const PackageAssessment &Assessment,
                           const PackageReviewState &Review);
/// Prepare a new bounded state from retained owner models. The caller holds
/// the session lock, serializes the response, then publishes this state and
/// charges AddedEvents/AddedBytes together. Failures never modify Prior.
/// Only an exact replay of the last accepted request bypasses stale-head and
/// capacity refusal. A disposition is a caller assertion, not analyzer proof.
PackageReviewUpdate appendPackageReview(const PackageAssessment &Assessment,
                                        const PackageReviewState &Prior,
                                        std::string_view ExpectedHead,
                                        std::string_view FindingID,
                                        PackageDisposition Disposition,
                                        PackageReviewCapacity Remaining);
/// Validate an entire supplied chain with this assessment's authoritative
/// identity recipe. Prior must be an exact prefix. Only a suffix is charged;
/// an exact current-head replay is free. No partial restoration is published.
/// Retained bytes are computed here, never trusted from a journal.
PackageReviewUpdate restorePackageReview(const PackageAssessment &Assessment,
                                         const PackageReviewState &Prior,
                                         std::string_view ExpectedHead,
                                         PackageReviewState Candidate,
                                         PackageReviewCapacity Remaining);
} // namespace neverd::web
