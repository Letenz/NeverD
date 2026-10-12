//===- NeverDCmdWebAttestation.cpp - Captured attestation inspection -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Offline metadata and one explicitly selected original-byte digest join.
///
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"

#include "neverd/sdk/NeverDCAPIWeb.h"

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
} // namespace

int runWebAttestation() {
  if ((WebArguments.size() != 5 && WebArguments.size() != 7) ||
      (WebArguments[4] != "registry-dist" &&
       WebArguments[4] != "intoto-statement" &&
       WebArguments[4] != "dsse-intoto")) {
    llvm::errs() << "usage: neverd web package-attestation <root> "
                    "<metadata-index> <descriptor-index> "
                    "<registry-dist|intoto-statement|dsse-intoto> "
                    "[original-index digest-index]\n";
    return 2;
  }
  uint64_t InputIndex = 0, DescriptorIndex = 0, OriginalIndex = 0,
           DigestIndex = 0;
  if (!index(WebArguments[2], InputIndex) ||
      !index(WebArguments[3], DescriptorIndex) ||
      (WebArguments.size() == 7 && (!index(WebArguments[5], OriginalIndex) ||
                                    !index(WebArguments[6], DigestIndex))))
    return 2;
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
  const auto Artifact = [&](uint64_t I) {
    const auto P = reply(
        neverd_web_artifacts_json(G.S, Revision.data(), Revision.size(), I, 1));
    const auto *Items = P ? P->getAsObject()->getArray("items") : nullptr;
    return Items && Items->size() == 1 ? field((*Items)[0], "artifact_id")
                                       : std::string();
  };
  const auto Input = Artifact(InputIndex),
             Descriptor = Artifact(DescriptorIndex);
  if (Input.empty() || Descriptor.empty())
    return 1;
  const auto &Kind = WebArguments[4];
  auto A = reply(neverd_web_package_attestation_import_json(
      G.S, Revision.data(), Revision.size(), Input.data(), Input.size(),
      Descriptor.data(), Descriptor.size(), Kind.data(), Kind.size()));
  if (!A)
    return 1;
  const auto ID = field(*A, "package_attestation_id");
  std::vector<Value> Reports;
  Reports.push_back(std::move(*A));
  std::string Digest;
  for (const llvm::StringRef Records : {"signatures", "subjects", "digests"}) {
    auto Page = reply(neverd_web_package_attestation_records_json(
        G.S, Revision.data(), Revision.size(), ID.data(), ID.size(),
        Records.data(), Records.size(), 0, 512));
    // The profile caps each record set at 256, so exactly one complete page
    // is required. Do not silently discard a future profile's continuation.
    if (!Page || Page->getAsObject()->getBoolean("page_complete") != true)
      return 1;
    const auto *Items = Page->getAsObject()->getArray("items");
    if (Records == "digests" && Items && DigestIndex < Items->size())
      Digest = field((*Items)[DigestIndex], "digest_id");
    Reports.push_back(std::move(*Page));
  }
  if (WebArguments.size() == 7) {
    const auto Original = Artifact(OriginalIndex);
    if (Original.empty() || Digest.empty()) {
      llvm::errs() << "web: unavailable_subject_digest_selection\n";
      return 1;
    }
    auto M = reply(neverd_web_package_attestation_digest_match_json(
        G.S, Revision.data(), Revision.size(), Original.data(), Original.size(),
        ID.data(), ID.size(), Digest.data(), Digest.size()));
    if (!M)
      return 1;
    Reports.push_back(std::move(*M));
  }
  for (const auto &Report : Reports)
    llvm::outs() << Report << '\n';
  return 0;
}
} // namespace neverd::cli
