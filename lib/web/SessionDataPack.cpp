//===- SessionDataPack.cpp - Chromium resource evidence publication -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Immutable resource selections, explicit decoding and revision-bound pages.
///
//===----------------------------------------------------------------------===//

#include "SessionInternal.h"
#include "compression/Gzip.h"

namespace neverd::web {
namespace {
llvm::json::Object summary(const DataPackExtraction &E, uint64_t Revision) {
  return llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(Revision)},
      {"extraction_id", E.ID},
      {"artifact_id", E.ArtifactID},
      {"container_sha256", E.BlobHash},
      {"profile", E.Profile},
      {"resource_id_width", E.Profile == DataPackProfile ? 16 : 32},
      {"layout_status", "compatible"},
      {"pack_version", 5},
      {"declared_encoding", E.Encoding == 0   ? "binary"
                            : E.Encoding == 1 ? "utf8"
                                              : "utf16le"},
      {"encoding_validated", false},
      {"resource_count", E.ResourceCount},
      {"alias_count", E.AliasCount},
      {"record_count", E.Resources.size()},
      {"index_byte_length", std::to_string(E.IndexBytes)},
      {"payload_byte_length", std::to_string(E.PayloadBytes)},
      {"region_coverage", "full_container_partition"},
      {"framework_identity", "not_inferred"},
      {"runtime_version", "not_inferred"},
      {"runtime_resource_binding", "not_checked"},
      {"resource_names", "not_supplied"},
      {"source_language", "caller_selected"},
      {"executes_input", false},
      {"redaction_policy", "metadata-only-v1"}};
}
} // namespace

std::string Session::extractDataPack(std::string_view Revision,
                                     std::string_view ArtifactID,
                                     std::string_view Profile) {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  if (Profile != DataPackProfile && Profile != WebViewDataPackProfile)
    throw Error("datapack_unsupported_profile");
  uint64_t Records = 0;
  for (const auto &[ID, E] : State->DataPacks) {
    if (E.ArtifactID == ArtifactID && E.Profile == Profile)
      return json(summary(E, State->Revision));
    Records += E.Resources.size();
  }
  if (State->DataPacks.size() >= MaxCachedDataPacks)
    throw Error("datapack_cache_budget_exceeded");
  const auto View = State->artifactView(ArtifactID);
  if (!View)
    throw Error("unknown_artifact");
  Artifact A;
  A.ID = ArtifactID;
  A.BlobHash = View->BlobHash;
  A.Content = View->Content;
  auto E = web::extractDataPack(A, Profile);
  if (E.Resources.size() > MaxCachedDataPackResources - Records)
    throw Error("datapack_cache_resource_budget_exceeded");
  auto Reply = json(summary(E, State->Revision));
  const auto ID = E.ID;
  State->DataPacks.emplace(ID, std::move(E));
  return Reply;
}

std::string Session::dataPackRecords(std::string_view Revision,
                                     std::string_view ExtractionID,
                                     uint64_t Offset, uint64_t Limit) const {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Found = State->DataPacks.find(std::string(ExtractionID));
  if (Found == State->DataPacks.end())
    throw Error("unknown_datapack_extraction");
  const auto &E = Found->second;
  if (!Limit || Limit > MaxDataPackPageRecords || Offset > E.Resources.size())
    throw Error("invalid_page");
  const auto End = std::min<uint64_t>(E.Resources.size(), Offset + Limit);
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I) {
    const auto &R = E.Resources[I];
    Items.emplace_back(llvm::json::Object{
        {"resource_id", R.ResourceID},
        {"record_index", I},
        {"selection_id", R.ID},
        {"kind", R.Alias ? "alias" : "stored_resource"},
        {"canonical_index", R.CanonicalIndex},
        {"canonical_selection_id", E.Resources[R.CanonicalIndex].ID},
        {"container_byte_offset", std::to_string(R.Offset)},
        {"stored_byte_length", std::to_string(R.Content.size())},
        {"stored_sha256", R.BlobHash},
        {"selection_status", "stored_bytes_available"},
        {"codec_hint",
         R.GzipCandidate ? "gzip_magic_candidate" : "unclassified"},
        {"codec_validated", false},
        {"filename", nullptr},
        {"source_language", "caller_selected"},
        {"runtime_resource_binding", "not_checked"}});
  }
  auto Reply = summary(E, State->Revision);
  Reply["items"] = std::move(Items);
  Reply["offset"] = Offset;
  Reply["page_complete"] = End == E.Resources.size();
  Reply["next_offset"] = End == E.Resources.size() ? llvm::json::Value(nullptr)
                                                   : llvm::json::Value(End);
  return json(std::move(Reply));
}

std::string Session::decodeDataPackResource(std::string_view Revision,
                                            std::string_view ResourceID,
                                            std::string_view Codec) {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  if (Codec != "gzip-rfc1952-v1")
    throw Error("datapack_unsupported_codec");
  auto Reply = [&](const DataPackDecodedResource &D) {
    return json(llvm::json::Object{
        {"schema_version", 1},
        {"status", "ok"},
        {"revision", std::to_string(State->Revision)},
        {"extraction_id", D.ExtractionID},
        {"resource_selection_id", D.ResourceID},
        {"selection_id", D.ID},
        {"codec", "gzip-rfc1952-v1"},
        {"decoded_sha256", D.BlobHash},
        {"decoded_byte_length", std::to_string(D.Content.size())},
        {"source_language", "caller_selected"},
        {"executes_input", false},
        {"redaction_policy", "metadata-only-v1"}});
  };
  uint64_t Bytes = 0;
  for (const auto &[ID, D] : State->DecodedDataPackResources) {
    if (D.ResourceID == ResourceID)
      return Reply(D);
    Bytes += D.Content.size();
  }
  const DataPackExtraction *Pack = nullptr;
  const DataPackResource *Resource = nullptr;
  for (const auto &[ID, E] : State->DataPacks)
    for (const auto &R : E.Resources)
      if (R.ID == ResourceID) {
        Pack = &E;
        Resource = &R;
      }
  if (!Resource)
    throw Error("unknown_datapack_resource");
  if (State->DecodedDataPackResources.size() >= MaxDecodedDataPackResources)
    throw Error("datapack_decoded_cache_budget_exceeded");
  DataPackDecodedResource D;
  D.ResourceID = ResourceID;
  D.ExtractionID = Pack->ID;
  D.Content =
      compression::decodeGzip(Resource->Content, MaxDataPackDecodedBytes);
  if (D.Content.size() > MaxCachedDataPackDecodedBytes - Bytes)
    throw Error("datapack_decoded_byte_budget_exceeded");
  D.BlobHash = D.Content.digest();
  D.ID =
      identity("datapack-decoded-resource", {D.ResourceID, Codec, D.BlobHash});
  auto Result = Reply(D);
  const auto ID = D.ID;
  State->DecodedDataPackResources.emplace(ID, std::move(D));
  return Result;
}
} // namespace neverd::web
