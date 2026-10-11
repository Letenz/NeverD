//===- Publication.cpp - Node source and package evidence metadata ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Publish identifiers and fixed syntax classes without argument values.
///
//===----------------------------------------------------------------------===//

#include "Publication.h"

namespace neverd::web::node_behavior {
namespace {
llvm::json::Value nullable(const std::string &Text) {
  return Text.empty() ? llvm::json::Value(nullptr) : llvm::json::Value(Text);
}
llvm::json::Array ids(const std::vector<std::string> &IDs) {
  llvm::json::Array Result;
  for (const auto &ID : IDs)
    Result.push_back(ID);
  return Result;
}
} // namespace

llvm::json::Object summary(const NodeBehavior &A, uint64_t Revision) {
  return llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(Revision)},
      {"node_behavior_id", A.ID},
      {"source_id", A.SourceID},
      {"artifact_id", A.ArtifactID},
      {"profile", std::string(NodeBehaviorProfile)},
      {"source_type", A.SourceType},
      {"analysis_status", A.Status},
      {"reason", A.Reason},
      {"binding_analysis_id", nullable(A.BindingID)},
      {"module_analysis_id", nullable(A.ModuleID)},
      {"origin_analysis_id", nullable(A.OriginID)},
      {"value_analysis_id", nullable(A.ValueID)},
      {"origin_status", A.OriginStatus},
      {"origin_reason", A.OriginReason},
      {"value_status", A.ValueStatus},
      {"record_count", A.Records.size()},
      {"call_syntax_count", A.Calls},
      {"unattributed_call_count", A.UnattributedCalls},
      {"outside_rule_call_count", A.UnmatchedCalls},
      {"construction_boundary_count", A.ConstructionCalls},
      {"steps", A.Steps},
      {"coverage", "explicit_import_rooted_call_syntax"},
      {"runtime_api_verified", false},
      {"reachability_verified", false},
      {"successful_io_verified", false},
      {"safety_verdict", "not_assessed"},
      {"executes_input", false},
      {"redaction_policy", "metadata-only-v1"}};
}

llvm::json::Object summary(const PackageBehavior &A, uint64_t Revision) {
  return llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(Revision)},
      {"package_behavior_id", A.ID},
      {"package_analysis_id", A.PackageAnalysisID},
      {"profile", std::string(NodeBehaviorProfile)},
      {"analysis_status", "partial"},
      {"source_count", A.Sources.size()},
      {"unavailable_source_count", A.UnavailableSources},
      {"record_count", A.RecordCount},
      {"steps", A.Steps},
      {"scope_evidence", "caller_selected_package_and_sources"},
      {"selection_complete", false},
      {"install_entry_propagation", "not_analyzed"},
      {"runtime_api_verified", false},
      {"reachability_verified", false},
      {"safety_verdict", "not_assessed"},
      {"executes_input", false},
      {"redaction_policy", "metadata-only-v1"}};
}

llvm::json::Object record(const NodeBehaviorRecord &R,
                          std::string_view SourceID,
                          std::string_view ArtifactID) {
  llvm::json::Array Arguments;
  for (const auto &A : R.Arguments)
    Arguments.emplace_back(llvm::json::Object{
        {"node_id", A.NodeID}, {"ordinal", A.Ordinal}, {"spread", A.Spread}});
  return llvm::json::Object{
      {"record_id", R.ID},
      {"source_id", std::string(SourceID)},
      {"artifact_id", std::string(ArtifactID)},
      {"kind", R.Kind},
      {"api_class", R.API},
      {"node_id", R.NodeID},
      {"callee_node_id", R.CalleeID},
      {"receiver_node_id", nullable(R.ReceiverID)},
      {"request_id", R.RequestID},
      {"request_node_id", R.RequestNodeID},
      {"import_id", nullable(R.ImportID)},
      {"origin_evidence", R.OriginEvidence},
      {"byte_offset", std::to_string(R.Start)},
      {"byte_length", std::to_string(R.End - R.Start)},
      {"optional_syntax", R.Optional},
      {"argument_count", R.ArgumentCount},
      {"spread_arguments", R.Spread},
      {"argument_anchors", std::move(Arguments)},
      {"argument_anchors_truncated", R.Arguments.size() != R.ArgumentCount},
      {"parameter_roles", "not_resolved"},
      {"encoding_class", R.Encoding},
      {"runtime_api_verified", false},
      {"reachability_verified", false},
      {"successful_io_verified", false},
      {"values_redacted", true}};
}

llvm::json::Object source(const PackageBehaviorSource &S) {
  const bool Direct = S.Relation == "direct_file";
  return llvm::json::Object{
      {"source_id", S.SourceID},
      {"artifact_id", S.ArtifactID},
      {"node_behavior_id", S.BehaviorID},
      {"analysis_status", S.Status},
      {"reason", S.Reason},
      {"record_count", S.Records.size()},
      {"file_evidence_id", S.FileID},
      {"delivery_artifact_id", S.DeliveryArtifactID},
      {"relation", S.Relation},
      {"origin_evidence_ids", ids(S.OriginIDs)},
      {"package_id",
       Direct ? nullable(S.PackageID) : llvm::json::Value(nullptr)},
      {"delivery_package_id", nullable(S.PackageID)},
      {"package_ownership", !Direct               ? "not_inferred"
                            : S.PackageID.empty() ? "ownership_unrecorded"
                                                  : "inventory_recorded"},
      {"declared_entry_count", S.EntryCount},
      {"declared_entry_ids", ids(S.EntryIDs)},
      {"declared_entry_ids_truncated", S.EntryIDs.size() != S.EntryCount},
      {"runtime_entry_verified", false}};
}
} // namespace neverd::web::node_behavior
