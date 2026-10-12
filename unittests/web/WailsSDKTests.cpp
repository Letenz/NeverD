//===- WailsSDKTests.cpp - Wails APIs and shared consumers ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Private metadata, distinct table namespaces, byte provenance and CLI use
/// without a runtime or external executable.
///
//===----------------------------------------------------------------------===//

#include "PackageArchiveFixture.h"
#include "WailsFixture.h"
#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPIWeb.h"
#include "neverd/web/Wails.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Program.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace {
using namespace neverd::web::wails_test;
using Value = llvm::json::Value;
constexpr auto Profile = neverd::web::WailsProfile;
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
class WebWailsSDK : public ::testing::Test {
protected:
  neverd_web_session_t S = nullptr;
  std::filesystem::path Root;
  std::string Path, Revision, Artifact;
  void SetUp() override {
    bool Available = false;
    const auto Caps = take(neverd_web_capabilities_json());
    for (const auto &A : *Caps.getAsObject()->getArray("analysis"))
      Available |= field(A, "kind") == "wails_extraction";
    if (!Available)
      GTEST_SKIP() << "pinned native path policy unavailable";
    llvm::SmallString<128> D;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-wails-sdk", D));
    Root = D.str().str();
    Path = (Root / "PRIVATE_INPUT_CANARY").string();
    S = neverd_web_session_create();
    ASSERT_NE(S, nullptr);
  }
  void TearDown() override {
    neverd_web_session_destroy(S);
    std::error_code EC;
    std::filesystem::remove_all(Root, EC);
  }
  void open(const std::string &Selected) {
    const auto P = take(neverd_web_import_preview_json(
        S, Selected.data(), Selected.size(), nullptr, 0));
    const auto T = field(P, "preview_token");
    Revision = field(take(neverd_web_import_commit_json(S, T.data(), T.size())),
                     "revision");
    const auto Page = take(
        neverd_web_artifacts_json(S, Revision.data(), Revision.size(), 0, 1));
    Artifact =
        field((*Page.getAsObject()->getArray("items"))[0], "artifact_id");
  }
  void capture(std::string_view Bytes) {
    {
      std::ofstream F(Path, std::ios::binary);
      F.write(Bytes.data(), Bytes.size());
    }
    open(Path);
  }
  Value extract(std::string_view A = {}, std::string_view P = Profile) {
    if (A.empty())
      A = Artifact;
    return take(neverd_web_wails_extract_json(S, Revision.data(),
                                              Revision.size(), A.data(),
                                              A.size(), P.data(), P.size()));
  }
  Value page(const std::string &ID, std::string_view Kind = "members",
             uint64_t At = 0, uint64_t Count = 128) {
    return take(neverd_web_wails_records_json(
        S, Revision.data(), Revision.size(), ID.data(), ID.size(), Kind.data(),
        Kind.size(), At, Count));
  }
};
TEST_F(WebWailsSDK, PrivatePagesCacheAndRevisionRevocation) {
  capture(elf());
  const auto E = extract();
  ASSERT_EQ(field(E, "status"), "ok");
  EXPECT_EQ(field(E, "asset_server_binding"), "not_checked");
  EXPECT_EQ(E.getAsObject()->getBoolean("producer_version_verified"), false);
  EXPECT_EQ(E.getAsObject()->getBoolean("executes_input"), false);
  const auto ID = field(E, "extraction_id");
  EXPECT_EQ(field(extract(), "extraction_id"), ID);
  const auto M = page(ID);
  ASSERT_EQ(M.getAsObject()->getArray("items")->size(), 3U);
  EXPECT_TRUE((*M.getAsObject()->getArray("items"))[0]
                  .getAsObject()
                  ->get("selection_id")
                  ->getAsNull()
                  .has_value());
  const auto Source =
      field((*M.getAsObject()->getArray("items"))[1], "selection_id");
  EXPECT_EQ(page(ID, "members", 3).getAsObject()->getArray("items")->size(),
            0U);
  EXPECT_EQ(code(page(ID, "members", 4)), "invalid_page");
  EXPECT_EQ(code(page(ID, "members", 0, 129)), "invalid_page");
  EXPECT_EQ(code(page(ID, "members", 0, 0)), "invalid_page");
  EXPECT_EQ(code(page(ID, "paths")), "invalid_record_kind");
  EXPECT_EQ(code(extract({}, "wails-latest")), "wails_unsupported_profile");
  const auto OldRevision = Revision;
  capture(elf());
  EXPECT_EQ(code(take(neverd_web_wails_records_json(
                S, OldRevision.data(), OldRevision.size(), ID.data(), ID.size(),
                "members", 7, 0, 1))),
            "stale_revision");
  EXPECT_EQ(code(page(ID)), "unknown_wails_extraction");
#if NEVERD_TEST_WEB_JAVASCRIPT
  EXPECT_EQ(code(take(neverd_web_source_analyze_json(
                S, Revision.data(), Revision.size(), Source.data(),
                Source.size(), "module", 6))),
            "unknown_artifact");
#endif
}
TEST_F(WebWailsSDK, HTMLReferencesCannotCrossFilesystemTables) {
  auto Second = entries();
  Second.erase(Second.begin() + 1);
  capture(elf({entries(), Second}));
  const auto E = extract();
  const auto Members = page(field(E, "extraction_id"));
  const auto &Items = *Members.getAsObject()->getArray("items");
  for (const auto Index : {2U, 4U}) {
    const auto A = field(Items[Index], "selection_id");
    const auto H = take(neverd_web_html_analyze_json(
        S, Revision.data(), Revision.size(), A.data(), A.size()));
    ASSERT_EQ(field(H, "status"), "ok");
    const auto HID = field(H, "html_id");
    const auto Scripts = take(neverd_web_html_records_json(
        S, Revision.data(), Revision.size(), HID.data(), HID.size(), "scripts",
        7, 0, 1));
    const auto &Script = (*Scripts.getAsObject()->getArray("items"))[0];
    EXPECT_EQ(field(Script, "candidate_artifact_id"),
              Index == 2 ? field(Items[1], "selection_id") : "");
  }
}
TEST_F(WebWailsSDK, BadHashPublicationLeavesOriginalArtifactAvailable) {
  auto B = elf();
  B[0x1400] ^= 1;
  capture(B);
  EXPECT_EQ(code(extract()), "wails_embed_hash_mismatch");
  const auto Meta = take(neverd_web_metadata_json(S));
  EXPECT_EQ(Meta.getAsObject()->getInteger("wails_extraction_count"), 0);
  EXPECT_EQ(field(Meta, "analysis_status"), "not_analyzed");
  EXPECT_EQ(field((*take(neverd_web_artifacts_json(S, Revision.data(),
                                                   Revision.size(), 0, 1))
                        .getAsObject()
                        ->getArray("items"))[0],
                  "artifact_id"),
            Artifact);
}
#if NEVERD_TEST_WEB_JAVASCRIPT
TEST_F(WebWailsSDK, CompressedParentAndSourceAnchorsKeepTheirStorageBasis) {
  using namespace neverd::web::test;
  capture(storedGzip(finishTar(tarMember("package/wails", elf()))));
  const auto Archive = take(neverd_web_package_archive_extract_json(
      S, Revision.data(), Revision.size(), Artifact.data(), Artifact.size(),
      "tgz", 3));
  const auto AID = field(Archive, "archive_id");
  const auto Members = take(neverd_web_package_archive_records_json(
      S, Revision.data(), Revision.size(), AID.data(), AID.size(), 0, 1));
  const auto MID =
      field((*Members.getAsObject()->getArray("items"))[0], "member_id");
  const auto E = extract(MID);
  const auto P = page(field(E, "extraction_id"));
  const auto Source =
      field((*P.getAsObject()->getArray("items"))[1], "selection_id");
  const auto Parsed = take(neverd_web_source_analyze_json(
      S, Revision.data(), Revision.size(), Source.data(), Source.size(),
      "module", 6));
  ASSERT_EQ(field(Parsed, "parse_status"), "parsed");
  const auto SID = field(Parsed, "source_id");
  const auto Anchor = take(
      neverd_web_source_anchor_json(S, Revision.data(), Revision.size(),
                                    SID.data(), SID.size(), 0, 6, nullptr, 0));
  const auto *Storage = Anchor.getAsObject()->getObject("storage");
  ASSERT_NE(Storage, nullptr);
  EXPECT_EQ(Storage->getString("mapping"), "containing_compressed_frame");
  EXPECT_EQ(Storage->getString("storage_artifact_id"), Artifact);
  EXPECT_EQ(Storage->getString("expanded_byte_offset"),
            std::to_string(512 + 0x1400));
  EXPECT_TRUE(Storage->get("byte_offset")->getAsNull().has_value());
}
#endif
TEST_F(WebWailsSDK, RawLocalExportUsesFixedNamesAndRefusesOverwriteOrLinks) {
  const auto Bytes = elf();
  capture(Bytes);
  const auto E = extract();
  const auto ID = field(E, "extraction_id");
  const auto Destination = (Root / "export").string();
  const auto Export = [&](const std::string &Path) {
    return take(neverd_web_wails_export_json(
        S, Revision.data(), Revision.size(), ID.data(), ID.size(), Path.data(),
        Path.size()));
  };
  const auto Reply = Export(Destination);
  ASSERT_EQ(field(Reply, "status"), "ok");
  EXPECT_EQ(Reply.getAsObject()->getInteger("file_count"), 4);
  EXPECT_EQ(field(Reply, "verification"),
            "all_exported_files_reread_sha256_matches");
  auto Read = [&](const char *Name) {
    std::ifstream F(std::filesystem::path(Destination) / Name,
                    std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(F), {});
  };
  EXPECT_EQ(Read("original.bin"), Bytes);
  EXPECT_EQ(Read("m00001.js"), entries()[1].Contents);
  EXPECT_EQ(Read("m00002.html"), entries()[2].Contents);
  const auto ManifestText = Read("manifest.json");
  auto Manifest = llvm::json::parse(ManifestText);
  ASSERT_TRUE(bool(Manifest));
  ASSERT_EQ(Manifest->getAsObject()->getArray("members")->size(), 3U);
  EXPECT_EQ((*Manifest->getAsObject()->getArray("members"))[1]
                .getAsObject()
                ->getString("virtual_path"),
            "PRIVATE_CANARY/app.js");
  EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(Destination) /
                                       "PRIVATE_CANARY"));
  EXPECT_EQ(code(Export(Destination)), "export_destination_exists");
  const auto Link = Root / "link";
  std::filesystem::create_directory_symlink(Destination, Link);
  EXPECT_EQ(code(Export(Link.string())), "export_destination_exists");
  EXPECT_EQ(Read("m00001.js"), entries()[1].Contents);
}
TEST_F(WebWailsSDK, Zentro030NativeHandoffWhenSupplied) {
  const auto *Selected = std::getenv("NEVERD_WAILS_ZENTRO_030_ELF");
  if (!Selected || !*Selected)
    GTEST_SKIP() << "captured Zentro v0.3.0 release required";
  open(Selected);
  const auto E = extract();
  ASSERT_EQ(field(E, "status"), "ok");
  neverd_session_t Native = nullptr;
  const auto N = take(
      neverd_web_native_open_json(S, Revision.data(), Revision.size(),
                                  Artifact.data(), Artifact.size(), &Native));
  EXPECT_EQ(field(N, "status"), "ok");
  ASSERT_NE(Native, nullptr);
  EXPECT_EQ(field(N, "blob_sha256"), field(E, "container_sha256"));
  EXPECT_EQ(N.getAsObject()->getInteger("section_count"), 36);
  EXPECT_EQ(N.getAsObject()->getInteger("function_count"), 32475);
  neverd_web_session_destroy(S);
  S = nullptr;
  EXPECT_EQ(field(take(neverd_web_native_metadata_json(Native)), "status"),
            "ok");
  neverd_session_destroy(Native);
}
#if NEVERD_TEST_WEB_JAVASCRIPT
TEST_F(WebWailsSDK,
       Zentro030HTMLRootRemainsUnboundAndWholeSourceLimitWhenSupplied) {
  const auto *Selected = std::getenv("NEVERD_WAILS_ZENTRO_030_ELF");
  if (!Selected || !*Selected)
    GTEST_SKIP() << "captured Zentro v0.3.0 release required";
  open(Selected);
  const auto E = extract();
  ASSERT_EQ(field(E, "status"), "ok");
  const auto Members = page(field(E, "extraction_id"));
  const auto &Items = *Members.getAsObject()->getArray("items");
  const auto HTML = field(Items[5], "selection_id"),
             JS = field(Items[7], "selection_id");
  const auto H = take(neverd_web_html_analyze_json(
      S, Revision.data(), Revision.size(), HTML.data(), HTML.size()));
  ASSERT_EQ(field(H, "status"), "ok");
  const auto HID = field(H, "html_id");
  const auto Scripts = take(neverd_web_html_records_json(
      S, Revision.data(), Revision.size(), HID.data(), HID.size(), "scripts", 7,
      0, 128));
  ASSERT_EQ(Scripts.getAsObject()->getArray("items")->size(), 1U);
  const auto &Script = (*Scripts.getAsObject()->getArray("items"))[0];
  EXPECT_EQ(field(Script, "link_status"), "unsupported_local_url");
  EXPECT_TRUE(Script.getAsObject()
                  ->get("candidate_artifact_id")
                  ->getAsNull()
                  .has_value());
  EXPECT_EQ(Script.getAsObject()->getBoolean("runtime_entry_verified"), false);
  EXPECT_EQ(code(take(neverd_web_source_analyze_json(S, Revision.data(),
                                                     Revision.size(), JS.data(),
                                                     JS.size(), "module", 6))),
            "source_byte_budget_exceeded");
  const auto Metadata = take(neverd_web_metadata_json(S));
  EXPECT_EQ(Metadata.getAsObject()->getInteger("wails_extraction_count"), 1);
  EXPECT_EQ(Metadata.getAsObject()->getInteger("source_inventory_count"), 0);
}
#endif
#ifdef NEVERD_WEB_TEST_CLI
TEST_F(WebWailsSDK, CLIExtractsAndAnalyzesWithoutExternalTools) {
  capture(elf());
  const auto Out = (Root / "out").string(), Err = (Root / "err").string();
  const llvm::StringRef Env[]{"PATH=/neverd-no-external-tools", "LC_ALL=C"};
  const std::optional<llvm::StringRef> Redirects[]{std::nullopt, Out, Err};
  const auto Destination = (Root / "readable").string();
  for (const auto *Command :
       {"wails", "wails-html", "wails-source", "wails-readable-export"}) {
    const bool Readable = std::string_view(Command) == "wails-readable-export";
    std::vector<llvm::StringRef> Args{NEVERD_WEB_TEST_CLI, "web", Command, Path,
                                      Profile};
    if (std::string_view(Command) != "wails") {
      Args.emplace_back("0");
      Args.emplace_back(std::string_view(Command) == "wails-html" ? "2" : "1");
    }
    if (std::string_view(Command) == "wails-source" || Readable)
      Args.emplace_back("module");
    if (Readable)
      Args.emplace_back(Destination);
    const auto Expected =
        !NEVERD_TEST_WEB_JAVASCRIPT &&
                (std::string_view(Command) == "wails-source" || Readable)
            ? 1
            : 0;
    ASSERT_EQ(llvm::sys::ExecuteAndWait(NEVERD_WEB_TEST_CLI, Args, Env,
                                        Redirects, 30),
              Expected);
    std::ifstream F(Out), E(Err);
    const std::string Text{std::istreambuf_iterator<char>(F), {}},
        Errors{std::istreambuf_iterator<char>(E), {}};
    EXPECT_EQ(Text.find("CANARY"), std::string::npos);
    EXPECT_EQ(Errors.find("CANARY"), std::string::npos);
    EXPECT_NE(Text.find("extraction_id"), std::string::npos);
    if (Readable)
      EXPECT_EQ(std::filesystem::exists(std::filesystem::path(Destination) /
                                        "readable.js"),
                bool(NEVERD_TEST_WEB_JAVASCRIPT));
  }
}
#endif
} // namespace
