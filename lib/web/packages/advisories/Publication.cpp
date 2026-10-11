//===- Publication.cpp - Source-attributed advisory metadata -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Publish original evidence and coverage without inventing trust or safety.
///
//===----------------------------------------------------------------------===//

#include "Publication.h"

namespace neverd::web::package_advisories {
namespace {
llvm::json::Object time(const PackageEvidenceTime &T) {
  return llvm::json::Object{{"status", T.Status},
                            {"value", T.Status == "valid_utc_timestamp"
                                          ? llvm::json::Value(T.Value)
                                          : llvm::json::Value(nullptr)}};
}
llvm::json::Array anchors(const std::vector<std::string> &IDs) {
  llvm::json::Array Values;
  for (const auto &ID : IDs)
    Values.emplace_back(ID);
  return Values;
}
llvm::json::Value anchor(std::string_view ID) {
  return ID.empty() ? llvm::json::Value(nullptr)
                    : llvm::json::Value(std::string(ID));
}
llvm::json::Object base(uint64_t Revision) {
  return llvm::json::Object{{"schema_version", 1},
                            {"status", "ok"},
                            {"revision", std::to_string(Revision)},
                            {"profile", std::string(PackageAdvisoryProfile)},
                            {"analysis_status", "partial"},
                            {"feed_completeness_verified", false},
                            {"freshness_assessed", false},
                            {"publisher_authenticated", false},
                            {"reachability_verified", false},
                            {"exploitability_verified", false},
                            {"executes_input", false},
                            {"fetches_remote_data", false},
                            {"safety_verdict", "not_assessed"},
                            {"redaction_policy", "metadata-only-v1"}};
}
} // namespace

llvm::json::Object summary(const PackageAdvisory &A, uint64_t Revision) {
  auto R = base(Revision);
  R["package_advisory_id"] = A.ID;
  R["artifact_id"] = A.ArtifactID;
  R["blob_sha256"] = A.BlobHash;
  R["declared_advisory_id_redacted"] = true;
  R["schema_validation"] = A.SchemaStatus;
  R["temporal_status"] = A.TemporalStatus;
  R["severity_status"] = A.SeverityStatus;
  R["modified"] = time(A.Modified);
  R["published"] = time(A.Published);
  R["withdrawn"] = time(A.Withdrawn);
  R["descriptor"] =
      llvm::json::Object{{"descriptor_id", A.Descriptor.ID},
                         {"artifact_id", A.Descriptor.ArtifactID},
                         {"blob_sha256", A.Descriptor.BlobHash},
                         {"profile", std::string(PackageEvidenceProfile)},
                         {"validation", A.Descriptor.Status},
                         {"source_present", !A.Descriptor.Source.empty()},
                         {"source_label_redacted", true},
                         {"retrieved_at", time(A.Descriptor.RetrievedAt)},
                         {"as_of", time(A.Descriptor.AsOf)}};
  R["affected_entry_count"] = A.Affected.size();
  R["severity_count"] = A.Severities.size();
  R["retained_metadata_bytes"] = A.RetainedBytes;
  R["steps"] = A.Steps;
  return R;
}
llvm::json::Object summary(const PackageAdvisoryMatches &A, uint64_t Revision) {
  auto R = base(Revision);
  R["package_advisory_matches_id"] = A.ID;
  R["package_analysis_id"] = A.PackageAnalysisID;
  R["package_advisory_ids"] = anchors(A.AdvisoryIDs);
  R["selection_status"] =
      A.AdvisoryIDs.empty() ? "not_supplied" : "explicit_advisory_selection";
  R["record_count"] = A.Records.size();
  R["declared_version_match_count"] = A.MatchCount;
  R["unknown_coverage_count"] = A.UnknownCount;
  R["version_matching"] = "exact_enumeration_or_strict_semver_range";
  R["negative_result_scope"] = "selected_admitted_declarations_only";
  R["steps"] = A.Steps;
  return R;
}
llvm::json::Object record(const PackageAdvisoryAffected &A) {
  uint64_t SemVer = 0, Other = 0, Events = 0;
  for (const auto &R : A.Ranges) {
    ++(R.Type == "SEMVER" ? SemVer : Other);
    Events += R.Events.size();
  }
  return llvm::json::Object{
      {"affected_entry_id", A.ID},
      {"array_index", A.Index},
      {"ecosystem_class", A.Ecosystem == "npm"  ? "npm"
                          : A.Ecosystem.empty() ? "not_supplied"
                                                : "other_ecosystem"},
      {"package_identity_status", A.IdentityStatus},
      {"package_name_present", !A.Name.empty()},
      {"ecosystem_wide_name", A.Name == "*"},
      {"purl_present", !A.PURL.empty()},
      {"enumerated_version_count", A.Versions.size()},
      {"semver_range_count", SemVer},
      {"other_range_count", Other},
      {"event_count", Events},
      {"selectors_supplied", !A.Versions.empty() || !A.Ranges.empty()}};
}
llvm::json::Object record(const PackageAdvisorySeverity &S) {
  const auto Type = S.Type == "CVSS_V2" || S.Type == "CVSS_V3" ||
                            S.Type == "CVSS_V4" || S.Type == "Ubuntu"
                        ? S.Type
                        : "other_supplied_scheme";
  return llvm::json::Object{
      {"severity_id", S.ID},
      {"affected_entry_id", anchor(S.AffectedID)},
      {"array_index", S.Index},
      {"declared_scheme", Type},
      {"validation", S.Validation},
      {"severity_source", S.Source.empty() ? "home_database_declaration"
                                           : "explicit_source_declaration"},
      {"source_verified", false},
      {"source_label_redacted", true},
      {"score_present", !S.Score.empty()},
      {"score_redacted", true},
      {"score_evaluated", false}};
}
llvm::json::Object record(const PackageAdvisoryMatch &A) {
  return llvm::json::Object{
      {"match_record_id", A.ID},
      {"package_id", A.PackageID},
      {"package_advisory_id", A.AdvisoryID},
      {"match_status", A.Status},
      {"package_identity_status", A.IdentityStatus},
      {"package_version_status", A.VersionStatus},
      {"selector_coverage_complete", A.SelectorCoverageComplete},
      {"matched_affected_entry_ids", anchors(A.MatchedAffectedIDs)},
      {"evidence_kind", "supplied_package_and_advisory_declarations"},
      {"installed_content_verified", false},
      {"reachability_verified", false},
      {"exploitability_verified", false},
      {"safety_verdict", "not_assessed"}};
}
} // namespace neverd::web::package_advisories
