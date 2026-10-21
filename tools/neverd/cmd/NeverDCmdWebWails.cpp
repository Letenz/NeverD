//===- NeverDCmdWebWails.cpp - Offline Wails CLI selection ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Thin C API selection of captured native containers and stored resources.
///
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"
#include "../ResourceLimits.h"

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
  if (!OK)
    return {};
  return std::move(*V);
}
std::string field(const llvm::json::Value &V, const char *Name) {
  return V.getAsObject()->getString(Name).value_or("").str();
}
std::string selected(const char *Owned, const char *Key) {
  auto V = reply(Owned);
  const auto *Items = V ? V->getAsObject()->getArray("items") : nullptr;
  return Items && Items->size() == 1 ? field((*Items)[0], Key) : "";
}
bool decimal(llvm::StringRef S, uint64_t &N) {
  return !S.empty() && !S.getAsInteger(10, N) && std::to_string(N) == S;
}
} // namespace

int runWebWails() {
  const auto &Command = WebArguments[0];
  const bool Readable = Command == "wails-readable-export";
  const bool Source = Command == "wails-source" || Readable,
             HTML = Command == "wails-html";
  const bool Native = Command == "wails-native";
  const bool Export = Command == "wails-export";
  auto Usage = [] {
    llvm::errs()
        << "usage: neverd web wails|wails-native <root> <profile> "
           "[artifact-index] | wails-export <root> <profile> "
           "<new-output-directory> [artifact-index] | wails-html <root> "
           "<profile> <artifact-index> <member-index> | wails-source <root> "
           "<profile> <artifact-index> <member-index> "
           "<script|module|commonjs> | wails-readable-export <root> "
           "<profile> <artifact-index> <member-index> "
           "<script|module|commonjs> <new-output-directory>\n";
    return 2;
  };
  const auto Count = WebArguments.size();
  if ((Source && Count != (Readable ? 7U : 6U)) || (HTML && Count != 5U) ||
      (Export && Count != 4U && Count != 5U) ||
      (!Source && !HTML && !Export && Count != 3U && Count != 4U))
    return Usage();
  uint64_t ArtifactIndex = 0, MemberIndex = 0;
  const auto ArtifactArgument = Export ? 4U : 3U;
  if ((Count > ArtifactArgument &&
       !decimal(WebArguments[ArtifactArgument], ArtifactIndex)) ||
      ((Source || HTML) && !decimal(WebArguments[4], MemberIndex)))
    return Usage();
  const auto Type = Source ? WebArguments[5] : "";
  if (Source && Type != "script" && Type != "module" && Type != "commonjs")
    return Usage();
  struct Guard {
    neverd_web_session_t Web = neverd_web_session_create();
    neverd_session_t Native = nullptr;
    ~Guard() {
      neverd_session_destroy(Native);
      neverd_web_session_destroy(Web);
    }
  } G;
  if (!G.Web)
    return 1;
  const auto &Path = WebArguments[1], &Profile = WebArguments[2];
  const auto Preview = reply(neverd_web_import_preview_json(
      G.Web, Path.data(), Path.size(), nullptr, 0));
  if (!Preview)
    return 1;
  const auto Token = field(*Preview, "preview_token");
  const auto Capture =
      reply(neverd_web_import_commit_json(G.Web, Token.data(), Token.size()));
  if (!Capture)
    return 1;
  const auto Rev = field(*Capture, "revision");
  const auto Artifact =
      selected(neverd_web_artifacts_json(G.Web, Rev.data(), Rev.size(),
                                         ArtifactIndex, 1),
               "artifact_id");
  if (Artifact.empty())
    return 1;
  const auto E = reply(neverd_web_wails_extract_json(
                           G.Web, Rev.data(), Rev.size(), Artifact.data(),
                           Artifact.size(), Profile.data(), Profile.size()),
                       true);
  if (!E)
    return 1;
  const auto ID = field(*E, "extraction_id");
  if (Export) {
    const auto &Directory = WebArguments[3];
    return reply(neverd_web_wails_export_json(
                     G.Web, Rev.data(), Rev.size(), ID.data(), ID.size(),
                     Directory.data(), Directory.size()),
                 true)
               ? 0
               : 1;
  }
  if (Native) {
    if (!reply(neverd_web_native_open_json(G.Web, Rev.data(), Rev.size(),
                                           Artifact.data(), Artifact.size(),
                                           &G.Native),
               true))
      return 1;
    return reply(neverd_web_native_metadata_json(G.Native), true) ? 0 : 1;
  }
  if (Source || HTML) {
    const auto Selection = selected(
        neverd_web_wails_records_json(G.Web, Rev.data(), Rev.size(), ID.data(),
                                      ID.size(), "members", 7, MemberIndex, 1),
        "selection_id");
    if (Selection.empty())
      return 1;
    if (Readable) {
      const auto &Directory = WebArguments[6];
      const auto R = reply(neverd_web_source_readable_export_json(
                               G.Web, Rev.data(), Rev.size(), Selection.data(),
                               Selection.size(), Type.data(), Type.size(),
                               Directory.data(), Directory.size()),
                           true);
      return R && field(*R, "readable_status") == "verified_same_parser_tree"
                 ? 0
                 : 1;
    }
    if (HTML)
      return reply(neverd_web_html_analyze_json(G.Web, Rev.data(), Rev.size(),
                                                Selection.data(),
                                                Selection.size()),
                   true)
                 ? 0
                 : 1;
    const auto S = reply(neverd_web_source_analyze_json(
                             G.Web, Rev.data(), Rev.size(), Selection.data(),
                             Selection.size(), Type.data(), Type.size()),
                         true);
    return S && field(*S, "parse_status") == "parsed" ? 0 : 1;
  }
  for (const std::string Kind : {"tables", "members"}) {
    for (uint64_t Offset = 0;;) {
      const auto Page =
          reply(neverd_web_wails_records_json(G.Web, Rev.data(), Rev.size(),
                                              ID.data(), ID.size(), Kind.data(),
                                              Kind.size(), Offset, 128),
                true);
      if (!Page)
        return 1;
      const auto Next = Page->getAsObject()->getInteger("next_offset");
      if (!Next)
        break;
      if (*Next < 0 || uint64_t(*Next) <= Offset ||
          *Next > ::neverd::limits::cli::kMaxWebWailsOffset)
        return 1;
      Offset = *Next;
    }
  }
  return 0;
}
} // namespace neverd::cli
