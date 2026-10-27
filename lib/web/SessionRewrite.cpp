//===- SessionRewrite.cpp - Local source rewrite publication ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Publish bounded metadata; disclose raw files only through explicit export.
///
//===----------------------------------------------------------------------===//

#include "SessionInternal.h"
#include "rewrite/Export.h"

#include "neverd/web/Limits.h"

#include <algorithm>

namespace neverd::web {
namespace {
bool verified(const SourceRewrite &A) {
  return A.Status == "verified_local_rewrites" ||
         A.Status == "no_eligible_rewrites";
}

std::string exportReceipt(const SourceRewrite &A, uint64_t Revision) {
  return identity("source-rewrite-export",
                  {A.ID, A.SourceID, A.SourceType, A.Pass, A.SourceHash,
                   A.CandidateHash, sourceRewriteProfile(A.Pass),
                   "raw-original-candidate-v1", std::to_string(Revision)});
}

llvm::json::Object summary(const SourceRewrite &A, uint64_t Revision) {
  llvm::json::Object Result{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(Revision)},
      {"rewrite_id", A.ID},
      {"source_id", A.SourceID},
      {"artifact_id", A.ArtifactID},
      {"source_type", A.SourceType},
      {"profile", std::string(sourceRewriteProfile(A.Pass))},
      {"pass", A.Pass},
      {"analysis_status", A.Status},
      {"analysis_reason", A.Reason},
      {"source_sha256", A.SourceHash},
      {"candidate_sha256", A.CandidateHash},
      {"source_bytes", std::to_string(A.SourceSize)},
      {"candidate_bytes", std::to_string(A.Text.size())},
      {"binding_analysis_id", A.BindingID},
      {"value_analysis_id", A.ValueID},
      {"effect_analysis_id", A.EffectID},
      {"value_profile", std::string(JavaScriptValueProfile)},
      {"effect_profile", std::string(JavaScriptEffectProfile)},
      {"record_count", A.Records.size()},
      {"accepted_count", A.Accepted},
      {"refused_count", A.Refused},
      {"rewrite_steps", A.Steps},
      {"inverse_verification",
       verified(A) ? "exact_original_sha256" : "unavailable"},
      {"outside_tree_verification",
       verified(A) ? "same_retained_structure" : "unavailable"},
      {"value_verification",
       verified(A) ? "primitive_same_value" : "unavailable"},
      {"whole_program_equivalence", false},
      {"source_reflection_preserved", false},
      {"derived_source_map", "not_generated"},
      {"captured_source_mutated", false},
      {"executes_input", false},
      {"export_receipt", verified(A)
                             ? llvm::json::Value(exportReceipt(A, Revision))
                             : llvm::json::Value(nullptr)},
      {"export_disclosure", "raw_original_and_candidate_local_files"},
      {"redaction_policy", "metadata-only-v1"}};
  if (A.Pass == "scope_renaming") {
    Result["value_analysis_id"] = nullptr;
    Result["effect_analysis_id"] = nullptr;
    Result["value_profile"] = nullptr;
    Result["effect_profile"] = nullptr;
    Result["value_verification"] = "not_applicable";
    Result["outside_tree_verification"] =
        verified(A) ? "same_tree_except_selected_identifier_names"
                    : "unavailable";
    Result["binding_verification"] =
        verified(A) ? "same_scopes_declarations_and_reference_resolution"
                    : "unavailable";
    Result["original_name_recovery"] = false;
  } else if (A.Pass == "boolean_branches") {
    Result["value_verification"] =
        verified(A) ? "constant_boolean_condition_only" : "unavailable";
    Result["outside_tree_verification"] =
        verified(A) ? "same_retained_tree_with_get_value_sequence"
                    : "unavailable";
    Result["binding_verification"] =
        verified(A) ? "same_scopes_declarations_and_retained_references"
                    : "unavailable";
    Result["retained_expression_effects"] = "preserved_not_assumed_absent";
    Result["statements_rewritten"] = false;
  }
  return Result;
}

llvm::json::Object record(const SourceRewriteRecord &R) {
  const bool Accepted = R.Status == "accepted";
  llvm::json::Object Result{
      {"record_id", R.ID},
      {"node_id", R.NodeID},
      {"status", R.Status},
      {"reason", R.Reason},
      {"value_kind", R.ValueKind.empty() ? llvm::json::Value(nullptr)
                                         : llvm::json::Value(R.ValueKind)},
      {"source_byte_offset", std::to_string(R.SourceStart)},
      {"source_byte_length", std::to_string(R.SourceEnd - R.SourceStart)},
      {"candidate_byte_offset", Accepted
                                    ? llvm::json::Value(std::to_string(R.Start))
                                    : llvm::json::Value(nullptr)},
      {"candidate_byte_length",
       Accepted ? llvm::json::Value(std::to_string(R.End - R.Start))
                : llvm::json::Value(nullptr)}};
  if (!R.BindingID.empty()) {
    Result["binding_id"] = R.BindingID;
    Result["identifier_role"] = R.Role;
  }
  if (!R.ConditionID.empty()) {
    Result["condition_node_id"] = R.ConditionID;
    Result["retained_node_id"] =
        Accepted ? llvm::json::Value(R.RetainedID) : llvm::json::Value(nullptr);
    Result["selected_operand"] =
        Accepted ? llvm::json::Value(R.Selection) : llvm::json::Value(nullptr);
  }
  return Result;
}
} // namespace

