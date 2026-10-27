//===- SessionTauri.cpp - Revision-bound Tauri asset evidence -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Atomic decoded-spool publication and metadata-only initializer/member pages.
///
//===----------------------------------------------------------------------===//

#include "SessionInternal.h"

#include "neverd/web/Limits.h"

namespace neverd::web {
namespace {
llvm::json::Object summary(const TauriExtraction &E, uint64_t Revision) {
  return llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(Revision)},
      {"extraction_id", E.ID},
      {"artifact_id", E.ArtifactID},
      {"container_sha256", E.BlobHash},
      {"profile", E.Profile},
      {"container_format", "macho"},
      {"architecture", "x64"},
      {"platform", "not_inferred"},
      {"initializer_address", std::to_string(E.Initializer.Start)},
      {"initializer_end_address", std::to_string(E.Initializer.End)},
      {"initializer_container_offset", std::to_string(E.Initializer.Offset)},
      {"initializer_function_entry", std::to_string(E.Initializer.Function)},
      {"initializer_instructions", E.Initializer.Instructions},
      {"initializer_basis", "native_lowir_selected_constant_stores"},
      {"native_profile", "x64-local-full-width-constant-stores-v1"},
      {"native_steps", std::to_string(E.Initializer.NativeSteps)},
      {"displacements_container_offset", std::to_string(E.DisplacementsOffset)},
      {"entries_container_offset", std::to_string(E.EntriesOffset)},
      {"displacement_count", E.Initializer.Fields[1].Value},
      {"member_count", E.Members.size()},
      {"phf_lookup_status", "all_keys_match_rows"},
      {"codec", "brotli-rfc7932-static-sdk-1.2.0"},
      {"stored_bytes", std::to_string(E.StoredBytes)},
      {"decoded_bytes", std::to_string(E.DecodedBytes)},
      {"reader_steps", std::to_string(E.Steps)},
      {"codec_calls", std::to_string(E.CodecCalls)},
      {"scope", "selected_initializer_table"},
      {"rust_type_identity", "not_verified"},
      {"producer_version_verified", false},
      {"initializer_reachability", "not_checked"},
      {"runtime_table_mutation", "not_checked"},
      {"asset_server_binding", "not_checked"},
      {"native_bridge_analysis", "not_analyzed"},
      {"signature_status", "not_checked"},
      {"authenticates_publisher", false},
      {"executes_input", false},
      {"redaction_policy", "metadata-only-v1"}};
}
} // namespace
std::string Session::extractTauri(std::string_view Revision,
                                  std::string_view ArtifactID,
                                  const TauriInitializer &Initializer,
                                  std::string_view Profile) {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  uint64_t Decoded = 0;
  for (const auto &[ID, E] : State->TauriExtractions) {
    if (E.ArtifactID == ArtifactID && E.Profile == Profile &&
        E.Initializer.Start == Initializer.Start &&
        E.BlobHash == Initializer.InputHash)
      return json(summary(E, State->Revision));
    Decoded += E.DecodedBytes;
  }
  if (State->TauriExtractions.size() >= MaxCachedTauriExtractions)
    throw Error("tauri_cache_budget_exceeded");
  const auto View = State->artifactView(ArtifactID);
  if (!View)
    throw Error("unknown_artifact");
  Artifact A;
  A.ID = ArtifactID;
  A.BlobHash = View->BlobHash;
  A.Content = View->Content;
  TauriLimits Limits;
  Limits.DecodedBytes =
      std::min(Limits.DecodedBytes, MaxCachedTauriDecodedBytes - Decoded);
  auto E = web::extractTauri(A, Initializer, Profile, Limits);
  auto Reply = json(summary(E, State->Revision));
  const auto ID = E.ID;
  State->TauriExtractions.emplace(ID, std::move(E));
  return Reply;
}
std::string Session::tauriRecords(std::string_view Revision,
                                  std::string_view ExtractionID,
                                  uint64_t Offset, uint64_t Limit) const {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Found = State->TauriExtractions.find(std::string(ExtractionID));
  if (Found == State->TauriExtractions.end())
    throw Error("unknown_tauri_extraction");
  const auto &E = Found->second;
  if (!Limit || Limit > ::neverd::limits::web::kSessionTauriRecordsLimit ||
      Offset > E.Members.size())
    throw Error("invalid_page");
  const auto End = std::min<uint64_t>(E.Members.size(), Offset + Limit);
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I) {
    const auto &M = E.Members[I];
    Items.emplace_back(llvm::json::Object{
        {"member_id", M.ID},
        {"member_index", I},
        {"selection_id", M.ID},
        {"parent_artifact_id", E.ArtifactID},
        {"kind", M.Kind},
        {"kind_basis", "private_name_suffix"},
        {"source_format_verified", false},
        {"blob_sha256", M.BlobHash},
        {"stored_sha256", M.StoredHash},
        {"row_container_offset", std::to_string(M.RowOffset)},
        {"name_container_offset", std::to_string(M.NameOffset)},
        {"stored_container_offset", std::to_string(M.Offset)},
        {"stored_byte_length", std::to_string(M.Stored.size())},
        {"byte_length", std::to_string(M.Content.size())},
        {"decoded_byte_offset", "0"},
        {"path_redacted", true}});
  }
  auto Reply = summary(E, State->Revision);
  Reply["items"] = std::move(Items);
  Reply["offset"] = Offset;
  Reply["page_complete"] = End == E.Members.size();
  Reply["next_offset"] = End == E.Members.size() ? llvm::json::Value(nullptr)
                                                 : llvm::json::Value(End);
  return json(std::move(Reply));
}
} // namespace neverd::web
