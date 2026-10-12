//===- SessionWailsExport.cpp - Verified local Wails export ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit raw evidence disclosure through the shared exclusive writer.
/// Private embedded paths become manifest data, never host path components.
///
//===----------------------------------------------------------------------===//

#include "ExportDirectory.h"
#include "SessionInternal.h"

namespace neverd::web {
namespace {
std::string filename(uint64_t Index, std::string_view Kind) {
  auto Number = std::to_string(Index);
  Number.insert(0, 5 - std::min<size_t>(5, Number.size()), '0');
  const auto *Extension = Kind == "javascript" ? ".js"
                          : Kind == "html"     ? ".html"
                          : Kind == "css"      ? ".css"
                                               : ".bin";
  return "m" + Number + Extension;
}
} // namespace
std::string Session::exportWails(std::string_view Revision,
                                 std::string_view ExtractionID,
                                 std::string_view OutputDirectory) {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Found = State->WailsExtractions.find(std::string(ExtractionID));
  if (Found == State->WailsExtractions.end())
    throw Error("unknown_wails_extraction");
  const auto &E = Found->second;
  const auto Original = State->artifactView(E.ArtifactID);
  if (!Original)
    throw Error("unknown_artifact");
  ExportDirectory Output(OutputDirectory);
  Output.write("original.bin", Original->Content, Original->BlobHash);
  llvm::json::Array Tables, Members;
  for (uint64_t I = 0; I < E.Tables.size(); ++I) {
    const auto &T = E.Tables[I];
    Tables.emplace_back(
        llvm::json::Object{{"table_id", T.ID},
                           {"table_index", I},
                           {"original_offset", std::to_string(T.Offset)},
                           {"bytes", std::to_string(T.Size)},
                           {"first_member_index", T.FirstMember},
                           {"member_count", T.MemberCount}});
  }
  uint64_t Files = 0, Directories = 0, JavaScript = 0;
  for (uint64_t I = 0; I < E.Members.size(); ++I) {
    const auto &M = E.Members[I];
    const auto &T = E.Tables.at(M.Table);
    llvm::json::Object Row{
        {"member_id", M.ID},
        {"member_index", I},
        {"table_id", M.TableID},
        {"kind", M.Kind},
        {"virtual_path", M.Path},
        {"directory", M.Directory},
        {"record_original_offset",
         std::to_string(T.Offset + 24 + (I - T.FirstMember) * 48)},
        {"name_original_offset", std::to_string(M.NameOffset)},
        {"name_bytes", M.Path.size() + (M.Directory ? 1 : 0)}};
    if (M.Directory) {
      ++Directories;
      Row["content_status"] = "directory_no_payload";
    } else {
      const auto File = filename(I, M.Kind);
      Output.write(File, M.Content, M.BlobHash);
      Row["file"] = File;
      Row["original_offset"] = std::to_string(M.Offset);
      Row["bytes"] = std::to_string(M.Content.size());
      Row["sha256"] = M.BlobHash;
      Row["content_status"] = "stored_bytes_exact";
      Row["decoding"] = "none";
      Row["source_analysis"] = "not_requested";
      ++Files;
      JavaScript += M.Kind == "javascript";
    }
    Members.emplace_back(std::move(Row));
  }
  llvm::json::Object Summary{
      {"schema_version", 1},
      {"status", "ok"},
      {"export_profile", "wails-local-evidence-export-v1"},
      {"layout_profile", std::string(WailsProfile)},
      {"container_format", "elf"},
      {"platform", "linux"},
      {"architecture", "x64"},
      {"extraction_id", E.ID},
      {"original_sha256", Original->BlobHash},
      {"original_bytes", std::to_string(Original->Content.size())},
      {"original_origin", llvm::json::Object(Original->Origin)},
      {"table_count", E.Tables.size()},
      {"member_count", E.Members.size()},
      {"asset_file_count", Files},
      {"directory_count", Directories},
      {"javascript_file_count", JavaScript},
      {"payload_bytes", std::to_string(E.PayloadBytes)},
      {"table_coverage", "matching_structural_framing_only"},
      {"stored_digest_status", "go_toolchain_hash128_match"},
      {"asset_server_binding", "not_checked"},
      {"native_bridge_analysis", "not_analyzed"},
      {"producer_version_verified", false},
      {"executes_input", false},
      {"verification", "all_exported_files_reread_sha256_matches"}};
  auto Manifest = Summary;
  Manifest["original_file"] = "original.bin";
  Manifest["virtual_paths_are_host_paths"] = false;
  Manifest["tables"] = std::move(Tables);
  Manifest["members"] = std::move(Members);
  const auto Text = json(std::move(Manifest));
  Output.complete(Text);
  Summary["manifest_sha256"] = sha256(Text);
  Summary["file_count"] = Output.files();
  Summary["bytes_written"] = std::to_string(Output.bytes());
  return json(std::move(Summary));
}
} // namespace neverd::web
