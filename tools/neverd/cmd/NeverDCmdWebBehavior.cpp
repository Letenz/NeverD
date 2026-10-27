//===- NeverDCmdWebBehavior.cpp - Offline Node behavior commands ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Select source and package evidence through the common C API.
///
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"
#include "../ResourceLimits.h"

#include "neverd/sdk/NeverDCAPIWeb.h"

#include "llvm/Support/FormatVariadic.h"
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
std::string field(const llvm::json::Value &V, const char *Key) {
  return V.getAsObject()->getString(Key).value_or("").str();
}
bool decimal(llvm::StringRef Text, uint64_t &N) {
  return !Text.empty() && !Text.getAsInteger(10, N) &&
         std::to_string(N) == Text;
}
std::string selected(const char *Owned, const char *Key) {
  auto V = reply(Owned);
  const auto *Items = V ? V->getAsObject()->getArray("items") : nullptr;
  return Items && Items->size() == 1 ? field((*Items)[0], Key) : "";
}
} // namespace

int runWebBehavior() {
  const auto &Command = WebArguments[0];
  const bool Install =
      Command == "package-install" || Command == "archive-package-install";
  const bool Archive = Command == "archive-package-behavior" ||
                       Command == "archive-package-install";
  const bool Package = Archive || Command == "package-behavior" || Install;
  const bool Bun = Command == "bun-node-behavior";
  const size_t FirstSource = Archive ? 6 : Package ? 4 : Bun ? 3 : 2;
  auto Usage = [] {
    llvm::errs()
        << "usage: neverd web node-behavior <root> <artifact-index:type> | "
           "bun-node-behavior <root> <artifact-index> <module-index:type> | "
           "package-behavior <root> <manifest-index> <npm-lock|package-json> "
           "<source-index:type>... | archive-package-behavior <root> "
           "<tar|tgz|zip> <archive-index> <manifest-member-index> "
           "<npm-lock|package-json> <source-member-index:type>...\n"
           "package-install and archive-package-install accept the same "
           "package arguments with zero to 16 selected sources\n"
           "type: script, module, or commonjs; at most 16 selected sources\n";
    return 2;
  };
  if (WebArguments.size() < FirstSource + (Install ? 0 : 1) ||
      WebArguments.size() > FirstSource + (Package ? 16 : 1))
    return Usage();
  const std::string Format = Archive ? WebArguments[2] : "";
  const std::string Kind = Package ? WebArguments[Archive ? 5 : 3] : "";
  if ((Archive && Format != "tar" && Format != "tgz" && Format != "zip") ||
      (Package && Kind != "npm-lock" && Kind != "package-json"))
    return Usage();
  uint64_t ArtifactIndex = 0, ManifestIndex = 0;
  if (((Archive || Bun) &&
       !decimal(WebArguments[Archive ? 3 : 2], ArtifactIndex)) ||
      (Package && !decimal(WebArguments[Archive ? 4 : 2], ManifestIndex)))
    return Usage();
  std::vector<std::pair<uint64_t, std::string>> Sources;
  for (size_t I = FirstSource; I < WebArguments.size(); ++I) {
    const auto [Index, Type] = llvm::StringRef(WebArguments[I]).split(':');
    uint64_t N = 0;
    if (!decimal(Index, N) ||
        (Type != "script" && Type != "module" && Type != "commonjs"))
      return Usage();
    Sources.emplace_back(N, Type.str());
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
  const auto Revision = field(*Capture, "revision");
  std::string ArchiveID, ExtractionID;
  auto Artifact = [&](uint64_t Index) {
    return selected(neverd_web_artifacts_json(G.S, Revision.data(),
                                              Revision.size(), Index, 1),
                    "artifact_id");
  };
  if (Archive || Bun) {
    const auto A = Artifact(ArtifactIndex);
    if (A.empty())
      return 1;
    const auto E =
        reply(Archive ? neverd_web_package_archive_extract_json(
                            G.S, Revision.data(), Revision.size(), A.data(),
                            A.size(), Format.data(), Format.size())
                      : neverd_web_bun_extract_json(G.S, Revision.data(),
                                                    Revision.size(), A.data(),
                                                    A.size()));
    if (!E)
      return 1;
    ArchiveID = field(*E, "archive_id");
    ExtractionID = field(*E, "extraction_id");
  }
  auto Member = [&](uint64_t Index) {
    if (Archive)
      return selected(neverd_web_package_archive_records_json(
                          G.S, Revision.data(), Revision.size(),
                          ArchiveID.data(), ArchiveID.size(), Index, 1),
                      "member_id");
    if (Bun)
      return selected(
          neverd_web_bun_records_json(G.S, Revision.data(), Revision.size(),
                                      ExtractionID.data(), ExtractionID.size(),
                                      "modules", 7, Index, 1),
          "source_artifact_id");
    return Artifact(Index);
  };
  std::string PackageID;
  if (Package) {
    const auto Manifest = Member(ManifestIndex);
    if (Manifest.empty())
      return 1;
    const auto P = reply(neverd_web_packages_analyze_json(
        G.S, Revision.data(), Revision.size(), Manifest.data(), Manifest.size(),
        Kind.data(), Kind.size()));
    if (!P)
      return 1;
    PackageID = field(*P, "package_analysis_id");
  }
  llvm::json::Array IDs;
  std::string SourceID;
  for (const auto &[Index, Type] : Sources) {
    const auto A = Member(Index);
    if (A.empty())
      return 1;
    const auto S = reply(neverd_web_source_analyze_json(
        G.S, Revision.data(), Revision.size(), A.data(), A.size(), Type.data(),
        Type.size()));
    if (!S)
      return 1;
    SourceID = field(*S, "source_id");
    IDs.push_back(SourceID);
  }
  const auto Options =
      llvm::formatv("{0}",
                    llvm::json::Value(llvm::json::Object{
                        {"schema_version", 1}, {"source_ids", std::move(IDs)}}))
          .str();
  if (Install) {
    llvm::json::Object InstallOptions{{"schema_version", 1}};
    if (!Sources.empty()) {
      const auto B = reply(neverd_web_package_behavior_analyze_json(
          G.S, Revision.data(), Revision.size(), PackageID.data(),
          PackageID.size(), Options.data(), Options.size()));
      if (!B)
        return 1;
      InstallOptions["package_behavior_id"] = field(*B, "package_behavior_id");
    }
    const auto O =
        llvm::formatv("{0}", llvm::json::Value(std::move(InstallOptions)))
            .str();
    const auto A =
        reply(neverd_web_package_install_analyze_json(
                  G.S, Revision.data(), Revision.size(), PackageID.data(),
                  PackageID.size(), O.data(), O.size()),
              true);
    if (!A)
      return 1;
    const auto ID = field(*A, "package_install_id");
    for (const std::string Kind :
         {"scripts", "sources", "requests", "associations"}) {
      uint64_t Offset = 0;
      for (;;) {
        const auto Page =
            reply(neverd_web_package_install_records_json(
                      G.S, Revision.data(), Revision.size(), ID.data(),
                      ID.size(), Kind.data(), Kind.size(), Offset, 128),
                  true);
        if (!Page)
          return 1;
        const auto Next = Page->getAsObject()->getInteger("next_offset");
        if (!Next)
          break;
        if (*Next <= 0 || uint64_t(*Next) <= Offset ||
            *Next > ::neverd::limits::cli::kMaxWebBehaviorOffset)
          return 1;
        Offset = *Next;
      }
    }
    return 0;
  }
  const auto Analysis = reply(
      Package ? neverd_web_package_behavior_analyze_json(
                    G.S, Revision.data(), Revision.size(), PackageID.data(),
                    PackageID.size(), Options.data(), Options.size())
              : neverd_web_node_behavior_analyze_json(
                    G.S, Revision.data(), Revision.size(), SourceID.data(),
                    SourceID.size()),
      true);
  if (!Analysis)
    return 1;
  const auto ID =
      field(*Analysis, Package ? "package_behavior_id" : "node_behavior_id");
  for (const std::string RowKind : {"sources", "records"}) {
    if (!Package && RowKind == "sources")
      continue;
    uint64_t Offset = 0;
    for (;;) {
      const auto Page = reply(
          Package ? neverd_web_package_behavior_records_json(
                        G.S, Revision.data(), Revision.size(), ID.data(),
                        ID.size(), RowKind.data(), RowKind.size(), Offset, 128)
                  : neverd_web_node_behavior_records_json(
                        G.S, Revision.data(), Revision.size(), ID.data(),
                        ID.size(), Offset, 128),
          true);
      if (!Page)
        return 1;
      const auto Next = Page->getAsObject()->getInteger("next_offset");
      if (!Next)
        break;
      if (*Next <= 0 || uint64_t(*Next) <= Offset ||
          *Next > ::neverd::limits::cli::kMaxWebBehaviorOffset)
        return 1;
      Offset = uint64_t(*Next);
    }
  }
  return 0;
}
} // namespace neverd::cli
