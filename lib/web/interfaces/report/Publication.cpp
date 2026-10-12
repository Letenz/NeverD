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
      {"schema_meaning", "observed_type_summary"},
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
      {"reason", R.Reason},
      {"observed_example_id",
       R.EvidenceClass == "imported_observation" ? R.AnchorID : ""},
      {"inferred_optionality", "unknown"},
      {"server_constraints", "unknown"},
      {"source_execution_observed", false},
      {"analyst_confirmed", false}};
}
} // namespace neverd::web::passive_report
