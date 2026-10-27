//===- SessionWails.cpp - Revision-bound Wails evidence -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Metadata-only table/member pages and bounded immutable extraction caches.
///
//===----------------------------------------------------------------------===//

#include "SessionInternal.h"

#include "neverd/web/Limits.h"

namespace neverd::web {
namespace {
llvm::json::Object summary(const WailsExtraction &E, uint64_t Revision) {
  return llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(Revision)},
      {"extraction_id", E.ID},
      {"artifact_id", E.ArtifactID},
      {"container_sha256", E.BlobHash},
      {"profile", E.Profile},
      {"container_format", E.ContainerFormat},
      {"architecture", E.Architecture},
      {"platform", E.Platform},
      {"declared_go_version", "go1.25.7"},
      {"declared_wails_version", "v2.11.0"},
      {"producer_version_verified", false},
      {"layout_status", "compatible"},
      {"build_info_offset", std::to_string(E.BuildInfoOffset)},
      {"build_info_size", std::to_string(E.BuildInfoSize)},
      {"table_count", E.Tables.size()},
      {"member_count", E.Members.size()},
      {"payload_bytes", std::to_string(E.PayloadBytes)},
      {"reader_steps", std::to_string(E.Steps)},
      {"scanned_bytes", std::to_string(E.ScannedBytes)},
      {"hashed_bytes", std::to_string(E.HashedBytes)},
      {"stored_digest_status", "go_toolchain_hash128_match"},
      {"table_coverage", "matching_structural_framing_only"},
      {"filesystem_type_identity", "not_verified"},
      {"asset_server_binding", "not_checked"},
      {"native_bridge_analysis", "not_analyzed"},
      {"runtime_activation", "not_checked"},
      {"signature_status", "not_checked"},
      {"authenticates_publisher", false},
      {"unreferenced_container_bytes", "retained_in_original_artifact"},
      {"executes_input", false},
      {"redaction_policy", "metadata-only-v1"}};
}
} // namespace

std::string Session::extractWails(std::string_view Revision,
                                  std::string_view ArtifactID,
                                  std::string_view Profile) {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  uint64_t PrivateBytes = 0;
  for (const auto &[ID, E] : State->WailsExtractions) {
    if (E.ArtifactID == ArtifactID && E.Profile == Profile)
      return json(summary(E, State->Revision));
    PrivateBytes += E.PrivateBytes;
  }
  if (State->WailsExtractions.size() >= MaxCachedWailsExtractions)
    throw Error("wails_cache_budget_exceeded");
  const auto View = State->artifactView(ArtifactID);
  if (!View)
    throw Error("unknown_artifact");
  Artifact A;
  A.ID = ArtifactID;
  A.BlobHash = View->BlobHash;
  A.Content = View->Content;
  auto E = web::extractWails(A, Profile);
  if (E.PrivateBytes > MaxCachedWailsPrivateBytes - PrivateBytes)
    throw Error("wails_cache_metadata_budget_exceeded");
  auto Reply = json(summary(E, State->Revision));
  const auto ID = E.ID;
  State->WailsExtractions.emplace(ID, std::move(E));
  return Reply;
}

std::string Session::wailsRecords(std::string_view Revision,
                                  std::string_view ExtractionID,
                                  std::string_view Kind, uint64_t Offset,
                                  uint64_t Limit) const {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Found = State->WailsExtractions.find(std::string(ExtractionID));
  if (Found == State->WailsExtractions.end())
    throw Error("unknown_wails_extraction");
  const auto &E = Found->second;
  const auto Count = Kind == "tables"    ? E.Tables.size()
                     : Kind == "members" ? E.Members.size()
                                         : throw Error("invalid_record_kind");
  if (!Limit || Limit > ::neverd::limits::web::kSessionWailsRecordsLimit ||
      Offset > Count)
    throw Error("invalid_page");
  const auto End = std::min<uint64_t>(Count, Offset + Limit);
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I) {
    if (Kind == "tables") {
      const auto &T = E.Tables[I];
      Items.emplace_back(llvm::json::Object{
          {"table_id", T.ID},
          {"table_index", I},
          {"container_byte_offset", std::to_string(T.Offset)},
          {"byte_length", std::to_string(T.Size)},
          {"first_member_index", T.FirstMember},
          {"member_count", T.MemberCount},
          {"asset_server_binding", "not_checked"}});
    } else {
      const auto &M = E.Members[I];
      Items.emplace_back(llvm::json::Object{
          {"member_id", M.ID},
          {"member_index", I},
          {"table_id", M.TableID},
          {"table_index", M.Table},
          {"parent_artifact_id", E.ArtifactID},
          {"kind", M.Kind},
          {"kind_basis",
           M.Directory ? "directory_record" : "private_name_suffix"},
          {"source_format_verified", false},
          {"selection_id",
           M.Directory ? llvm::json::Value(nullptr) : llvm::json::Value(M.ID)},
          {"blob_sha256", M.Directory ? llvm::json::Value(nullptr)
                                      : llvm::json::Value(M.BlobHash)},
          {"container_byte_offset",
           M.Directory ? llvm::json::Value(nullptr)
                       : llvm::json::Value(std::to_string(M.Offset))},
          {"byte_length", std::to_string(M.Content.size())},
          {"name_byte_offset", std::to_string(M.NameOffset)},
          {"path_redacted", true},
          {"runtime_filesystem_binding", "not_checked"}});
    }
  }
  auto Reply = summary(E, State->Revision);
  Reply["record_kind"] = std::string(Kind);
  Reply["items"] = std::move(Items);
  Reply["offset"] = Offset;
  Reply["page_complete"] = End == Count;
  Reply["next_offset"] =
      End == Count ? llvm::json::Value(nullptr) : llvm::json::Value(End);
  return json(std::move(Reply));
}
} // namespace neverd::web
