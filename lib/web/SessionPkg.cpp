//===- SessionPkg.cpp - pkg evidence publication -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Revision-bound pkg selections with private metadata and explicit
/// uncertainty.
///
//===----------------------------------------------------------------------===//

#include "SessionInternal.h"

namespace neverd::web {
namespace {
llvm::json::Object summary(const PkgExtraction &E, uint64_t Revision) {
  return llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(Revision)},
      {"extraction_id", E.ID},
      {"artifact_id", E.ArtifactID},
      {"container_sha256", E.BlobHash},
      {"profile", std::string(PkgProfile)},
      {"layout_status", "compatible"},
      {"producer_version_verified", false},
      {"native_prefix_validation", "not_checked"},
      {"signature_status", "not_checked"},
      {"runtime_activation", "not_checked"},
      {"path_style", E.PathStyle},
      {"locator_line_style", E.LocatorLineStyle},
      {"locator_offset", std::to_string(E.LocatorOffset)},
      {"locator_size", std::to_string(E.LocatorSize)},
      {"payload_offset", std::to_string(E.PayloadOffset)},
      {"payload_size", std::to_string(E.PayloadSize)},
      {"referenced_payload_bytes", std::to_string(E.ReferencedBytes)},
      {"unreferenced_payload_bytes",
       std::to_string(E.PayloadSize - E.ReferencedBytes)},
      {"payload_coverage", E.ReferencedBytes == E.PayloadSize
                               ? "all_bytes_indexed"
                               : "unreferenced_bytes_retained"},
      {"path_count", E.PathCount},
      {"content_file_count", E.ContentFileCount},
      {"cache_only_file_count", E.CacheOnlyFileCount},
      {"unavailable_file_count", E.UnavailableFileCount},
      {"directory_count", E.DirectoryCount},
      {"member_count", E.Members.size()},
      {"region_count", E.Regions.size()},
      {"link_count", E.Links.size()},
      {"unique_store_ranges", E.UniqueStoreRanges},
      {"region_coverage", "full_container_partition"},
      {"entry_declaration", "path_redacted"},
      {"entry_match", E.LiteralEntryMemberID.empty()
                          ? "no_selectable_content_match"
                          : "literal_path_only"},
      {"entry_literal_member_id",
       E.LiteralEntryMemberID.empty()
           ? llvm::json::Value(nullptr)
           : llvm::json::Value(E.LiteralEntryMemberID)},
      {"entry_link_status", E.EntrySymlinkPrefix ? "symlink_prefix_unresolved"
                                                 : "no_declared_prefix_link"},
      {"entry_cache_present", E.EntryCachePresent},
      {"entry_runtime_membership", "not_checked"},
      {"source_cache_equivalence", "not_checked"},
      {"v8_decoding", "opaque"},
      {"reader_steps", std::to_string(E.Steps)},
      {"scanned_bytes", std::to_string(E.ScannedBytes)},
      {"hashed_bytes", std::to_string(E.HashedBytes)},
      {"json_nodes", std::to_string(E.JSONNodes)},
      {"executes_input", false},
      {"redaction_policy", "metadata-only-v1"}};
}
} // namespace

std::string Session::extractPkg(std::string_view Revision,
                                std::string_view ArtifactID,
                                std::string_view Profile) {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  if (Profile != PkgProfile)
    throw Error("pkg_unsupported_profile");
  uint64_t PrivateBytes = 0;
  for (const auto &[ID, E] : State->PkgExtractions) {
    if (E.ArtifactID == ArtifactID)
      return json(summary(E, State->Revision));
    PrivateBytes += E.PrivateBytes;
  }
  if (State->PkgExtractions.size() >= MaxCachedPkgExtractions)
    throw Error("pkg_cache_budget_exceeded");
  const auto View = State->artifactView(ArtifactID);
  if (!View)
    throw Error("unknown_artifact");
  Artifact A;
  A.ID = ArtifactID;
  A.BlobHash = View->BlobHash;
  A.Content = View->Content;
  auto E = web::extractPkg(A, Profile);
  if (PrivateBytes > MaxCachedPkgPrivateBytes ||
      E.PrivateBytes > MaxCachedPkgPrivateBytes - PrivateBytes)
    throw Error("pkg_cache_metadata_budget_exceeded");
  auto Reply = json(summary(E, State->Revision));
  const auto ID = E.ID;
  State->PkgExtractions.emplace(ID, std::move(E));
  return Reply;
}

