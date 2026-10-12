//===- NeverDCmdWebTauri.cpp - Explicit offline native asset selection
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Thin shared-SDK CLI for initializer evidence, immutable assets and exports.
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
std::string field(const llvm::json::Value &V, const char *Name) {
  return V.getAsObject()->getString(Name).value_or("").str();
}
std::string selected(const char *Owned, const char *Key) {
  auto V = reply(Owned);
  const auto *A = V ? V->getAsObject()->getArray("items") : nullptr;
  return A && A->size() == 1 ? field((*A)[0], Key) : "";
}
} // namespace
int runWebTauri() {
  const auto &A = WebArguments;
  const bool Export = A[0] == "tauri-export";
  const bool Readable = A[0] == "tauri-readable-export";
  const bool Source = A[0] == "tauri-source" || Readable;
  const bool HTML = A[0] == "tauri-html";
  auto Usage = [] {
    llvm::errs()
        << "usage: neverd web tauri <root> <profile> <initializer-address> "
           "[artifact-index] | "
           "tauri-export <root> <profile> <initializer-address> "
           "<new-output-directory> [artifact-index] | "
           "tauri-html <root> <profile> <initializer-address> <artifact-index> "
           "<member-index> | "
           "tauri-source <root> <profile> <initializer-address> "
           "<artifact-index> <member-index> <script|module|commonjs> | "
           "tauri-readable-export <root> <profile> <initializer-address> "
           "<artifact-index> <member-index> <script|module|commonjs> "
           "<new-output-directory>\n";
    return 2;
  };
  if ((Export && A.size() != 5 && A.size() != 6) ||
      (Source && A.size() != (Readable ? 8u : 7u)) || (HTML && A.size() != 6) ||
      (!Export && !Source && !HTML && A.size() != 4 && A.size() != 5))
    return Usage();
  uint64_t Start = 0, Index = 0, Member = 0;
  if (llvm::StringRef(A[3]).getAsInteger(0, Start))
    return Usage();
  const auto Position = Export ? 5u : 4u;
  if (A.size() > Position &&
      llvm::StringRef(A[Position]).getAsInteger(10, Index))
    return Usage();
  if ((Source || HTML) && llvm::StringRef(A[5]).getAsInteger(10, Member))
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
  const auto Artifact =
      selected(neverd_web_artifacts_json(G.S, Revision.data(), Revision.size(),
                                         Index, 1),
               "artifact_id");
  if (Artifact.empty())
    return 1;
  const auto E =
      reply(neverd_web_tauri_extract_json(G.S, Revision.data(), Revision.size(),
                                          Artifact.data(), Artifact.size(),
                                          Start, A[2].data(), A[2].size()),
            true);
  if (!E)
    return 1;
  const auto ID = field(*E, "extraction_id");
  if (Export)
    return reply(neverd_web_tauri_export_json(
                     G.S, Revision.data(), Revision.size(), ID.data(),
                     ID.size(), A[4].data(), A[4].size()),
                 true)
               ? 0
               : 1;
  if (Source || HTML) {
    const auto Selection = selected(
        neverd_web_tauri_records_json(G.S, Revision.data(), Revision.size(),
                                      ID.data(), ID.size(), Member, 1),
        "selection_id");
    if (Selection.empty())
      return 1;
    if (HTML)
      return reply(neverd_web_html_analyze_json(
                       G.S, Revision.data(), Revision.size(), Selection.data(),
                       Selection.size()),
                   true)
                 ? 0
                 : 1;
    if (Readable)
      return reply(neverd_web_source_readable_export_json(
                       G.S, Revision.data(), Revision.size(), Selection.data(),
                       Selection.size(), A[6].data(), A[6].size(), A[7].data(),
                       A[7].size()),
                   true)
                 ? 0
                 : 1;
    return reply(neverd_web_source_analyze_json(
                     G.S, Revision.data(), Revision.size(), Selection.data(),
                     Selection.size(), A[6].data(), A[6].size()),
                 true)
               ? 0
               : 1;
  }
  const auto Count = E->getAsObject()->getInteger("member_count").value_or(0);
  for (uint64_t At = 0; At < uint64_t(Count); At += 128)
    if (!reply(neverd_web_tauri_records_json(G.S, Revision.data(),
                                             Revision.size(), ID.data(),
                                             ID.size(), At, 128),
               true))
      return 1;
  return 0;
}
} // namespace neverd::cli