std::string Session::analyzeSourceRewrite(std::string_view Revision,
                                          std::string_view SourceID,
                                          std::string_view Pass) {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  if (sourceRewriteProfile(Pass).empty())
    throw Error("unknown_source_rewrite_pass");
  const auto Found = State->Sources.find(std::string(SourceID));
  if (Found == State->Sources.end())
    throw Error("unknown_source");
  for (const auto &[ID, A] : State->SourceRewrites)
    if (A.SourceID == SourceID && A.Pass == Pass)
      return json(summary(A, State->Revision));
  if (State->SourceRewrites.size() >= MaxCachedSourceRewrites)
    throw Error("source_rewrite_cache_budget_exceeded");
  const auto &S = Found->second;
  const auto Bytes = State->sourceBytes(S.ArtifactID, MaxSourceRewriteBytes,
                                        "source_rewrite_byte_budget_exceeded");
  auto A = rewriteJavaScript(S.ArtifactID, Bytes, S.SourceType, Pass);
  if (A.SourceID != S.ID || A.SourceHash != S.BlobHash)
    throw Error("source_rewrite_source_mismatch");
  auto Reply = json(summary(A, State->Revision));
  if (!A.ID.empty()) {
    const auto ID = A.ID;
    State->SourceRewrites.emplace(ID, std::move(A));
  }
  return Reply;
#endif
}

std::string Session::sourceRewriteRecords(std::string_view Revision,
                                          std::string_view RewriteID,
                                          uint64_t Offset,
                                          uint64_t Limit) const {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Found = State->SourceRewrites.find(std::string(RewriteID));
  if (Found == State->SourceRewrites.end())
    throw Error("unknown_source_rewrite");
  const auto &A = Found->second;
  if (!Limit || Limit > ::neverd::limits::web::kMaxSessionPageRows ||
      Offset > A.Records.size())
    throw Error("invalid_page");
  const auto End = std::min<uint64_t>(A.Records.size(), Offset + Limit);
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I)
    Items.emplace_back(record(A.Records[I]));
  auto Reply = summary(A, State->Revision);
  Reply["items"] = std::move(Items);
  Reply["offset"] = Offset;
  Reply["page_complete"] = End == A.Records.size();
  Reply["next_offset"] = End == A.Records.size() ? llvm::json::Value(nullptr)
                                                 : llvm::json::Value(End);
  return json(std::move(Reply));
#endif
}

std::string Session::exportSourceRewrite(std::string_view Revision,
                                         std::string_view RewriteID,
                                         std::string_view Receipt,
                                         std::string_view OutputDirectory) {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Found = State->SourceRewrites.find(std::string(RewriteID));
  if (Found == State->SourceRewrites.end())
    throw Error("unknown_source_rewrite");
  const auto &A = Found->second;
  if (!verified(A))
    throw Error("source_rewrite_not_verified");
  if (Receipt != exportReceipt(A, State->Revision))
    throw Error("source_rewrite_export_receipt_mismatch");
  const auto Original = undoSourceRewrite(A, A.Text);
  auto Manifest = summary(A, State->Revision);
  Manifest["original_file"] = "original.js";
  Manifest["candidate_file"] = "candidate.js";
  Manifest["receipt_file"] = "receipt.json";
  Manifest["inverse_recipe"] = "replace_candidate_ranges_with_original_ranges";
  llvm::json::Array Items;
  for (const auto &R : A.Records)
    Items.emplace_back(record(R));
  Manifest["records"] = std::move(Items);
  const auto Text = json(std::move(Manifest));
  auto Reply = summary(A, State->Revision);
  Reply["export_status"] = "verified_local_files";
  Reply["receipt_sha256"] = sha256(Text);
  Reply["file_count"] = 3;
  auto Response = json(std::move(Reply));
  rewrite::Export Output(OutputDirectory);
  Output.write(rewrite::ExportFile::Original, Original);
  Output.write(rewrite::ExportFile::Candidate, A.Text);
  Output.write(rewrite::ExportFile::Receipt, Text);
  Output.commit();
  return Response;
#endif
}

} // namespace neverd::web