std::string Session::pkgRecords(std::string_view Revision,
                                std::string_view ExtractionID,
                                std::string_view Kind, uint64_t Offset,
                                uint64_t Limit) const {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Found = State->PkgExtractions.find(std::string(ExtractionID));
  if (Found == State->PkgExtractions.end())
    throw Error("unknown_pkg_extraction");
  const auto &E = Found->second;
  const auto Total = Kind == "members"   ? E.Members.size()
                     : Kind == "regions" ? E.Regions.size()
                     : Kind == "links"   ? E.Links.size()
                                         : throw Error("invalid_record_kind");
  if (!Limit || Limit > MaxPkgPageRecords || Offset > Total)
    throw Error("invalid_page");
  const auto End = std::min<uint64_t>(Total, Offset + Limit);
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I) {
    if (Kind == "members") {
      const auto &M = E.Members[I];
      Items.emplace_back(llvm::json::Object{
          {"member_id", M.ID},
          {"member_index", I},
          {"storage_range_id", M.RangeID},
          {"parent_artifact_id", E.ArtifactID},
          {"namespace_id", E.ID},
          {"kind", pkgStoreKind(M.Store)},
          {"declared_kind", M.Directory ? "directory" : "file"},
          {"blob_sha256", M.privateHash() ? llvm::json::Value(nullptr)
                                          : llvm::json::Value(M.BlobHash)},
          {"hash_redacted", M.privateHash()},
          {"offset", std::to_string(M.Offset)},
          {"payload_byte_offset", std::to_string(M.Offset - E.PayloadOffset)},
          {"size", std::to_string(M.Content.size())},
          {"declared_stat_size", std::to_string(M.DeclaredStatSize)},
          {"stored_size_relation",
           M.Store != PkgStore::Content             ? "not_applicable"
           : M.Content.size() == M.DeclaredStatSize ? "equal"
                                                    : "different"},
          {"source_preprocessing", "not_checked"},
          {"selection_id", M.selectable() ? llvm::json::Value(M.ID)
                                          : llvm::json::Value(nullptr)},
          {"selection_status", M.selectable()    ? "available"
                               : M.MetadataRange ? "private_metadata_range"
                                                 : "opaque_store"},
          {"cache_present", M.CachePresent},
          {"source_cache_equivalence", "not_checked"},
          {"path_redacted", true}});
    } else if (Kind == "regions") {
      const auto &R = E.Regions[I];
      Items.emplace_back(llvm::json::Object{
          {"region_id", R.ID},
          {"region_index", I},
          {"parent_artifact_id", E.ArtifactID},
          {"kind", R.Kind},
          {"blob_sha256", R.MetadataRange ? llvm::json::Value(nullptr)
                                          : llvm::json::Value(R.BlobHash)},
          {"hash_redacted", R.MetadataRange},
          {"offset", std::to_string(R.Offset)},
          {"size", std::to_string(R.Content.size())},
          {"selection_id", R.selectable() ? llvm::json::Value(R.ID)
                                          : llvm::json::Value(nullptr)}});
    } else {
      Items.emplace_back(
          llvm::json::Object{{"link_id", E.Links[I].ID},
                             {"link_index", I},
                             {"kind", "virtual_symlink_declaration"},
                             {"path_redacted", true},
                             {"target_redacted", true},
                             {"resolution", "not_performed"}});
    }
  }
  auto Reply = summary(E, State->Revision);
  Reply["record_kind"] = std::string(Kind);
  Reply["items"] = std::move(Items);
  Reply["offset"] = Offset;
  Reply["page_complete"] = End == Total;
  Reply["next_offset"] =
      End == Total ? llvm::json::Value(nullptr) : llvm::json::Value(End);
  return json(std::move(Reply));
}
} // namespace neverd::web
