//===- NeverDCmdWebDataPack.cpp - Explicit offline resource selection ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Thin C API selection of original and explicitly decoded pack resources.
///
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"

#include "neverd/sdk/NeverDCAPIWeb.h"

#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd::cli {
namespace {
std::optional<llvm::json::Value> reply(const char *Owned, bool Print = false) {
  if (!Owned)
    return {};
  auto V = llvm::json::parse(Owned);
  neverd_free_string(Owned);
  if (!V) {
    llvm::consumeError(V.takeError());
    return {};
  }
  const bool OK =
      V->getAsObject() && V->getAsObject()->getString("status") == "ok";
  if (Print || !OK)
    llvm::outs() << *V << '\n';
  return OK ? std::optional(std::move(*V)) : std::nullopt;
}
std::string field(const llvm::json::Value &V, const char *Key) {
  return V.getAsObject()->getString(Key).value_or("").str();
}
std::string selected(const char *Owned, const char *Key) {
  const auto V = reply(Owned);
  const auto *Items = V ? V->getAsObject()->getArray("items") : nullptr;
  return Items && Items->size() == 1 ? field((*Items)[0], Key) : "";
}
bool decimal(llvm::StringRef S, uint64_t &N) {
  return !S.empty() && !S.getAsInteger(10, N) && std::to_string(N) == S;
}
} // namespace

int runWebDataPack() {
  const auto &A = WebArguments;
  const bool Archive = A[0].starts_with("archive-");
  const bool Source = A[0].ends_with("-source");
  const bool HTML = A[0].ends_with("-html");
  const bool Export = A[0].ends_with("-export");
  const size_t Base = Archive ? 6 : 4;
  auto Usage = [] {
    llvm::errs()
        << "usage: neverd web pak <root> <profile> [artifact-index] | "
           "pak-source <root> <profile> <artifact-index> <resource-index> "
           "<raw|gzip> <script|module|commonjs> | "
           "pak-html <root> <profile> <artifact-index> <resource-index> "
           "<raw|gzip> | pak-export <root> <profile> <artifact-index> "
           "<new-output-directory>\n"
           "Archive selections use archive-pak[-source|-html|-export] "
           "<root> <profile> <tar|tgz|zip> <archive-index> "
           "<archive-member-index>, followed by the same resource/export "
           "arguments.\n";
    return 2;
  };
  if (Source   ? A.size() != Base + 3
      : HTML   ? A.size() != Base + 2
      : Export ? A.size() != Base + 1
               : A.size() != Base && (Archive || A.size() != 3))
    return Usage();
  uint64_t Index = 0, ArchiveMember = 0, ResourceIndex = 0;
  if ((A.size() >= Base && !decimal(A[Archive ? 4 : 3], Index)) ||
      (Archive && !decimal(A[5], ArchiveMember)) ||
      ((Source || HTML) && !decimal(A[Base], ResourceIndex)))
    return Usage();
  const auto Format = Archive ? A[3] : "";
  const auto Codec = Source || HTML ? A[Base + 1] : "";
  const auto Type = Source ? A[Base + 2] : "";
  if ((Archive && Format != "tar" && Format != "tgz" && Format != "zip") ||
      ((Source || HTML) && Codec != "raw" && Codec != "gzip") ||
      (Source && Type != "script" && Type != "module" && Type != "commonjs"))
    return Usage();
  struct Guard {
    neverd_web_session_t S = neverd_web_session_create();
    ~Guard() { neverd_web_session_destroy(S); }
  } G;
  if (!G.S)
    return 1;
  const auto Preview = reply(neverd_web_import_preview_json(
      G.S, A[1].data(), A[1].size(), nullptr, 0));
  if (!Preview)
    return 1;
  const auto Token = field(*Preview, "preview_token");
  const auto Commit =
      reply(neverd_web_import_commit_json(G.S, Token.data(), Token.size()));
  if (!Commit)
    return 1;
  const auto Revision = field(*Commit, "revision");
  auto Artifact = selected(neverd_web_artifacts_json(G.S, Revision.data(),
                                                     Revision.size(), Index, 1),
                           "artifact_id");
  if (Artifact.empty())
    return 1;
  if (Archive) {
    const auto Container = reply(neverd_web_package_archive_extract_json(
        G.S, Revision.data(), Revision.size(), Artifact.data(), Artifact.size(),
        Format.data(), Format.size()));
    if (!Container)
      return 1;
    const auto ID = field(*Container, "archive_id");
    Artifact = selected(neverd_web_package_archive_records_json(
                            G.S, Revision.data(), Revision.size(), ID.data(),
                            ID.size(), ArchiveMember, 1),
                        "member_id");
    if (Artifact.empty())
      return 1;
  }
  const auto E =
      reply(neverd_web_datapack_extract_json(
                G.S, Revision.data(), Revision.size(), Artifact.data(),
                Artifact.size(), A[2].data(), A[2].size()),
            true);
  if (!E)
    return 1;
  const auto ID = field(*E, "extraction_id");
  if (Export)
    return reply(neverd_web_datapack_export_json(
                     G.S, Revision.data(), Revision.size(), ID.data(),
                     ID.size(), A[Base].data(), A[Base].size()),
                 true)
               ? 0
               : 1;
  if (Source || HTML) {
    auto Selection = selected(neverd_web_datapack_records_json(
                                  G.S, Revision.data(), Revision.size(),
                                  ID.data(), ID.size(), ResourceIndex, 1),
                              "selection_id");
    if (Selection.empty())
      return 1;
    if (Codec == "gzip") {
      constexpr std::string_view Gzip = "gzip-rfc1952-v1";
      const auto D =
          reply(neverd_web_datapack_decode_json(
                    G.S, Revision.data(), Revision.size(), Selection.data(),
                    Selection.size(), Gzip.data(), Gzip.size()),
                true);
      if (!D)
        return 1;
      Selection = field(*D, "selection_id");
    }
    if (HTML)
      return reply(neverd_web_html_analyze_json(
                       G.S, Revision.data(), Revision.size(), Selection.data(),
                       Selection.size()),
                   true)
                 ? 0
                 : 1;
    const auto S =
        reply(neverd_web_source_analyze_large_json(
                  G.S, Revision.data(), Revision.size(), Selection.data(),
                  Selection.size(), Type.data(), Type.size()),
              true);
    return S && field(*S, "parse_status") == "parsed" ? 0 : 1;
  }
  const auto Count = E->getAsObject()->getInteger("record_count").value_or(0);
  for (uint64_t At = 0; At < uint64_t(Count); At += 128)
    if (!reply(neverd_web_datapack_records_json(G.S, Revision.data(),
                                                Revision.size(), ID.data(),
                                                ID.size(), At, 128),
               true))
      return 1;
  return 0;
}
} // namespace neverd::cli
