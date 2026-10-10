#include "SessionInternal.h"

namespace neverd::web {
namespace {
llvm::json::Value idOrNull(const std::string &ID) {
  return ID.empty() ? llvm::json::Value(nullptr) : llvm::json::Value(ID);
}
llvm::json::Object summary(const HTMLDocument &H, const HTMLLinks &L,
                           uint64_t Revision) {
  return llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(Revision)},
      {"html_id", H.ID},
      {"artifact_id", H.ArtifactID},
      {"blob_sha256", H.BlobHash},
      {"profile", std::string(HTMLProfile)},
      {"analysis_status", H.Status},
      {"reason", H.Reason},
      {"link_analysis_id", L.ID},
      {"link_status", L.Status},
      {"script_count", H.Scripts.size()},
      {"base_count", H.Bases.size()},
      {"steps", H.Steps},
      {"link_steps", L.Steps},
      {"decoded_attribute_bytes", H.DecodedBytes},
      {"tree_construction_verified", false},
      {"runtime_entries_verified", false},
      {"scripting_context", "enabled_candidate"},
      {"encoding_evidence", "caller_selected_utf8_profile"},
      {"byte_preprocessing", "none_raw_source_candidates"},
      {"import_map_analysis", "not_analyzed"},
      {"executes_input", false},
      {"resolves_external_references", false},
      {"redaction_policy", "metadata-only-v1"}};
}
} // namespace

std::string Session::analyzeHTML(std::string_view Revision,
                                 std::string_view ArtifactID) {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  if (const auto I = State->HTMLDocuments.find(std::string(ArtifactID));
      I != State->HTMLDocuments.end())
    return json(summary(I->second.Document, I->second.Links, State->Revision));
  if (State->HTMLDocuments.size() >= 4)
    throw Error("html_cache_budget_exceeded");
  const auto View = State->artifactView(ArtifactID);
  if (!View)
    throw Error("unknown_artifact");
  if (View->Origin.getString("kind") == "html_inline_script")
    throw Error("nested_html_analysis_unsupported");
  if (View->Content.size() > MaxHTMLBytes)
    throw Error("html_byte_budget_exceeded");
  auto H = inspectHTML(
      ArtifactID, View->Content.read(0, View->Content.size(), MaxHTMLBytes));
  if (H.BlobHash != View->BlobHash)
    throw Error("html_artifact_mismatch");
  const auto Members = State->memberNamespace(ArtifactID);
  auto L = linkHTMLScripts(H, Members ? *Members : State->Published);
  auto Reply = json(summary(H, L, State->Revision));
  State->HTMLDocuments.emplace(std::string(ArtifactID),
                               Impl::HTMLResults{std::move(H), std::move(L)});
  return Reply;
}

std::string Session::htmlRecords(std::string_view Revision,
                                 std::string_view HTMLID, std::string_view Kind,
                                 uint64_t Offset, uint64_t Limit) const {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Found = std::find_if(
      State->HTMLDocuments.begin(), State->HTMLDocuments.end(),
      [&](const auto &Pair) { return Pair.second.Document.ID == HTMLID; });
  if (Found == State->HTMLDocuments.end())
    throw Error("unknown_html_analysis");
  const auto &H = Found->second.Document;
  const auto &L = Found->second.Links;
  const auto Count = Kind == "scripts" ? H.Scripts.size()
                     : Kind == "bases" ? H.Bases.size()
                                       : throw Error("invalid_record_kind");
  if (!Limit || Limit > 512 || Offset > Count)
    throw Error("invalid_page");
  const auto End = std::min<uint64_t>(Count, Offset + Limit);
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I) {
    if (Kind == "bases") {
      const auto &B = H.Bases[I];
      Items.emplace_back(llvm::json::Object{
          {"base_id", B.ID},
          {"context", B.Context},
          {"byte_offset", std::to_string(B.Start)},
          {"byte_length", std::to_string(B.End - B.Start)},
          {"reference_byte_offset", std::to_string(B.ValueStart)},
          {"reference_byte_length", std::to_string(B.ValueLength)},
          {"selected_preceding_base_candidate", B.Selected},
          {"duplicate_attributes", B.DuplicateAttributes},
          {"reference_redacted", true},
          {"runtime_base_verified", false}});
      continue;
    }
    const auto &S = H.Scripts[I];
    const auto *Link = I < L.Scripts.size() ? &L.Scripts[I] : nullptr;
    Items.emplace_back(llvm::json::Object{
        {"script_id", S.ID},
        {"kind", S.Kind},
        {"context", S.Context},
        {"source_type", idOrNull(S.SourceType)},
        {"tag_byte_offset", std::to_string(S.TagStart)},
        {"tag_byte_length", std::to_string(S.TagEnd - S.TagStart)},
        {"body_byte_offset", std::to_string(S.BodyStart)},
        {"body_byte_length", std::to_string(S.BodyEnd - S.BodyStart)},
        {"end_byte", std::to_string(S.End)},
        {"reference_byte_offset",
         S.HasSource ? llvm::json::Value(std::to_string(S.ReferenceStart))
                     : llvm::json::Value(nullptr)},
        {"reference_byte_length",
         S.HasSource ? llvm::json::Value(std::to_string(S.ReferenceLength))
                     : llvm::json::Value(nullptr)},
        {"body_status", S.BodyStatus},
        {"has_src", S.HasSource},
        {"closed", S.Closed},
        {"async", S.Async},
        {"defer", S.Defer},
        {"nomodule", S.NoModule},
        {"integrity_present", S.Integrity},
        {"crossorigin_present", S.CrossOrigin},
        {"duplicate_attributes", S.DuplicateAttributes},
        {"inline_artifact_id", idOrNull(S.InlineArtifactID)},
        {"preceding_base_id", S.Base == NoHTMLIndex
                                  ? llvm::json::Value(nullptr)
                                  : llvm::json::Value(H.Bases.at(S.Base).ID)},
        {"base_status", Link ? Link->BaseStatus : "unavailable"},
        {"link_status", Link ? Link->Status : "unavailable"},
        {"candidate_artifact_id",
         Link ? idOrNull(Link->ArtifactID) : llvm::json::Value(nullptr)},
        {"query_present", Link && Link->Query},
        {"fragment_present", Link && Link->Fragment},
        {"reference_redacted", true},
        {"runtime_entry_verified", false}});
  }
  auto Reply = summary(H, L, State->Revision);
  Reply["record_kind"] = std::string(Kind);
  Reply["items"] = std::move(Items);
  Reply["offset"] = Offset;
  Reply["page_complete"] = End == Count;
  Reply["next_offset"] =
      End == Count ? llvm::json::Value(nullptr) : llvm::json::Value(End);
  return json(std::move(Reply));
}
} // namespace neverd::web
