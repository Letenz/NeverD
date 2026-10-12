//===- SessionReadableExport.cpp - Verified local source recovery ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Sequential readable-source export from the authoritative immutable byte
/// selection. Recovery models never enter interactive semantic caches.
///
//===----------------------------------------------------------------------===//

#include "ExportDirectory.h"
#include "RecoveryParser.h"
#include "SessionInternal.h"

#include "neverd/web/SourceRecovery.h"

#include <algorithm>

namespace neverd::web {
std::string Session::exportReadableSource(std::string_view Revision,
                                          std::string_view ArtifactID,
                                          std::string_view SourceType,
                                          std::string_view OutputDirectory) {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  if (SourceType != "script" && SourceType != "module" &&
      SourceType != "commonjs")
    throw Error("unsupported_source_type");
  const auto View = State->artifactView(ArtifactID);
  if (!View)
    throw Error("readable_source_selection_unavailable");
  if (View->Content.size() > MaxRecoverySourceBytes)
    throw Error("source_byte_budget_exceeded");
  // The export profile may materialize 32 MiB, while each immutable storage
  // read retains the same 64 KiB transfer bound as every other byte consumer.
  std::string Text;
  Text.reserve(View->Content.size());
  for (uint64_t At = 0; At < View->Content.size();) {
    const auto Count = std::min(BlobTransferBytes, View->Content.size() - At);
    Text += View->Content.read(At, Count);
    At += Count;
  }
  if (sha256(Text) != View->BlobHash)
    throw Error("readable_source_hash_mismatch");
  const auto Recovered =
      recoverReadableJavaScript(ArtifactID, Text, SourceType);
  const bool Verified = Recovered.Status == "verified_same_parser_tree";
  ExportDirectory Output(OutputDirectory);
  Output.write("original.bin", View->Content, View->BlobHash);
  if (Verified)
    Output.write("readable.js", Recovered.Text);
  llvm::json::Array Diagnostics;
  for (const auto &D : Recovered.Diagnostics)
    Diagnostics.emplace_back(llvm::json::Object{
        {"code", D.Code},
        {"original_byte_offset",
         D.ByteOffset < 0 ? llvm::json::Value(nullptr)
                          : llvm::json::Value(std::to_string(D.ByteOffset))}});
  llvm::json::Object Summary{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(State->Revision)},
      {"export_profile", std::string(SourceReadableExportProfile)},
      {"recovery_profile", std::string(JavaScriptRecoveryProfile)},
      {"artifact_id", std::string(ArtifactID)},
      {"source_type", std::string(SourceType)},
      {"source_type_basis", "explicit_caller_selection"},
      {"original_sha256", View->BlobHash},
      {"original_bytes", std::to_string(Text.size())},
      {"original_origin", llvm::json::Object(View->Origin)},
      {"original_range",
       llvm::json::Object{
           {"byte_offset_basis",
            View->DirectStorage ? "storage_artifact" : "expanded_stream"},
           {"byte_offset",
            View->DirectStorage
                ? llvm::json::Value(std::to_string(View->StorageOffset))
                : llvm::json::Value(nullptr)},
           {"expanded_byte_offset",
            !View->DirectStorage
                ? llvm::json::Value(std::to_string(View->StorageOffset))
                : llvm::json::Value(nullptr)},
           {"byte_length", std::to_string(Text.size())}}},
      {"parse_status", Recovered.ParseStatus.empty()
                           ? llvm::json::Value(nullptr)
                           : llvm::json::Value(Recovered.ParseStatus)},
      {"readable_status", Recovered.Status},
      {"syntax_nodes", Recovered.Nodes},
      {"parse_diagnostics", std::move(Diagnostics)},
      {"readable_sha256", Verified ? llvm::json::Value(sha256(Recovered.Text))
                                   : llvm::json::Value(nullptr)},
      {"readable_bytes",
       Verified ? llvm::json::Value(std::to_string(Recovered.Text.size()))
                : llvm::json::Value(nullptr)},
      {"transformation", "whitespace_insertion_only"},
      {"semantic_coverage", "not_established"},
      {"interactive_cache_modified", false},
      {"deleted_source_information", "not_recovered"},
      {"executes_input", false},
      {"verification", "all_exported_files_reread_sha256_matches"}};
  auto Manifest = Summary;
  Manifest["original_file"] = "original.bin";
  Manifest["readable_file"] =
      Verified ? llvm::json::Value("readable.js") : llvm::json::Value(nullptr);
  const auto ManifestText = json(std::move(Manifest));
  Output.complete(ManifestText);
  Summary["manifest_sha256"] = sha256(ManifestText);
  Summary["file_count"] = Output.files();
  Summary["bytes_written"] = std::to_string(Output.bytes());
  return json(std::move(Summary));
#endif
}
} // namespace neverd::web
