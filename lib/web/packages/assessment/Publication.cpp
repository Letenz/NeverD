//===- Publication.cpp - Fixed assessment and analyst metadata -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Publish opaque anchors and caller assertions without target prose or scores.
///
//===----------------------------------------------------------------------===//

#include "Publication.h"

namespace neverd::web::package_assessment {
namespace {
llvm::json::Value anchor(const std::string &ID) {
  return ID.empty() ? llvm::json::Value(nullptr) : llvm::json::Value(ID);
}
llvm::json::Array feeds(const PackageAssessment &A) {
  llvm::json::Array Feeds;
  for (const auto &F : A.Feeds) {
    llvm::json::Array Times;
    for (const auto &T : F.Times)
      Times.emplace_back(llvm::json::Object{{"kind", T.Kind},
                                            {"validation", T.Status},
                                            {"value", anchor(T.Value)}});
    Feeds.emplace_back(
        llvm::json::Object{{"owner_id", F.OwnerID},
                           {"descriptor_id", F.DescriptorID},
                           {"artifact_id", F.ArtifactID},
                           {"blob_sha256", F.BlobHash},
                           {"validation", F.Validation},
                           {"temporal_status", anchor(F.TemporalStatus)},
                           {"declared_times", std::move(Times)},
                           {"freshness_assessed", false}});
  }
  return Feeds;
}
} // namespace

Dispositions dispositions(const PackageAssessment &A,
                          const PackageReviewState &Review) {
  validatePackageReview(A, Review);
  Dispositions R;
  for (const auto &D : Review.Decisions)
    R[D.FindingID] = D.Disposition;
  return R;
}
llvm::json::Object summary(const PackageAssessment &A,
                           const PackageReviewState &Review,
                           uint64_t Revision) {
  validatePackageReview(A, Review);
  llvm::json::Array Selections;
  for (const auto &ID : A.SelectionIDs)
    Selections.emplace_back(ID);
  return llvm::json::Object{{"schema_version", 1},
                            {"status", "ok"},
                            {"revision", std::to_string(Revision)},
                            {"profile", std::string(PackageAssessmentProfile)},
                            {"package_assessment_id", A.ID},
                            {"package_analysis_id", A.PackageAnalysisID},
                            {"namespace_id", A.NamespaceID},
                            {"review_root_id", Review.RootID},
                            {"review_head_id", Review.HeadID},
                            {"decision_count", Review.Decisions.size()},
                            {"finding_count", A.Findings.size()},
                            {"selection_ids", std::move(Selections)},
                            {"feeds", feeds(A)},
                            {"steps", A.Steps},
                            {"retained_metadata_bytes", A.RetainedBytes},
                            {"analysis_status", "partial"},
                            {"selection_complete", false},
                            {"publisher_authenticated", false},
                            {"signature_verification", "not_performed"},
                            {"installed_tree_verified", false},
                            {"runtime_api_verified", false},
                            {"reachability_verified", false},
                            {"freshness_assessed", false},
                            {"dispositions_are", "caller_assertions"},
                            {"safety_verdict", "not_assessed"},
                            {"executes_input", false},
                            {"fetches_remote_data", false},
                            {"redaction_policy", "metadata-only-v1"}};
}
llvm::json::Object record(const PackageFinding &F,
                          std::optional<PackageDisposition> D) {
  llvm::json::Array Related;
  for (const auto &ID : F.RelatedIDs)
    Related.emplace_back(ID);
  return llvm::json::Object{
      {"finding_id", F.ID},
      {"kind", std::string(packageFindingKindName(F.Kind))},
      {"evidence_status", F.Status},
      {"binding", F.Binding},
      {"confidence", F.Confidence},
      {"digest_algorithm", anchor(F.DigestAlgorithm)},
      {"legacy_sha1", F.DigestAlgorithm.empty()
                          ? llvm::json::Value(nullptr)
                          : llvm::json::Value(F.DigestAlgorithm == "sha1")},
      {"api_class", anchor(F.API)},
      {"pattern_kind", anchor(F.PatternKind)},
      {"origin_evidence", anchor(F.OriginEvidence)},
      {"recognized_count_before", F.HasPatternCounts
                                      ? llvm::json::Value(F.BeforeCount)
                                      : llvm::json::Value(nullptr)},
      {"recognized_count_after", F.HasPatternCounts
                                     ? llvm::json::Value(F.AfterCount)
                                     : llvm::json::Value(nullptr)},
      {"owner_id", anchor(F.OwnerID)},
      {"record_id", anchor(F.RecordID)},
      {"package_id", anchor(F.PackageID)},
      {"artifact_id", anchor(F.ArtifactID)},
      {"blob_sha256", anchor(F.BlobHash)},
      {"source_id", anchor(F.SourceID)},
      {"node_id", anchor(F.NodeID)},
      {"related_ids", std::move(Related)},
      {"declaration_artifact_id", anchor(F.DeclarationArtifactID)},
      {"declaration_sha256", anchor(F.DeclarationHash)},
      {"byte_offset", F.HasSourceSpan
                          ? llvm::json::Value(std::to_string(F.Start))
                          : llvm::json::Value(nullptr)},
      {"byte_length", F.HasSourceSpan
                          ? llvm::json::Value(std::to_string(F.End - F.Start))
                          : llvm::json::Value(nullptr)},
      {"disposition", std::string(packageDispositionName(
                          D.value_or(PackageDisposition::Unreviewed)))},
      {"disposition_source", D ? "caller_assertion" : "not_reviewed"}};
}
llvm::json::Object record(const PackageReviewDecision &D) {
  return llvm::json::Object{
      {"decision_id", D.ID},
      {"package_assessment_id", D.AssessmentID},
      {"finding_id", D.FindingID},
      {"parent_head_id", D.ParentHead},
      {"disposition", std::string(packageDispositionName(D.Disposition))},
      {"decision_index", D.Index},
      {"assertion_source", "caller"}};
}
} // namespace neverd::web::package_assessment
