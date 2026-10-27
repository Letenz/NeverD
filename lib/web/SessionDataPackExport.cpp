//===- SessionDataPackExport.cpp - Explicit local resource disclosure ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exclusive fixed filenames; aliases reference the same verified stored file.
/// Completion is published only after every selected byte hash is reread.
///
//===----------------------------------------------------------------------===//

#include "ExportDirectory.h"
#include "SessionInternal.h"

#include "neverd/web/Limits.h"

namespace neverd::web {
std::string Session::exportDataPack(std::string_view Revision,
                                    std::string_view ExtractionID,
                                    std::string_view OutputDirectory) {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Found = State->DataPacks.find(std::string(ExtractionID));
  if (Found == State->DataPacks.end())
    throw Error("unknown_datapack_extraction");
  const auto &E = Found->second;
  if (E.ResourceCount >
      ::neverd::limits::web::
          kSessionDataPackExportSessionExportDataPackEResourceCountLimit)
    throw Error("datapack_export_resource_budget_exceeded");
  ExportDirectory Out(OutputDirectory);
  llvm::json::Array Resources, Decoded;
  for (const auto &R : E.Resources) {
    const auto Filename =
        "resource-" + std::to_string(R.CanonicalIndex) + ".bin";
    if (!R.Alias)
      Out.write(Filename, R.Content, R.BlobHash);
    const auto Origin = State->artifactView(R.ID);
    if (!Origin)
      throw Error("datapack_resource_bytes_unavailable");
    Resources.emplace_back(llvm::json::Object{
        {"selection_id", R.ID},
        {"resource_id", R.ResourceID},
        {"alias", R.Alias},
        {"file", Filename},
        {"stored_sha256", R.BlobHash},
        {"stored_byte_length", std::to_string(R.Content.size())},
        {"origin", llvm::json::Object(Origin->Origin)}});
  }
  for (const auto &[ID, D] : State->DecodedDataPackResources) {
    if (D.ExtractionID != E.ID)
      continue;
    const auto Filename = "decoded-" + std::to_string(Decoded.size()) + ".bin";
    Out.write(Filename, D.Content, D.BlobHash);
    const auto Origin = State->artifactView(D.ID);
    if (!Origin)
      throw Error("datapack_decoded_bytes_unavailable");
    Decoded.emplace_back(llvm::json::Object{
        {"selection_id", D.ID},
        {"file", Filename},
        {"codec", "gzip-rfc1952-v1"},
        {"decoded_sha256", D.BlobHash},
        {"decoded_byte_length", std::to_string(D.Content.size())},
        {"origin", llvm::json::Object(Origin->Origin)}});
  }
  const auto Manifest =
      json(llvm::json::Object{{"schema_version", 1},
                              {"status", "complete"},
                              {"profile", E.Profile},
                              {"revision", std::to_string(State->Revision)},
                              {"extraction_id", E.ID},
                              {"container_sha256", E.BlobHash},
                              {"original_resource_names", "not_supplied"},
                              {"runtime_resource_binding", "not_checked"},
                              {"resources", std::move(Resources)},
                              {"decoded_resources", std::move(Decoded)},
                              {"executes_input", false}});
  Out.complete(Manifest);
  return json(
      llvm::json::Object{{"schema_version", 1},
                         {"status", "ok"},
                         {"revision", std::to_string(State->Revision)},
                         {"extraction_id", E.ID},
                         {"profile", E.Profile},
                         {"export_status", "complete"},
                         {"file_count", std::to_string(Out.files())},
                         {"verified_bytes", std::to_string(Out.bytes())},
                         {"executes_input", false},
                         {"redaction_policy", "metadata-only-v1"}});
}
} // namespace neverd::web
