//===- NeverDCmdWebReadableExport.cpp - Explicit local source recovery
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Thin C API selection of a captured source and explicit parse goal.
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
  return OK ? std::optional<llvm::json::Value>(std::move(*V)) : std::nullopt;
}
std::string field(const llvm::json::Value &V, const char *Name) {
  return V.getAsObject()->getString(Name).value_or("").str();
}
} // namespace

int runWebReadableExport() {
  const auto Count = WebArguments.size();
  uint64_t Index = 0;
  auto Usage = [] {
    llvm::errs() << "usage: neverd web readable-export <root> "
                    "<script|module|commonjs> <new-output-directory> "
                    "[artifact-index]\n";
    return 2;
  };
  if (Count != 4 && Count != 5)
    return Usage();
  const auto &Path = WebArguments[1], &Type = WebArguments[2],
             &Directory = WebArguments[3];
  if ((Type != "script" && Type != "module" && Type != "commonjs") ||
      (Count == 5 &&
       (WebArguments[4].empty() ||
        llvm::StringRef(WebArguments[4]).getAsInteger(10, Index) ||
        std::to_string(Index) != WebArguments[4])))
    return Usage();
  struct Guard {
    neverd_web_session_t Web = neverd_web_session_create();
    ~Guard() { neverd_web_session_destroy(Web); }
  } G;
  if (!G.Web)
    return 1;
  const auto Preview = reply(neverd_web_import_preview_json(
      G.Web, Path.data(), Path.size(), nullptr, 0));
  if (!Preview)
    return 1;
  const auto Token = field(*Preview, "preview_token");
  const auto Capture =
      reply(neverd_web_import_commit_json(G.Web, Token.data(), Token.size()));
  if (!Capture)
    return 1;
  const auto Revision = field(*Capture, "revision");
  const auto Page = reply(neverd_web_artifacts_json(G.Web, Revision.data(),
                                                    Revision.size(), Index, 1));
  const auto *Items = Page ? Page->getAsObject()->getArray("items") : nullptr;
  if (!Items || Items->size() != 1)
    return 1;
  const auto Artifact = field((*Items)[0], "artifact_id");
  const auto Result =
      reply(neverd_web_source_readable_export_json(
                G.Web, Revision.data(), Revision.size(), Artifact.data(),
                Artifact.size(), Type.data(), Type.size(), Directory.data(),
                Directory.size()),
            true);
  return Result && field(*Result, "readable_status") ==
                       "verified_same_parser_tree"
             ? 0
             : 1;
}
} // namespace neverd::cli
