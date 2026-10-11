//===- DiffPublication.cpp - Coverage and pattern count metadata --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Preserve opaque evidence anchors without publishing paths or argument data.
///
//===----------------------------------------------------------------------===//

#include "DiffPublication.h"

namespace neverd::web::package_behavior_diff {
namespace {
llvm::json::Value anchor(const std::string &ID) {
  return ID.empty() ? llvm::json::Value(nullptr) : llvm::json::Value(ID);
}
llvm::json::Object endpoint(const PackageBehaviorDiffEndpoint &R) {
  return llvm::json::Object{{"source_id", anchor(R.SourceID)},
                            {"artifact_id", anchor(R.ArtifactID)},
                            {"file_evidence_id", anchor(R.FileID)},
                            {"package_id", anchor(R.PackageID)},
                            {"node_behavior_id", anchor(R.BehaviorID)},
                            {"source_type", anchor(R.SourceType)},
                            {"status", R.Status},
                            {"behavior_status", anchor(R.BehaviorStatus)},
                            {"origin_status", anchor(R.OriginStatus)},
                            {"value_status", anchor(R.ValueStatus)}};
}
} // namespace

llvm::json::Object summary(const PackageBehaviorDiff &D, uint64_t Revision) {
  return llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(Revision)},
      {"package_behavior_diff_id", D.ID},
      {"package_diff_id", D.PackageDiffID},
      {"before_analysis_id", D.BeforeID},
      {"after_analysis_id", D.AfterID},
      {"before_package_behavior_id", anchor(D.BeforeBehaviorID)},
      {"after_package_behavior_id", anchor(D.AfterBehaviorID)},
      {"profile", std::string(PackageBehaviorDiffProfile)},
      {"behavior_profile", std::string(NodeBehaviorProfile)},
      {"analysis_status", "partial"},
      {"root_identity", D.RootIdentity},
      {"inventory_coverage", D.Coverage},
      {"platform_comparison", D.Platform},
      {"input_contract_comparison", D.InputContract},
      {"before_selection",
       D.BeforeBehaviorID.empty() ? "not_supplied" : "explicit_sources"},
      {"after_selection",
       D.AfterBehaviorID.empty() ? "not_supplied" : "explicit_sources"},
      {"source_pair_count", D.Pairs.size()},
      {"compared_pair_count", D.ComparedPairs},
      {"pattern_group_count", D.Patterns.size()},
      {"occurrence_count", D.Occurrences.size()},
      {"occurrence_scope", "comparable_selected_source_pairs"},
      {"steps", D.Steps},
      {"pairing", "exact_inventory_relative_path_candidate"},
      {"selection_complete", false},
      {"runtime_api_verified", false},
      {"reachability_verified", false},
      {"release_alignment_verified", false},
      {"call_correspondence_verified", false},
      {"argument_values_compared", false},
      {"safety_verdict", "not_assessed"},
      {"executes_input", false},
      {"redaction_policy", "metadata-only-v1"}};
}

llvm::json::Object record(const PackageBehaviorSourcePair &R) {
  return llvm::json::Object{{"source_pair_id", R.ID},
                            {"status", R.Status},
                            {"file_bytes", R.Bytes},
                            {"before", endpoint(R.Before)},
                            {"after", endpoint(R.After)}};
}
llvm::json::Object record(const PackageBehaviorPattern &R) {
  return llvm::json::Object{
      {"pattern_group_id", R.ID},
      {"source_pair_id", R.PairID},
      {"api_class", R.API},
      {"kind", R.Kind},
      {"origin_evidence", R.OriginEvidence},
      {"encoding_class", R.Encoding},
      {"optional_syntax", R.Optional},
      {"spread_arguments", R.Spread},
      {"argument_count", R.ArgumentCount},
      {"recognized_count_before", R.Before},
      {"recognized_count_after", R.After},
      {"recognized_count_delta", int64_t(R.After) - int64_t(R.Before)},
      {"call_correspondence_verified", false}};
}
llvm::json::Object record(const PackageBehaviorOccurrence &R) {
  return llvm::json::Object{{"occurrence_id", R.ID},
                            {"source_pair_id", R.PairID},
                            {"pattern_group_id", R.GroupID},
                            {"side", R.Side},
                            {"package_behavior_id", R.PackageBehaviorID},
                            {"source_id", R.SourceID},
                            {"artifact_id", R.ArtifactID},
                            {"file_evidence_id", R.FileID},
                            {"node_behavior_id", R.BehaviorID},
                            {"behavior_record_id", R.RecordID},
                            {"node_id", R.NodeID},
                            {"byte_offset", std::to_string(R.Start)},
                            {"byte_length", std::to_string(R.End - R.Start)}};
}
} // namespace neverd::web::package_behavior_diff
