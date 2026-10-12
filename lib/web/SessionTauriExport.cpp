//===- SessionTauriExport.cpp - Verified local Tauri evidence export ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original image, exact compressed frames and decoded assets under fixed
/// names. Stored asset keys are manifest data only; the shared writer owns all
/// paths.
///
//===----------------------------------------------------------------------===//

#include "ExportDirectory.h"
#include "SessionInternal.h"

namespace neverd::web {
std::string Session::exportTauri(std::string_view Revision,
                                 std::string_view ExtractionID,
                                 std::string_view OutputDirectory) {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Found = State->TauriExtractions.find(std::string(ExtractionID));
  if (Found == State->TauriExtractions.end())
    throw Error("unknown_tauri_extraction");
  const auto &E = Found->second;
  const auto Original = State->artifactView(E.ArtifactID);
  if (!Original)
    throw Error("unknown_artifact");
  ExportDirectory Output(OutputDirectory);
  Output.write("original.bin", Original->Content, Original->BlobHash);
  llvm::json::Array Members, Fields;
  uint64_t JavaScript = 0;
  for (uint64_t I = 0; I < E.Members.size(); ++I) {
    const auto &M = E.Members[I];
    auto Number = std::to_string(I);
    Number.insert(0, 5 - std::min<size_t>(5, Number.size()), '0');
    const auto Prefix = "m" + Number;
    const auto StoredFile = Prefix + ".br";
    const auto DecodedFile = Prefix + (M.Kind == "javascript" ? ".js"
                                       : M.Kind == "html"     ? ".html"
                                       : M.Kind == "css"      ? ".css"
                                                              : ".bin");
    Output.write(StoredFile, M.Stored, M.StoredHash);
    Output.write(DecodedFile, M.Content, M.BlobHash);
    JavaScript += M.Kind == "javascript";
    Members.emplace_back(llvm::json::Object{
        {"member_id", M.ID},
        {"member_index", I},
        {"virtual_key", "/" + M.Path},
        {"kind", M.Kind},
        {"row_original_offset", std::to_string(M.RowOffset)},
        {"name_original_offset", std::to_string(M.NameOffset)},
        {"name_bytes", M.Path.size() + 1},
        {"stored_file", StoredFile},
        {"decoded_file", DecodedFile},
        {"stored_original_offset", std::to_string(M.Offset)},
        {"stored_bytes", std::to_string(M.Stored.size())},
        {"stored_sha256", M.StoredHash},
        {"decoded_bytes", std::to_string(M.Content.size())},
        {"decoded_sha256", M.BlobHash}});
  }
  for (const auto &F : E.Initializer.Fields)
    Fields.emplace_back(llvm::json::Object{
        {"instruction_address", std::to_string(F.Instruction)},
        {"value", std::to_string(F.Value)},
        {"exact_address_role", F.ExactAddress}});
  llvm::json::Object Summary{
      {"schema_version", 1},
      {"status", "ok"},
      {"export_profile", "tauri-local-evidence-export-v1"},
      {"layout_profile", E.Profile},
      {"extraction_id", E.ID},
      {"original_sha256", Original->BlobHash},
      {"original_bytes", std::to_string(Original->Content.size())},
      {"original_origin", llvm::json::Object(Original->Origin)},
      {"initializer_address", std::to_string(E.Initializer.Start)},
      {"initializer_end_address", std::to_string(E.Initializer.End)},
      {"initializer_original_offset", std::to_string(E.Initializer.Offset)},
      {"displacements_original_offset", std::to_string(E.DisplacementsOffset)},
      {"entries_original_offset", std::to_string(E.EntriesOffset)},
      {"member_count", E.Members.size()},
      {"javascript_file_count", JavaScript},
      {"stored_bytes", std::to_string(E.StoredBytes)},
      {"decoded_bytes", std::to_string(E.DecodedBytes)},
      {"phf_lookup_status", "all_keys_match_rows"},
      {"asset_server_binding", "not_checked"},
      {"producer_version_verified", false},
      {"executes_input", false},
      {"verification", "all_exported_files_reread_sha256_matches"}};
  auto Manifest = Summary;
  Manifest["original_file"] = "original.bin";
  Manifest["virtual_keys_are_host_paths"] = false;
  Manifest["initializer_fields"] = std::move(Fields);
  Manifest["members"] = std::move(Members);
  const auto Text = json(std::move(Manifest));
  Output.complete(Text);
  Summary["manifest_sha256"] = sha256(Text);
  Summary["file_count"] = Output.files();
  Summary["bytes_written"] = std::to_string(Output.bytes());
  return json(std::move(Summary));
}
} // namespace neverd::web
