//===- TauriSDKTests.cpp - Native projection and immutable consumers
//-------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SDK boundaries, nested original evidence, shared source/HTML and local
/// export.
///
//===----------------------------------------------------------------------===//

#include "PackageArchiveFixture.h"
#include "TauriFixture.h"
#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPIWeb.h"
#include "neverd/web/Tauri.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace {
using namespace neverd::web::tauri_test;
using Value = llvm::json::Value;
Value take(const char *Owned) {
  if (!Owned)
    throw std::runtime_error("missing response");
  const std::string Text(Owned);
  neverd_free_string(Owned);
  EXPECT_EQ(Text.find("CANARY"), std::string::npos);
  auto V = llvm::json::parse(Text);
  if (!V) {
    llvm::consumeError(V.takeError());
    throw std::runtime_error("invalid response");
  }
  return std::move(*V);
}
std::string field(const Value &V, const char *Name) {
  return V.getAsObject()->getString(Name).value_or("").str();
}
std::string code(const Value &V) {
  EXPECT_EQ(field(V, "status"), "error");
  return V.getAsObject()->getObject("error")->getString("code")->str();
}
std::string read(const std::filesystem::path &P) {
  std::ifstream F(P, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(F), {});
}
class WebTauriSDK : public ::testing::Test {
protected:
  neverd_web_session_t S = nullptr;
  std::filesystem::path Root;
  std::string Path, Revision, Artifact;
  void SetUp() override {
    const auto Caps = take(neverd_web_capabilities_json());
    bool Available = false;
    for (const auto &A : *Caps.getAsObject()->getArray("analysis"))
      Available |= field(A, "kind") == "tauri_extraction";
    if (!Available)
      GTEST_SKIP() << "pinned Tauri codec/path policy unavailable";
    llvm::SmallString<128> D;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-tauri-sdk", D));
    Root = D.str().str();
    Path = (Root / "INPUT_CANARY").string();
    S = neverd_web_session_create();
    ASSERT_NE(S, nullptr);
  }
  void TearDown() override {
    neverd_web_session_destroy(S);
    std::error_code EC;
    std::filesystem::remove_all(Root, EC);
  }
  void capture(std::string_view Bytes) {
    {
      std::ofstream F(Path, std::ios::binary);
      F.write(Bytes.data(), Bytes.size());
    }
    const auto P = take(neverd_web_import_preview_json(
        S, Path.data(), Path.size(), nullptr, 0));
    const auto T = field(P, "preview_token");
    Revision = field(take(neverd_web_import_commit_json(S, T.data(), T.size())),
                     "revision");
    const auto Page = take(
        neverd_web_artifacts_json(S, Revision.data(), Revision.size(), 0, 1));
    Artifact =
        field((*Page.getAsObject()->getArray("items"))[0], "artifact_id");
  }
  Value extract(uint64_t PC = Start, std::string_view ID = {},
                std::string_view Profile = neverd::web::TauriProfile) {
    if (ID.empty())
      ID = Artifact;
    return take(neverd_web_tauri_extract_json(
        S, Revision.data(), Revision.size(), ID.data(), ID.size(), PC,
        Profile.data(), Profile.size()));
  }
  Value page(const std::string &ID, uint64_t At = 0, uint64_t Count = 128) {
    return take(neverd_web_tauri_records_json(
        S, Revision.data(), Revision.size(), ID.data(), ID.size(), At, Count));
  }
};
TEST_F(WebTauriSDK, NativeProofPrivatePagesCacheAndRevisionRevocation) {
  capture(image());
  const auto E = extract();
  ASSERT_EQ(field(E, "status"), "ok") << llvm::formatv("{0}", E).str();
  EXPECT_EQ(field(E, "initializer_basis"),
            "native_lowir_selected_constant_stores");
  EXPECT_EQ(field(E, "phf_lookup_status"), "all_keys_match_rows");
  EXPECT_EQ(field(E, "asset_server_binding"), "not_checked");
  EXPECT_EQ(E.getAsObject()->getBoolean("executes_input"), false);
  const auto ID = field(E, "extraction_id");
  EXPECT_EQ(field(extract(), "extraction_id"), ID);
  const auto Page = page(ID);
  ASSERT_EQ(Page.getAsObject()->getArray("items")->size(), 2u);
  EXPECT_EQ(code(page(ID, 3)), "invalid_page");
  EXPECT_EQ(code(page(ID, 0, 0)), "invalid_page");
  EXPECT_EQ(code(page(ID, 0, 129)), "invalid_page");
  EXPECT_EQ(code(extract(Start + 1)),
            "native_store_instruction_boundary_required");
  EXPECT_EQ(code(extract(Start, {}, "tauri-latest")),
            "tauri_unsupported_profile");
  const auto Old = Revision;
  capture(image());
  EXPECT_EQ(code(take(neverd_web_tauri_records_json(
                S, Old.data(), Old.size(), ID.data(), ID.size(), 0, 1))),
            "stale_revision");
  EXPECT_EQ(code(page(ID)), "unknown_tauri_extraction");
}
TEST_F(WebTauriSDK, ForeignContainersAndChainedFixupsRemainUnsupported) {
  for (const auto &Bytes :
       {neverd::web::test::nativeELF(), neverd::web::test::nativePE()}) {
    capture(Bytes);
    EXPECT_EQ(code(extract()), "tauri_native_profile_unavailable");
  }
  auto Bytes = image();
  auto Put = [&](uint64_t At, uint64_t Value, unsigned Width = 4) {
    neverd::web::test::nativePut(Bytes, At, Value, Width);
  };
  Put(16, 4);
  Put(20, 208);
  Put(224, 0x80000034); // LC_DYLD_CHAINED_FIXUPS
  Put(228, 16);
  Put(232, 1024);
  Put(236, 36);
  Put(1028, 28); // starts_offset
  Put(1032, 36); // imports_offset
  Put(1036, 36); // symbols_offset
  Put(1044, 1);  // imports_format; no imports
  Put(1052, 1);  // one segment with zero chain-start offset
  capture(Bytes);
  EXPECT_EQ(code(extract()), "tauri_native_profile_unavailable");
  EXPECT_EQ(take(neverd_web_metadata_json(S))
                .getAsObject()
                ->getInteger("tauri_extraction_count"),
            0);
}
TEST_F(WebTauriSDK, NestedArchiveConsumersKeepCompressedAndDecodedCoordinates) {
  using namespace neverd::web::test;
  capture(finishTar(tarMember("PRIVATE_CANARY/app.bin", image())));
  const auto Archive = take(neverd_web_package_archive_extract_json(
      S, Revision.data(), Revision.size(), Artifact.data(), Artifact.size(),
      "tar", 3));
  ASSERT_EQ(field(Archive, "status"), "ok");
  const auto ArchiveID = field(Archive, "archive_id");
  const auto Rows = take(neverd_web_package_archive_records_json(
      S, Revision.data(), Revision.size(), ArchiveID.data(), ArchiveID.size(),
      0, 1));
  const auto Native =
      field((*Rows.getAsObject()->getArray("items"))[0], "member_id");
  const auto E = extract(Start, Native);
  ASSERT_EQ(field(E, "status"), "ok") << llvm::formatv("{0}", E).str();
  const auto ID = field(E, "extraction_id");
  const auto Members = page(ID);
  const auto JS =
      field((*Members.getAsObject()->getArray("items"))[0], "selection_id");
  const auto H =
      field((*Members.getAsObject()->getArray("items"))[1], "selection_id");
  const auto HTML = take(neverd_web_html_analyze_json(
      S, Revision.data(), Revision.size(), H.data(), H.size()));
  ASSERT_EQ(field(HTML, "status"), "ok");
  const auto Destination = (Root / "raw").string();
  const auto Export = take(neverd_web_tauri_export_json(
      S, Revision.data(), Revision.size(), ID.data(), ID.size(),
      Destination.data(), Destination.size()));
  ASSERT_EQ(field(Export, "status"), "ok");
  EXPECT_EQ(Export.getAsObject()->getInteger("file_count"), 6);
  EXPECT_EQ(read(std::filesystem::path(Destination) / "original.bin"), image());
  EXPECT_EQ(read(std::filesystem::path(Destination) / "m00000.br"),
            compressedJS());
  EXPECT_EQ(read(std::filesystem::path(Destination) / "m00000.js"),
            javascript());
  EXPECT_EQ(code(take(neverd_web_tauri_export_json(
                S, Revision.data(), Revision.size(), ID.data(), ID.size(),
                Destination.data(), Destination.size()))),
            "export_destination_exists");
  const auto ReadableDir = (Root / "readable").string();
  const auto Readable = take(neverd_web_source_readable_export_json(
      S, Revision.data(), Revision.size(), JS.data(), JS.size(), "module", 6,
      ReadableDir.data(), ReadableDir.size()));
#if NEVERD_TEST_WEB_JAVASCRIPT
  ASSERT_EQ(field(Readable, "readable_status"), "verified_same_parser_tree");
  const auto *Origin = Readable.getAsObject()->getObject("original_origin");
  ASSERT_NE(Origin, nullptr);
  EXPECT_EQ(Origin->getString("kind"), "tauri_decoded_member");
  EXPECT_EQ(Origin->getObject("container_origin")->getString("kind"),
            "package_archive_member");
  EXPECT_EQ(Readable.getAsObject()
                ->getObject("original_range")
                ->getString("expanded_byte_offset"),
            "0");
  EXPECT_EQ(read(std::filesystem::path(ReadableDir) / "original.bin"),
            javascript());
#else
  EXPECT_EQ(code(Readable), "capability_unavailable");
#endif
  neverd_session_t NativeSession = nullptr;
  const auto Open = take(neverd_web_native_open_json(
      S, Revision.data(), Revision.size(), Native.data(), Native.size(),
      &NativeSession));
  EXPECT_EQ(field(Open, "status"), "ok");
  EXPECT_NE(NativeSession, nullptr);
  neverd_session_destroy(NativeSession);
}
TEST_F(WebTauriSDK, OptionalChatGPT110GNUArchiveAndAllDecodedMembers) {
  const auto *Input = std::getenv("NEVERD_TAURI_CHATGPT_110_X64_TGZ");
  if (!Input)
    GTEST_SKIP() << "preserved public qualification input unavailable";
  capture(read(Input));
  const auto A = take(neverd_web_package_archive_extract_json(
      S, Revision.data(), Revision.size(), Artifact.data(), Artifact.size(),
      "tgz", 3));
  ASSERT_EQ(field(A, "status"), "ok");
  EXPECT_EQ(field(A, "profile"), "gnu-oldtar-basic-single-gzip-v1");
  const auto ArchiveID = field(A, "archive_id");
  const auto Members = take(neverd_web_package_archive_records_json(
      S, Revision.data(), Revision.size(), ArchiveID.data(), ArchiveID.size(),
      2, 1));
  const auto N =
      field((*Members.getAsObject()->getArray("items"))[0], "member_id");
  const auto E = extract(0x1000135a8, N);
  ASSERT_EQ(field(E, "status"), "ok");
  EXPECT_EQ(field(E, "container_sha256"),
            "351cffd71cc63205dda57990b1432d124150bd625ae886fc577a03e49e9bed92");
  EXPECT_EQ(E.getAsObject()->getInteger("member_count"), 15);
  EXPECT_EQ(field(E, "entries_container_offset"), "8421464");
  const auto ID = field(E, "extraction_id");
  const auto Rows = page(ID);
  const auto Directory = (Root / "real-export").string();
  const auto Out = take(neverd_web_tauri_export_json(
      S, Revision.data(), Revision.size(), ID.data(), ID.size(),
      Directory.data(), Directory.size()));
  ASSERT_EQ(field(Out, "status"), "ok");
  EXPECT_EQ(Out.getAsObject()->getInteger("file_count"), 32);
#if NEVERD_TEST_WEB_JAVASCRIPT
  for (const auto &M : *Rows.getAsObject()->getArray("items")) {
    if (field(M, "kind") != "javascript")
      continue;
    const auto Selection = field(M, "selection_id");
    const auto Dest = (Root / Selection).string();
    const auto R = take(neverd_web_source_readable_export_json(
        S, Revision.data(), Revision.size(), Selection.data(), Selection.size(),
        "module", 6, Dest.data(), Dest.size()));
    ASSERT_EQ(field(R, "status"), "ok");
    EXPECT_EQ(field(R, "readable_status"), "verified_same_parser_tree");
  }
#endif
}
} // namespace
