//===- Publication.cpp - Fixed installation evidence metadata -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Publish opaque anchors, bounded counts and explicit static-analysis limits.
///
//===----------------------------------------------------------------------===//

#include "Internal.h"

#include "neverd/web/SourceModules.h"

namespace neverd::web::package_install {
namespace {
llvm::json::Value anchor(const std::string &ID) {
  return ID.empty() ? llvm::json::Value(nullptr) : llvm::json::Value(ID);
}
llvm::json::Value offset(std::optional<uint64_t> Value) {
  return Value ? llvm::json::Value(std::to_string(*Value))
               : llvm::json::Value(nullptr);
}
} // namespace

llvm::json::Object summary(const PackageInstall &A, uint64_t Revision) {
  return llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(Revision)},
      {"package_install_id", A.ID},
      {"package_analysis_id", A.PackageAnalysisID},
      {"package_behavior_id", anchor(A.PackageBehaviorID)},
      {"namespace_id", A.NamespaceID},
      {"profile", std::string(PackageInstallProfile)},
      {"module_link_profile", std::string(NodeModuleLinkProfile)},
      {"analysis_status", "partial"},
      {"script_count", A.Scripts.size()},
      {"lifecycle_declaration_count", A.LifecycleDeclarations},
      {"install_presence_count", A.PresenceDeclarations},
      {"source_count", A.Sources.size()},
      {"direct_source_count", A.DirectSources},
      {"embedded_source_count", A.EmbeddedSources},
      {"unavailable_source_count", A.UnavailableSources},
      {"request_count", A.Requests.size()},
      {"unresolved_request_count", A.UnresolvedRequests},
      {"association_count", A.Associations.size()},
      {"steps", A.Steps},
      {"selection_complete", false},
      {"runtime_resolution_verified", false},
      {"executable_identity_verified", false},
      {"reachability_verified", false},
      {"lifecycle_activation_verified", false},
      {"dependency_edges_inferred", false},
      {"alternative_paths_enumerated", false},
      {"path_selection", "deterministic_shortest_candidate_path"},
      {"command_offsets", "decoded_command_bytes"},
      {"script_arguments", "uninterpreted_not_propagated"},
      {"safety_verdict", "not_assessed"},
      {"redaction_policy", "metadata-only-v1"}};
}
llvm::json::Object record(const PackageInstallScript &R) {
  return llvm::json::Object{{"id", R.ID},
                            {"script_id", anchor(R.ScriptID)},
                            {"package_id", R.PackageID},
                            {"manifest_artifact_id", anchor(R.ManifestID)},
                            {"kind", R.Kind},
                            {"status", R.Status},
                            {"command_status", R.CommandStatus},
                            {"command_start_byte", offset(R.CommandStart)},
                            {"command_end_byte", offset(R.CommandEnd)},
                            {"file_evidence_id", anchor(R.FileID)},
                            {"artifact_id", anchor(R.ArtifactID)},
                            {"source_id", anchor(R.SourceID)}};
}
llvm::json::Object record(const PackageInstallSource &R) {
  return llvm::json::Object{{"source_id", R.SourceID},
                            {"artifact_id", R.ArtifactID},
                            {"file_evidence_id", R.FileID},
                            {"package_id", anchor(R.PackageID)},
                            {"node_behavior_id", R.BehaviorID},
                            {"status", R.Status},
                            {"module_analysis_id", anchor(R.ModuleID)},
                            {"origin_analysis_id", anchor(R.OriginID)},
                            {"module_link_id", anchor(R.LinkID)}};
}
llvm::json::Object record(const PackageInstallRequest &R) {
  return llvm::json::Object{{"id", R.ID},
                            {"source_id", R.SourceID},
                            {"request_id", R.RequestID},
                            {"node_id", R.NodeID},
                            {"kind", R.Kind},
                            {"status", R.Status},
                            {"origin_evidence", anchor(R.OriginEvidence)},
                            {"target_artifact_id", anchor(R.ArtifactID)},
                            {"target_file_evidence_id", anchor(R.FileID)},
                            {"target_package_id", anchor(R.PackageID)},
                            {"target_source_id", anchor(R.TargetSourceID)}};
}
llvm::json::Object record(const PackageInstallAssociation &R) {
  llvm::json::Array Chain;
  for (const auto &ID : R.RequestIDs)
    Chain.push_back(ID);
  return llvm::json::Object{{"id", R.ID},
                            {"script_trace_id", R.ScriptTraceID},
                            {"script_id", R.ScriptID},
                            {"entry_file_evidence_id", R.EntryFileID},
                            {"source_id", R.SourceID},
                            {"node_behavior_id", R.BehaviorID},
                            {"behavior_record_id", R.RecordID},
                            {"node_id", R.NodeID},
                            {"api_class", R.API},
                            {"kind", R.Kind},
                            {"origin_evidence", R.OriginEvidence},
                            {"byte_offset", std::to_string(R.Start)},
                            {"byte_length", std::to_string(R.End - R.Start)},
                            {"file_evidence_id", R.FileID},
                            {"package_id", anchor(R.PackageID)},
                            {"request_trace_ids", std::move(Chain)},
                            {"reachability_verified", false}};
}
} // namespace neverd::web::package_install
