//===- NeverDCmdWebRewrite.cpp - Source rewrite evidence CLI -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit passes and receipt-bound local disclosure through the shared API.
///
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"

#include "neverd/sdk/NeverDCAPIWeb.h"

#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd::cli {
namespace {
std::optional<llvm::json::Value> reply(const char *Owned) {
  if (!Owned) {
    llvm::errs() << "web: allocation_failed\n";
    return {};
  }
  auto R = llvm::json::parse(Owned);
  neverd_free_string(Owned);
  if (!R) {
    llvm::consumeError(R.takeError());
    llvm::errs() << "web: invalid_backend_response\n";
    return {};
  }
  if (!R->getAsObject() || R->getAsObject()->getString("status") != "ok") {
    llvm::outs() << *R << '\n';
    return {};
  }
  return std::move(*R);
}
std::string field(const llvm::json::Value &V, const char *Name) {
  return V.getAsObject()->getString(Name).value_or("").str();
}
} // namespace

int runWebRewrite() {
  const auto &Command = WebArguments[0];
  const bool Bun = Command == "bun-rewrite" || Command == "bun-rewrite-export";
  const bool Export =
      Command == "rewrite-export" || Command == "bun-rewrite-export";
  const auto Count = WebArguments.size();
  if ((Export && Count != 7) || (!Export && Bun && Count != 5) ||
      (!Export && !Bun && Count != 4 && Count != 5)) {
    llvm::errs()
        << "usage: neverd web rewrite <root> <source-type> <pass> "
           "[artifact-index]\n"
           "       neverd web rewrite-export <root> <source-type> <pass> "
           "<artifact-index> <preview-receipt> <output-dir>\n"
           "       neverd web bun-rewrite <binary> <module-index> "
           "<source-type> <pass>\n"
           "       neverd web bun-rewrite-export <binary> <module-index> "
           "<source-type> "
           "<pass> <preview-receipt> <output-dir>\n"
           "pass: string_recovery, constant_folding or scope_renaming\n";
    return 2;
  }
  const auto &Type = WebArguments[Bun ? 3 : 2];
  const auto &Pass = WebArguments[Bun ? 4 : 3];
  if ((Type != "script" && Type != "module" && Type != "commonjs") ||
      (Pass != "string_recovery" && Pass != "constant_folding" &&
       Pass != "scope_renaming")) {
    llvm::errs() << "web: invalid_source_rewrite_selection\n";
    return 2;
  }
  uint64_t Index = 0;
  if (Bun || Count >= 5) {
    const llvm::StringRef Text = WebArguments[Bun ? 2 : 4];
    if (Text.empty() || Text.getAsInteger(10, Index) ||
        std::to_string(Index) != Text) {
      llvm::errs() << "web: invalid_source_rewrite_index\n";
      return 2;
    }
  }
  struct Guard {
    neverd_web_session_t S = neverd_web_session_create();
    ~Guard() { neverd_web_session_destroy(S); }
  } G;
  if (!G.S) {
    reply(neverd_web_capabilities_json());
    return 1;
  }
  const auto &Path = WebArguments[1];
  const auto P = reply(neverd_web_import_preview_json(G.S, Path.data(),
                                                      Path.size(), nullptr, 0));
  if (!P)
    return 1;
  const auto Token = field(*P, "preview_token");
  const auto M =
      reply(neverd_web_import_commit_json(G.S, Token.data(), Token.size()));
  if (!M)
    return 1;
  const auto Revision = field(*M, "revision");
  const auto Artifacts = reply(neverd_web_artifacts_json(
      G.S, Revision.data(), Revision.size(), Bun ? 0 : Index, 1));
  if (!Artifacts)
    return 1;
  const auto *Items = Artifacts->getAsObject()->getArray("items");
  if (!Items || Items->size() != 1)
    return 1;
  auto Artifact = field((*Items)[0], "artifact_id");
  if (Bun) {
    const auto Extraction =
        reply(neverd_web_bun_extract_json(G.S, Revision.data(), Revision.size(),
                                          Artifact.data(), Artifact.size()));
    if (!Extraction)
      return 1;
    const auto ID = field(*Extraction, "extraction_id");
    const auto Modules = reply(neverd_web_bun_records_json(
        G.S, Revision.data(), Revision.size(), ID.data(), ID.size(), "modules",
        7, Index, 1));
    if (!Modules)
      return 1;
    const auto *Rows = Modules->getAsObject()->getArray("items");
    if (!Rows || Rows->size() != 1)
      return 1;
    Artifact = field((*Rows)[0], "source_artifact_id");
    if (Artifact.empty()) {
      llvm::errs() << "web: bun_source_unavailable\n";
      return 1;
    }
  }
  const auto Source = reply(neverd_web_source_analyze_json(
      G.S, Revision.data(), Revision.size(), Artifact.data(), Artifact.size(),
      Type.data(), Type.size()));
  if (!Source)
    return 1;
  const auto SourceID = field(*Source, "source_id");
  const auto Analysis = reply(neverd_web_source_rewrite_analyze_json(
      G.S, Revision.data(), Revision.size(), SourceID.data(), SourceID.size(),
      Pass.data(), Pass.size()));
  if (!Analysis)
    return 1;
  llvm::outs() << *Analysis << '\n';
  const auto Status = field(*Analysis, "analysis_status");
  if (Status != "verified_local_rewrites" && Status != "no_eligible_rewrites")
    return 1;
  const auto ID = field(*Analysis, "rewrite_id");
  if (Export) {
    const auto &Receipt = WebArguments[5], &Output = WebArguments[6];
    const auto Written = reply(neverd_web_source_rewrite_export_json(
        G.S, Revision.data(), Revision.size(), ID.data(), ID.size(),
        Receipt.data(), Receipt.size(), Output.data(), Output.size()));
    if (!Written)
      return 1;
    llvm::outs() << *Written << '\n';
    return 0;
  }
  uint64_t Offset = 0;
  do {
    const auto Page = reply(neverd_web_source_rewrite_records_json(
        G.S, Revision.data(), Revision.size(), ID.data(), ID.size(), Offset,
        128));
    if (!Page)
      return 1;
    llvm::outs() << *Page << '\n';
    const auto Next = Page->getAsObject()->getInteger("next_offset");
    if (!Next)
      break;
    Offset = *Next;
  } while (true);
  return 0;
}

} // namespace neverd::cli
