//===- NeverDCmdWebBehaviorDiff.cpp - Captured source behavior comparison ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Compose explicit before/after selections through the shared C SDK.
///
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"
#include "../ResourceLimits.h"

#include "neverd/sdk/NeverDCAPIWeb.h"

#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <array>

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
bool decimal(llvm::StringRef Text, uint64_t &N) {
  return !Text.empty() && !Text.getAsInteger(10, N) &&
         std::to_string(N) == Text;
}
} // namespace

int runWebBehaviorDiff() {
  const bool Archive = WebArguments[0] == "archive-package-behavior-diff";
  const size_t FirstSource = Archive ? 8 : 5;
  auto Usage = [] {
    llvm::errs()
        << "usage: neverd web package-behavior-diff <root> "
           "<before-manifest-index> <after-manifest-index> "
           "<npm-lock|package-json> [before:index:type|after:index:type]...\n"
           "neverd web archive-package-behavior-diff <root> <tar|tgz|zip> "
           "<before-archive-index> <before-manifest-member-index> "
           "<after-archive-index> <after-manifest-member-index> "
           "<npm-lock|package-json> [before:member:type|after:member:type]...\n"
           "type: script, module, or commonjs; at most 16 sources total\n";
    return 2;
  };
  if (WebArguments.size() < FirstSource ||
      WebArguments.size() > FirstSource + 16)
    return Usage();
  const std::string Kind = WebArguments[FirstSource - 1];
  const std::string Format = Archive ? WebArguments[2] : "";
  if ((Kind != "package-json" && Kind != "npm-lock") ||
      (Archive && Format != "tar" && Format != "tgz" && Format != "zip"))
    return Usage();
  struct Side {
    uint64_t ArchiveIndex = 0, ManifestIndex = 0;
    std::string ArchiveID, PackageID;
    std::vector<std::pair<uint64_t, std::string>> Sources;
  };
  std::array<Side, 2> Sides;
  if (!decimal(WebArguments[Archive ? 4 : 2], Sides[0].ManifestIndex) ||
      !decimal(WebArguments[Archive ? 6 : 3], Sides[1].ManifestIndex) ||
      (Archive && (!decimal(WebArguments[3], Sides[0].ArchiveIndex) ||
                   !decimal(WebArguments[5], Sides[1].ArchiveIndex))))
    return Usage();
  for (size_t I = FirstSource; I < WebArguments.size(); ++I) {
    const auto [SideName, Source] = llvm::StringRef(WebArguments[I]).split(':');
    const auto [Index, Type] = Source.split(':');
    uint64_t N = 0;
    if ((SideName != "before" && SideName != "after") || !decimal(Index, N) ||
        (Type != "script" && Type != "module" && Type != "commonjs"))
      return Usage();
    Sides[SideName == "after"].Sources.emplace_back(N, Type.str());
  }
  struct Guard {
    neverd_web_session_t S = neverd_web_session_create();
    ~Guard() { neverd_web_session_destroy(S); }
  } G;
  if (!G.S)
    return 1;
  const auto &Path = WebArguments[1];
  const auto Preview = reply(neverd_web_import_preview_json(
      G.S, Path.data(), Path.size(), nullptr, 0));
  if (!Preview)
    return 1;
  const auto Token = field(*Preview, "preview_token");
  const auto Capture =
      reply(neverd_web_import_commit_json(G.S, Token.data(), Token.size()));
  if (!Capture)
    return 1;
  const auto Rev = field(*Capture, "revision");
  auto Artifact = [&](uint64_t Index) {
    return selected(
        neverd_web_artifacts_json(G.S, Rev.data(), Rev.size(), Index, 1),
        "artifact_id");
  };
  llvm::json::Object Options{{"schema_version", 1}};
  for (unsigned I = 0; I < Sides.size(); ++I) {
    auto &S = Sides[I];
    if (Archive) {
      const auto A = Artifact(S.ArchiveIndex);
      if (A.empty())
        return 1;
      const auto E = reply(neverd_web_package_archive_extract_json(
          G.S, Rev.data(), Rev.size(), A.data(), A.size(), Format.data(),
          Format.size()));
      if (!E)
        return 1;
      S.ArchiveID = field(*E, "archive_id");
    }
    auto Member = [&](uint64_t Index) {
      return Archive
                 ? selected(neverd_web_package_archive_records_json(
                                G.S, Rev.data(), Rev.size(), S.ArchiveID.data(),
                                S.ArchiveID.size(), Index, 1),
                            "member_id")
                 : Artifact(Index);
    };
    const auto Manifest = Member(S.ManifestIndex);
    if (Manifest.empty())
      return 1;
    const auto P = reply(neverd_web_packages_analyze_json(
        G.S, Rev.data(), Rev.size(), Manifest.data(), Manifest.size(),
        Kind.data(), Kind.size()));
    if (!P)
      return 1;
    S.PackageID = field(*P, "package_analysis_id");
    if (S.Sources.empty())
      continue;
    llvm::json::Array IDs;
    for (const auto &[Index, Type] : S.Sources) {
      const auto A = Member(Index);
      if (A.empty())
        return 1;
      const auto Source = reply(
          neverd_web_source_analyze_json(G.S, Rev.data(), Rev.size(), A.data(),
                                         A.size(), Type.data(), Type.size()));
      if (!Source)
        return 1;
      IDs.push_back(field(*Source, "source_id"));
    }
    const auto Selection =
        llvm::formatv(
            "{0}", llvm::json::Value(llvm::json::Object{
                       {"schema_version", 1}, {"source_ids", std::move(IDs)}}))
            .str();
    const auto B = reply(neverd_web_package_behavior_analyze_json(
        G.S, Rev.data(), Rev.size(), S.PackageID.data(), S.PackageID.size(),
        Selection.data(), Selection.size()));
    if (!B)
      return 1;
    Options[I ? "after_package_behavior_id" : "before_package_behavior_id"] =
        field(*B, "package_behavior_id");
  }
  const auto Base = reply(neverd_web_packages_compare_json(
      G.S, Rev.data(), Rev.size(), Sides[0].PackageID.data(),
      Sides[0].PackageID.size(), Sides[1].PackageID.data(),
      Sides[1].PackageID.size()));
  if (!Base)
    return 1;
  const auto BaseID = field(*Base, "package_diff_id");
  const auto Encoded =
      llvm::formatv("{0}", llvm::json::Value(std::move(Options))).str();
  const auto D = reply(neverd_web_package_behavior_diff_analyze_json(
                           G.S, Rev.data(), Rev.size(), BaseID.data(),
                           BaseID.size(), Encoded.data(), Encoded.size()),
                       true);
  if (!D)
    return 1;
  const auto ID = field(*D, "package_behavior_diff_id");
  for (const std::string Kind : {"pairs", "patterns", "occurrences"}) {
    uint64_t Offset = 0;
    for (;;) {
      const auto Page =
          reply(neverd_web_package_behavior_diff_records_json(
                    G.S, Rev.data(), Rev.size(), ID.data(), ID.size(),
                    Kind.data(), Kind.size(), Offset, 128),
                true);
      if (!Page)
        return 1;
      const auto Next = Page->getAsObject()->getInteger("next_offset");
      if (!Next)
        break;
      if (*Next <= 0 || uint64_t(*Next) <= Offset ||
          *Next > ::neverd::limits::cli::kMaxWebBehaviorDiffOffset)
        return 1;
      Offset = *Next;
    }
  }
  return 0;
}
} // namespace neverd::cli
