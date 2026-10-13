//===- Publication.cpp - Passive report metadata projection -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// No raw examples, names, values, URL keys or protocol identifiers.
///
//===----------------------------------------------------------------------===//

#include "Publication.h"

namespace neverd::web::passive_report {
llvm::json::Object summary(const PassiveReport &R, uint64_t Revision) {
  return llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(Revision)},
      {"passive_report_id", R.ID},
      {"profile", std::string(PassiveReportProfile)},
      {"source_id", R.SourceID},
      {"har_capture_id", R.HARID},
      {"stream_capture_id", R.StreamID},
      {"source_status", R.SourceStatus},
      {"record_count", R.Records.size()},
      {"static_records", R.StaticRecords},
      {"observed_records", R.ObservedRecords},
      {"http_candidates", R.HTTPPairs},
      {"mcp_method_candidates", R.MethodCandidates},
      {"steps", R.Steps},
      {"analysis_status", "partial"},
      {"schema_meaning", "observed_structure_and_presence"},
      {"field_names", "capture_local_opaque_identity"},
      {"url_component_meaning", "lexical_syntax_only"},
      {"examples", "observation_ids_only_values_withheld"},
      {"inferred_optionality", "unknown"},
      {"server_constraints", "unknown"},
      {"server_completeness", "unknown"},
      {"api_identity", "syntax_unverified"},
      {"source_execution_observed", false},
      {"analyst_confirmed", false},
      {"permission_enforcement_verified", false},
      {"recorded_response_links", "imported_stream_owner_only"},
      {"mcp_candidate_rule", "known_method_spelling_only"},
      {"redaction_policy", "passive-report-metadata-v1"},
      {"all_input_values", "withheld"},
      {"network_access", false},
      {"executes_input", false}};
}
llvm::json::Object record(const PassiveReportRecord &R) {
  llvm::json::Object Types;
  for (const auto &[Type, Count] : R.Types)
    Types[Type] = Count;
  llvm::json::Value Structure = nullptr;
  if (R.Structure) {
    const auto &S = *R.Structure;
    auto Number = [](std::optional<uint64_t> V) -> llvm::json::Value {
      return V ? llvm::json::Value(*V) : llvm::json::Value(nullptr);
    };
    Structure = llvm::json::Object{
        {"node_id", S.NodeID},
        {"parent_node_id", S.ParentNodeID},
        {"group_id", S.GroupID},
        {"schema_node_id", S.SchemaNodeID},
        {"edge", S.Edge},
        {"name_class", S.NameClass},
        {"component", S.Component},
        {"ordinal", S.Ordinal},
        {"component_index", Number(S.ComponentIndex)},
        {"observed_instances", Number(S.Instances)},
        {"eligible_parent_instances", Number(S.EligibleParents)},
        {"observed_missing_instances", Number(S.MissingInstances)},
        {"decoded_utf16_offset", Number(S.DecodedOffset)},
        {"decoded_utf16_length", Number(S.DecodedLength)}};
  }
  return llvm::json::Object{
      {"record_id", R.ID},
      {"kind", R.Kind},
      {"evidence_class", R.EvidenceClass},
      {"anchor_id", R.AnchorID},
      {"related_id", R.RelatedID},
      {"parent_id", R.ParentID},
      {"source_id", R.SourceID},
      {"artifact_id", R.ArtifactID},
      {"byte_offset", R.CoordinatesPresent
                          ? llvm::json::Value(std::to_string(R.Offset))
                          : llvm::json::Value(nullptr)},
      {"byte_length", R.CoordinatesPresent
                          ? llvm::json::Value(std::to_string(R.Length))
                          : llvm::json::Value(nullptr)},
      {"method_class", R.Method},
      {"domain", R.Domain},
      {"direction", R.Direction},
      {"shape_status", R.ShapeStatus},
      {"shape_kind", R.ShapeKind},
      {"top_level_members",
       R.ShapeStatus == "observed_json_types" &&
               (R.ShapeKind == "object" || R.ShapeKind == "array")
           ? llvm::json::Value(R.Members)
           : llvm::json::Value(nullptr)},
      {"observed_type_counts", std::move(Types)},
      {"structure", std::move(Structure)},
      {"reason", R.Reason},
      {"observed_example_id", R.EvidenceClass == "imported_observation" &&
                                      R.Kind != "observed_json_schema_node"
                                  ? R.AnchorID
                                  : ""},
      {"inferred_optionality", "unknown"},
      {"server_constraints", "unknown"},
      {"source_execution_observed", false},
      {"analyst_confirmed", false}};
}
} // namespace neverd::web::passive_report
