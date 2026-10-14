//===- DataPackCorpusSDKTests.cpp - Pinned real desktop resource packs ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Inert CEF distribution and local WebView2 resource qualification. No app,
/// producer, script, external decoder or language runtime is executed.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPIWeb.h"

#include "llvm/Support/JSON.h"

#include <cstdlib>
#include <stdexcept>
#include <string_view>

namespace {
using Value = llvm::json::Value;
Value take(const char *Owned) {
  if (!Owned)
    throw std::runtime_error("missing corpus response");
  auto V = llvm::json::parse(Owned);
  neverd_free_string(Owned);
  if (!V)
    throw std::runtime_error(llvm::toString(V.takeError()));
  return std::move(*V);
}
std::string field(const Value &V, const char *Key) {
  return V.getAsObject()->getString(Key).value_or("").str();
}
struct CorpusSession {
  neverd_web_session_t S = neverd_web_session_create();
  std::string Revision, Artifact;
  ~CorpusSession() { neverd_web_session_destroy(S); }
  void open(std::string_view Path, std::string_view Hash) {
    ASSERT_NE(S, nullptr);
    const auto P = take(neverd_web_import_preview_json(
        S, Path.data(), Path.size(), nullptr, 0));
    ASSERT_EQ(field(P, "status"), "ok");
    const auto Token = field(P, "preview_token");
    const auto C =
        take(neverd_web_import_commit_json(S, Token.data(), Token.size()));
    ASSERT_EQ(field(C, "status"), "ok");
    Revision = field(C, "revision");
    const auto Page = take(
        neverd_web_artifacts_json(S, Revision.data(), Revision.size(), 0, 1));
    const auto *Items = Page.getAsObject()->getArray("items");
    ASSERT_NE(Items, nullptr);
    ASSERT_EQ(Items->size(), 1U);
    EXPECT_EQ(field((*Items)[0], "blob_sha256"), Hash);
    Artifact = field((*Items)[0], "artifact_id");
  }
  Value extract(std::string_view Profile) {
    return take(neverd_web_datapack_extract_json(
        S, Revision.data(), Revision.size(), Artifact.data(), Artifact.size(),
        Profile.data(), Profile.size()));
  }
  Value page(std::string_view ID, uint64_t Offset, uint64_t Limit = 1) {
    return take(neverd_web_datapack_records_json(S, Revision.data(),
                                                 Revision.size(), ID.data(),
                                                 ID.size(), Offset, Limit));
  }
  Value decode(std::string_view Selection) {
    constexpr std::string_view Codec = "gzip-rfc1952-v1";
    return take(neverd_web_datapack_decode_json(
        S, Revision.data(), Revision.size(), Selection.data(), Selection.size(),
        Codec.data(), Codec.size()));
  }
  Value source(std::string_view Selection, std::string_view Type) {
    return take(neverd_web_source_analyze_large_json(
        S, Revision.data(), Revision.size(), Selection.data(), Selection.size(),
        Type.data(), Type.size()));
  }
  Value anchor(std::string_view Source) {
    return take(neverd_web_source_anchor_json(S, Revision.data(),
                                              Revision.size(), Source.data(),
                                              Source.size(), 0, 6, nullptr, 0));
  }
};

TEST(WebDataPackReal, PinnedCEF12027NuGetResourcesFeedSourceAndHTMLConsumers) {
  const auto *Path = std::getenv("NEVERD_CEF_REDIST_12027_X64");
  const auto *Required = std::getenv("NEVERD_REQUIRE_CEF_RESOURCE_CORPUS");
  if (!Path || !*Path) {
    ASSERT_FALSE(Required && std::string_view(Required) == "1")
        << "Required fixed CEF 120.2.7 NuGet distribution is missing";
    GTEST_SKIP()
        << "Set NEVERD_CEF_REDIST_12027_X64 to the fixed NuGet package";
  }
  CorpusSession H;
  H.open(Path,
         "e7f2e0b6860d8cce3ab66d7cb50492b9e1ea601172c850e5c7595d2a45baff17");
  ASSERT_FALSE(::testing::Test::HasFatalFailure());
  const auto Archive = take(neverd_web_package_archive_extract_json(
      H.S, H.Revision.data(), H.Revision.size(), H.Artifact.data(),
      H.Artifact.size(), "zip", 3));
  ASSERT_EQ(field(Archive, "status"), "ok");
  const auto ArchiveID = field(Archive, "archive_id"), Outer = H.Artifact;
  const auto Members = take(neverd_web_package_archive_records_json(
      H.S, H.Revision.data(), H.Revision.size(), ArchiveID.data(),
      ArchiveID.size(), 0, 128));
  const auto *Items = Members.getAsObject()->getArray("items");
  ASSERT_NE(Items, nullptr);
  std::string Pack;
  for (const auto &M : *Items)
    if (field(M, "blob_sha256") ==
        "5a18a11950d57afda243b033083fb83aac3ed6785bc85888a794d4f9da874543")
      Pack = field(M, "member_id");
  ASSERT_FALSE(Pack.empty());
  H.Artifact = Pack;
  const auto E = H.extract("chromium-datapack-v5-v1");
  ASSERT_EQ(field(E, "status"), "ok");
  EXPECT_EQ(E.getAsObject()->getInteger("resource_count"), 2504);
  EXPECT_EQ(E.getAsObject()->getInteger("alias_count"), 16);
  EXPECT_EQ(field(E, "index_byte_length"), "15106");
  const auto ID = field(E, "extraction_id");
  const auto P = H.page(ID, 5);
  const auto &R = (*P.getAsObject()->getArray("items"))[0];
  EXPECT_EQ(R.getAsObject()->getInteger("resource_id"), 698);
  EXPECT_EQ(field(R, "container_byte_offset"), "24660");
  EXPECT_EQ(field(R, "stored_byte_length"), "23108");
  EXPECT_EQ(field(R, "stored_sha256"),
            "aa74295a42e524cee7460ad73481a44aec820a3c474064aa1e790368c37d1a42");
  const auto D = H.decode(field(R, "selection_id"));
  ASSERT_EQ(field(D, "status"), "ok");
  EXPECT_EQ(field(D, "decoded_byte_length"), "102823");
  EXPECT_EQ(field(D, "decoded_sha256"),
            "f017c5aec879c43e4b7753dbddbccbbaace776a39ac527f4ebb7b913ef41b703");
  const auto S = H.source(field(D, "selection_id"), "module");
#if NEVERD_TEST_WEB_JAVASCRIPT
  ASSERT_EQ(field(S, "parse_status"), "parsed");
  EXPECT_EQ(S.getAsObject()->getInteger("node_count"), 11830);
  const auto A = H.anchor(field(S, "source_id"));
  const auto *Storage = A.getAsObject()->getObject("storage");
  ASSERT_NE(Storage, nullptr);
  EXPECT_EQ(Storage->getString("kind"), "datapack_decoded_resource");
  EXPECT_EQ(Storage->getString("mapping"), "containing_compressed_frame");
  const auto *Stored = Storage->getObject("stored_origin");
  ASSERT_NE(Stored, nullptr);
  EXPECT_EQ(Stored->getString("storage_artifact_id"), Outer);
  EXPECT_EQ(Stored->getString("container_byte_offset"), "24660");
  ASSERT_NE(Stored->getObject("container_origin"), nullptr);
#else
  EXPECT_EQ(field(S, "status"), "error");
#endif
  const auto HP = H.page(ID, 4);
  const auto HD = H.decode(
      field((*HP.getAsObject()->getArray("items"))[0], "selection_id"));
  ASSERT_EQ(field(HD, "status"), "ok");
  EXPECT_EQ(field(HD, "decoded_sha256"),
            "c85a2d4ea185d35006b03e184f6650ec17a33e9352d08fb2d44a0499be273e6b");
  const auto HTMLID = field(HD, "selection_id");
  const auto HTML = take(neverd_web_html_analyze_json(
      H.S, H.Revision.data(), H.Revision.size(), HTMLID.data(), HTMLID.size()));
  ASSERT_EQ(field(HTML, "status"), "ok");
  EXPECT_EQ(HTML.getAsObject()->getInteger("script_count"), 1);
}

TEST(WebDataPackReal, PinnedTeamsWebView2154UsesTheMeasuredWideResourceTable) {
  const auto *Path = std::getenv("NEVERD_TEAMS_WV2_154_PAK");
  if (!Path || !*Path)
    GTEST_SKIP()
        << "Set NEVERD_TEAMS_WV2_154_PAK to the fixed local resource pack";
  CorpusSession H;
  H.open(Path,
         "b3b4fb23fa809ac33895d6f0a87d03fdb6a2f087d6a15824d39faf48644c7b8d");
  ASSERT_FALSE(::testing::Test::HasFatalFailure());
  const auto E = H.extract("msedge-webview2-datapack-v5-u32-v1");
  ASSERT_EQ(field(E, "status"), "ok");
  EXPECT_EQ(E.getAsObject()->getInteger("resource_count"), 5814);
  EXPECT_EQ(E.getAsObject()->getInteger("alias_count"), 633);
  EXPECT_EQ(field(E, "index_byte_length"), "51600");
  const auto ID = field(E, "extraction_id");
  const auto P = H.page(ID, 41);
  const auto &R = (*P.getAsObject()->getArray("items"))[0];
  EXPECT_EQ(R.getAsObject()->getInteger("resource_id"), 24521);
  EXPECT_EQ(field(R, "container_byte_offset"), "621511");
  EXPECT_EQ(field(R, "stored_byte_length"), "535");
  EXPECT_EQ(field(R, "stored_sha256"),
            "f0a4be163e13efddbfc172e6d1e433f5ccce9385a5b3fdd3771b1133ea144d5f");
  const auto S = H.source(field(R, "selection_id"), "script");
#if NEVERD_TEST_WEB_JAVASCRIPT
  ASSERT_EQ(field(S, "parse_status"), "parsed");
  EXPECT_EQ(S.getAsObject()->getInteger("node_count"), 56);
  const auto A = H.anchor(field(S, "source_id"));
  const auto *Storage = A.getAsObject()->getObject("storage");
  ASSERT_NE(Storage, nullptr);
  EXPECT_EQ(Storage->getString("kind"), "datapack_stored_resource");
  EXPECT_EQ(Storage->getString("mapping"), "byte_identity");
  EXPECT_EQ(Storage->getString("byte_offset"), "621511");
#else
  EXPECT_EQ(field(S, "status"), "error");
#endif
}
} // namespace
