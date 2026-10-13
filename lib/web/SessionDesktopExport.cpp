//===- SessionDesktopExport.cpp - Explicit local resource export --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Supplied resource selections use fixed files and exact captured origins.
///
//===----------------------------------------------------------------------===//

#include "ExportDirectory.h"
#include "SessionInternal.h"

namespace neverd::web {
std::string Session::exportDesktopResources(std::string_view Revision,
                                            std::string_view ManifestID,
                                            std::string_view OutputDirectory) {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const DesktopManifest *Manifest = nullptr;
  for (const auto &[Key, A] : State->DesktopManifests)
    if (A.ID == ManifestID && A.Profile == DesktopResourceProfile)
      Manifest = &A;
  if (!Manifest)
    throw Error("unknown_desktop_resource_manifest");
  const auto Descriptor = State->artifactView(Manifest->ArtifactID);
  if (!Descriptor)
    throw Error("unknown_artifact");
  ExportDirectory Output(OutputDirectory);
  Output.write("descriptor.json", Descriptor->Content, Descriptor->BlobHash);
  llvm::json::Array Entries;
  uint64_t Exported = 0;
  for (uint64_t I = 0; I < Manifest->Entries.size(); ++I) {
    const auto &E = Manifest->Entries[I];
    llvm::json::Object R{{"entry_id", E.ID},
                         {"entry_index", I},
                         {"role", E.Field},
                         {"content_kind", E.ContentKind},
                         {"link_status", E.LinkStatus},
                         {"artifact_id", E.ArtifactID}};
    if (!E.ArtifactID.empty()) {
      const auto View = State->artifactView(E.ArtifactID);
      if (!View || View->BlobHash != E.BlobHash ||
          View->Content.size() != E.ByteLength)
        throw Error("desktop_resource_input_mismatch");
      const auto *Extension =
          E.ContentKind == "javascript_filename_candidate"   ? ".js"
          : E.ContentKind == "html_filename_candidate"       ? ".html"
          : E.ContentKind == "css_filename_candidate"        ? ".css"
          : E.ContentKind == "source_map_filename_candidate" ? ".map"
                                                             : ".bin";
      const auto Name = "resource" + std::to_string(I) + Extension;
      Output.write(Name, View->Content, View->BlobHash);
      R["file"] = Name;
      R["sha256"] = View->BlobHash;
      R["bytes"] = std::to_string(View->Content.size());
      R["origin"] = llvm::json::Object(View->Origin);
      R["source_recovery"] = "not_requested";
      ++Exported;
    } else
      R["file"] = nullptr;
    Entries.emplace_back(std::move(R));
  }
  llvm::json::Object Summary{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(State->Revision)},
      {"manifest_id", Manifest->ID},
      {"profile", Manifest->Profile},
      {"export_profile", "desktop-local-resource-export-v1"},
      {"descriptor_sha256", Descriptor->BlobHash},
      {"descriptor_origin", llvm::json::Object(Descriptor->Origin)},
      {"entry_count", Manifest->Entries.size()},
      {"exported_entries", Exported},
      {"unavailable_entries", Manifest->Entries.size() - Exported},
      {"framework_verified", false},
      {"runtime_version_verified", false},
      {"complete_application", false},
      {"executes_input", false},
      {"verification", "all_exported_files_reread_sha256_matches"}};
  auto Receipt = Summary;
  Receipt["entries"] = std::move(Entries);
  const auto Text = json(std::move(Receipt));
  Output.complete(Text);
  Summary["export_manifest_sha256"] = sha256(Text);
  Summary["file_count"] = Output.files();
  return json(std::move(Summary));
}
} // namespace neverd::web
