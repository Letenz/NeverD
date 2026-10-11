//===- NeverDCmdWebAdvisories.cpp - Captured advisory comparisons
//-----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// CLI adapter for bounded package and offline advisory evidence selections.
///
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"

#include "neverd/sdk/NeverDCAPIWeb.h"

#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd::cli {
namespace {
using Value = llvm::json::Value;
std::optional<Value> reply(const char *Owned) {
  if (!Owned) {
    llvm::errs() << "web: allocation_failed\n";
    return {};
  }
  auto V = llvm::json::parse(Owned);
  neverd_free_string(Owned);
  if (!V) {
    llvm::consumeError(V.takeError());
    llvm::errs() << "web: invalid_backend_response\n";
    return {};
  }
  if (!V->getAsObject() || V->getAsObject()->getString("status") != "ok") {
    llvm::outs() << *V << '\n';
    return {};
  }
  return std::move(*V);
}
std::string field(const Value &V, const char *Name) {
  return V.getAsObject()->getString(Name).value_or("").str();
}
bool index(llvm::StringRef Text, uint64_t &Number) {
  return !Text.empty() && !Text.getAsInteger(10, Number) &&
         std::to_string(Number) == Text;
}
template <class Reader> bool pages(Reader Read) {
  uint64_t Offset = 0;
  while (true) {
    const auto P = reply(Read(Offset));
    if (!P)
      return false;
    llvm::outs() << *P << '\n';
    const auto *Continuation = P->getAsObject()->get("next_offset");
    if (!Continuation)
      return false;
    if (Continuation->kind() == Value::Null)
      return true;
    const auto Next = Continuation->getAsInteger();
    if (!Next)
      return false;
    if (*Next <= 0 || uint64_t(*Next) <= Offset)
      return false;
    Offset = uint64_t(*Next);
  }
}
} // namespace

int runWebAdvisories() {
  if (WebArguments.size() < 4 || WebArguments.size() > 20 ||
      (WebArguments[3] != "package-json" && WebArguments[3] != "npm-lock")) {
    llvm::errs()
        << "usage: neverd web package-advisories <root> <manifest-index> "
           "<package-json|npm-lock> [advisory-index:descriptor-index]...\n";
    return 2;
  }
  uint64_t ManifestIndex = 0;
  if (!index(WebArguments[2], ManifestIndex))
    return 2;
  std::vector<std::pair<uint64_t, uint64_t>> Selections;
  for (size_t I = 4; I < WebArguments.size(); ++I) {
    const auto Parts = llvm::StringRef(WebArguments[I]).split(':');
    uint64_t Advisory = 0, Descriptor = 0;
    if (!index(Parts.first, Advisory) || !index(Parts.second, Descriptor))
      return 2;
    Selections.emplace_back(Advisory, Descriptor);
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
  const auto Commit =
      reply(neverd_web_import_commit_json(G.S, Token.data(), Token.size()));
  if (!Commit)
    return 1;
  const auto Revision = field(*Commit, "revision");
  const auto Artifact = [&](uint64_t I) -> std::string {
    const auto P = reply(
        neverd_web_artifacts_json(G.S, Revision.data(), Revision.size(), I, 1));
    const auto *Items = P ? P->getAsObject()->getArray("items") : nullptr;
    return Items && Items->size() == 1 ? field((*Items)[0], "artifact_id") : "";
  };
  const auto Manifest = Artifact(ManifestIndex);
  if (Manifest.empty())
    return 1;
  const auto &Kind = WebArguments[3];
  const auto Inventory = reply(neverd_web_packages_analyze_json(
      G.S, Revision.data(), Revision.size(), Manifest.data(), Manifest.size(),
      Kind.data(), Kind.size()));
  if (!Inventory)
    return 1;
  const auto Package = field(*Inventory, "package_analysis_id");
  std::vector<Value> Imports;
  llvm::json::Array IDs;
  for (const auto &[AdvisoryIndex, DescriptorIndex] : Selections) {
    const auto Advisory = Artifact(AdvisoryIndex);
    const auto Descriptor = Artifact(DescriptorIndex);
    if (Advisory.empty() || Descriptor.empty())
      return 1;
    auto A = reply(neverd_web_package_advisory_import_json(
        G.S, Revision.data(), Revision.size(), Advisory.data(), Advisory.size(),
        Descriptor.data(), Descriptor.size()));
    if (!A)
      return 1;
    IDs.emplace_back(field(*A, "package_advisory_id"));
    Imports.push_back(std::move(*A));
  }
  const auto Options =
      llvm::formatv("{0}",
                    Value(llvm::json::Object{{"schema_version", 1},
                                             {"advisory_ids", std::move(IDs)}}))
          .str();
  const auto Matches = reply(neverd_web_package_advisories_match_json(
      G.S, Revision.data(), Revision.size(), Package.data(), Package.size(),
      Options.data(), Options.size()));
  if (!Matches)
    return 1;
  llvm::outs() << *Inventory << '\n';
  if (!pages([&](uint64_t Offset) {
        return neverd_web_package_records_json(
            G.S, Revision.data(), Revision.size(), Package.data(),
            Package.size(), "packages", 8, Offset, 512);
      }))
    return 1;
  for (const auto &A : Imports) {
    llvm::outs() << A << '\n';
    const auto ID = field(A, "package_advisory_id");
    for (const llvm::StringRef Kind : {"affected", "severities"})
      if (!pages([&](uint64_t Offset) {
            return neverd_web_package_advisory_records_json(
                G.S, Revision.data(), Revision.size(), ID.data(), ID.size(),
                Kind.data(), Kind.size(), Offset, 512);
          }))
        return 1;
  }
  llvm::outs() << *Matches << '\n';
  const auto ID = field(*Matches, "package_advisory_matches_id");
  return pages([&](uint64_t Offset) {
    return neverd_web_package_advisories_match_records_json(
        G.S, Revision.data(), Revision.size(), ID.data(), ID.size(), Offset,
        512);
  })
             ? 0
             : 1;
}
} // namespace neverd::cli
