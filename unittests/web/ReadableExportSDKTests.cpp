//===- ReadableExportSDKTests.cpp - Shared local recovery boundaries ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exact selected bytes, independent large-source limits, refusal receipts
/// and nested storage origins without a runtime or external executable.
///
//===----------------------------------------------------------------------===//

#include "PackageArchiveFixture.h"
#include "WailsFixture.h"
#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPIWeb.h"
#include "neverd/web/Wails.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SHA256.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {
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
#if NEVERD_TEST_WEB_JAVASCRIPT
std::string hash(std::string_view Text) {
  llvm::SHA256 H;
  H.update(llvm::StringRef(Text.data(), Text.size()));
  return llvm::toHex(H.final(), true);
}
std::string read(const std::filesystem::path &Path) {
  std::ifstream F(Path, std::ios::binary);
  if (!F)
    throw std::runtime_error("missing local evidence");
  return std::string(std::istreambuf_iterator<char>(F), {});
}
#endif
class WebReadableExportSDK : public ::testing::Test {
protected:
  neverd_web_session_t S = nullptr;
  std::filesystem::path Root;
  std::string Path, Revision, Artifact;
  void SetUp() override {
    llvm::SmallString<128> D;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-readable-sdk", D));
    Root = D.str().str();
    Path = (Root / "PRIVATE_CANARY.js").string();
    S = neverd_web_session_create();
    ASSERT_NE(S, nullptr);
  }
  void TearDown() override {
    neverd_web_session_destroy(S);
    std::error_code EC;
    std::filesystem::remove_all(Root, EC);
  }
  bool has(std::string_view Op) {
    const auto C = take(neverd_web_capabilities_json());
    for (const auto &Name : *C.getAsObject()->getArray("operations"))
      if (Name.getAsString().value_or("") ==
          llvm::StringRef(Op.data(), Op.size()))
        return true;
    return false;
  }
  void open(const std::string &Selected) {
    const auto P = take(neverd_web_import_preview_json(
        S, Selected.data(), Selected.size(), nullptr, 0));
    const auto Token = field(P, "preview_token");
    Revision = field(
        take(neverd_web_import_commit_json(S, Token.data(), Token.size())),
        "revision");
    const auto Page = take(
        neverd_web_artifacts_json(S, Revision.data(), Revision.size(), 0, 1));
    Artifact =
        field((*Page.getAsObject()->getArray("items"))[0], "artifact_id");
  }
  void capture(std::string_view Text) {
    {
      std::ofstream F(Path, std::ios::binary);
      F.write(Text.data(), Text.size());
      ASSERT_TRUE(F.good());
    }
    open(Path);
  }
  Value exportSource(const std::filesystem::path &Destination,
                     std::string_view Type = "module",
                     std::string_view Selected = {},
                     std::string_view Expected = {}) {
    if (Selected.empty())
      Selected = Artifact;
    if (Expected.empty())
      Expected = Revision;
    const auto Directory = Destination.string();
    return take(neverd_web_source_readable_export_json(
        S, Expected.data(), Expected.size(), Selected.data(), Selected.size(),
        Type.data(), Type.size(), Directory.data(), Directory.size()));
  }
  void expectNoInteractiveSource() {
    const auto M = take(neverd_web_metadata_json(S));
    EXPECT_EQ(M.getAsObject()->getInteger("source_inventory_count"), 0);
    EXPECT_EQ(M.getAsObject()->getInteger("source_binding_analysis_count"), 0);
  }
};
#if NEVERD_TEST_WEB_JAVASCRIPT
TEST_F(WebReadableExportSDK, ExactOriginalAndSameTreeSurviveInputDeletion) {
  ASSERT_TRUE(has("source_readable_export"));
  const std::string Text =
      "// LICENSE CANARY stays\nfunction f(x){if(x){return /[{};]/.test("
      "`CANARY${x}`)}return\n{x:1}};";
  capture(Text);
  std::filesystem::remove(Path);
  const auto Destination = Root / "recovered";
  const auto R = exportSource(Destination);
  ASSERT_EQ(field(R, "status"), "ok");
  EXPECT_EQ(field(R, "parse_status"), "parsed");
  EXPECT_EQ(field(R, "readable_status"), "verified_same_parser_tree");
  EXPECT_EQ(R.getAsObject()->getInteger("file_count"), 3);
  EXPECT_EQ(R.getAsObject()->getBoolean("executes_input"), false);
  EXPECT_EQ(R.getAsObject()->getBoolean("interactive_cache_modified"), false);
  EXPECT_EQ(read(Destination / "original.bin"), Text);
  const auto Readable = read(Destination / "readable.js");
  EXPECT_EQ(hash(Readable), field(R, "readable_sha256"));
  EXPECT_NE(Readable.find("// LICENSE CANARY stays"), std::string::npos);
  const auto ManifestText = read(Destination / "manifest.json");
  EXPECT_EQ(hash(ManifestText), field(R, "manifest_sha256"));
  auto Manifest = llvm::json::parse(ManifestText);
  ASSERT_TRUE(bool(Manifest));
  EXPECT_EQ(field(*Manifest, "original_sha256"), hash(Text));
  EXPECT_EQ(field(*Manifest, "source_type_basis"), "explicit_caller_selection");
  expectNoInteractiveSource();
  EXPECT_EQ(code(exportSource(Destination)), "export_destination_exists");
  const auto Link = Root / "link";
  std::filesystem::create_directory_symlink(Destination, Link);
  EXPECT_EQ(code(exportSource(Link)), "export_destination_exists");
  EXPECT_EQ(read(Destination / "original.bin"), Text);
}
TEST_F(WebReadableExportSDK, ParseRefusalsPreserveBytesWithoutReadableFiles) {
  for (const auto &Text :
       {std::string("const CANARY = ;"), std::string("CANARY\0", 7)}) {
    capture(Text);
    const auto Destination = Root / (Text.back() ? "syntax" : "encoding");
    const auto R = exportSource(Destination);
    ASSERT_EQ(field(R, "status"), "ok");
    EXPECT_EQ(field(R, "readable_status"),
              Text.back() ? "invalid_syntax" : "unsupported_encoding");
    EXPECT_EQ(R.getAsObject()->getInteger("file_count"), 2);
    EXPECT_TRUE(R.getAsObject()->get("readable_sha256")->getAsNull());
    EXPECT_FALSE(std::filesystem::exists(Destination / "readable.js"));
    EXPECT_EQ(read(Destination / "original.bin"), Text);
    expectNoInteractiveSource();
  }
  capture("return 'CANARY';");
  EXPECT_EQ(field(exportSource(Root / "module"), "readable_status"),
            "invalid_syntax");
  EXPECT_EQ(
      field(exportSource(Root / "commonjs", "commonjs"), "readable_status"),
      "verified_same_parser_tree");
}
TEST_F(WebReadableExportSDK,
       LargeChunkedSelectionDoesNotRaiseInteractiveLimits) {
  const std::string Text =
      "const CANARY='" + std::string(9 * 1024 * 1024, 'x') + "';";
  capture(Text);
  const auto I = take(neverd_web_source_analyze_json(
      S, Revision.data(), Revision.size(), Artifact.data(), Artifact.size(),
      "module", 6));
  EXPECT_EQ(code(I), "source_byte_budget_exceeded");
  const auto Destination = Root / "large";
  const auto R = exportSource(Destination);
  ASSERT_EQ(field(R, "readable_status"), "verified_same_parser_tree");
  EXPECT_EQ(field(R, "original_sha256"), hash(Text));
  EXPECT_EQ(read(Destination / "original.bin"), Text);
  EXPECT_EQ(hash(read(Destination / "readable.js")),
            field(R, "readable_sha256"));
  expectNoInteractiveSource();
}
TEST_F(WebReadableExportSDK, UnknownStaleGoalAndOversizeRefuseBeforeWrites) {
  capture("const CANARY=42;");
  const auto Destination = Root / "refused";
  EXPECT_EQ(code(exportSource(Destination, "typescript")),
            "unsupported_source_type");
  EXPECT_EQ(code(exportSource(Destination, "module", std::string(64, '0'))),
            "readable_source_selection_unavailable");
  const auto Old = Revision, OldArtifact = Artifact;
  capture("const CANARY=43;");
  EXPECT_EQ(code(exportSource(Destination, "module", OldArtifact, Old)),
            "stale_revision");
  capture(std::string(32 * 1024 * 1024 + 1, ' '));
  EXPECT_EQ(code(exportSource(Destination)), "source_byte_budget_exceeded");
  EXPECT_FALSE(std::filesystem::exists(Destination));
  expectNoInteractiveSource();
}
TEST_F(WebReadableExportSDK, CompressedWailsMemberRetainsExpandedOrigin) {
  if (!has("wails_extract"))
    GTEST_SKIP() << "pinned native path policy unavailable";
  using namespace neverd::web::test;
  capture(storedGzip(
      finishTar(tarMember("package/wails", neverd::web::wails_test::elf()))));
  const auto A = take(neverd_web_package_archive_extract_json(
      S, Revision.data(), Revision.size(), Artifact.data(), Artifact.size(),
      "tgz", 3));
  const auto AID = field(A, "archive_id");
  const auto M = take(neverd_web_package_archive_records_json(
      S, Revision.data(), Revision.size(), AID.data(), AID.size(), 0, 1));
  const auto MID = field((*M.getAsObject()->getArray("items"))[0], "member_id");
  const auto Profile = neverd::web::WailsProfile;
  const auto E = take(neverd_web_wails_extract_json(
      S, Revision.data(), Revision.size(), MID.data(), MID.size(),
      Profile.data(), Profile.size()));
  const auto ID = field(E, "extraction_id");
  const auto Members = take(
      neverd_web_wails_records_json(S, Revision.data(), Revision.size(),
                                    ID.data(), ID.size(), "members", 7, 1, 1));
  const auto JS =
      field((*Members.getAsObject()->getArray("items"))[0], "selection_id");
  const auto Destination = Root / "nested";
  const auto R = exportSource(Destination, "module", JS);
  ASSERT_EQ(field(R, "readable_status"), "verified_same_parser_tree");
  const auto *Origin = R.getAsObject()->getObject("original_origin");
  ASSERT_NE(Origin, nullptr);
  EXPECT_EQ(Origin->getString("kind"), "wails_embed_member");
  EXPECT_EQ(Origin->getString("storage_artifact_id"), Artifact);
  EXPECT_TRUE(Origin->get("byte_offset")->getAsNull());
  EXPECT_EQ(Origin->getString("byte_offset_basis"), "expanded_stream");
  EXPECT_EQ(Origin->getString("expanded_byte_offset"),
            std::to_string(512 + 0x1400));
  EXPECT_EQ(read(Destination / "original.bin"),
            neverd::web::wails_test::entries()[1].Contents);
  expectNoInteractiveSource();
}
TEST_F(WebReadableExportSDK, Zentro030WholeBundleWhenSupplied) {
  const auto *Selected = std::getenv("NEVERD_WAILS_ZENTRO_030_ELF");
  if (!Selected || !*Selected)
    GTEST_SKIP() << "captured Zentro v0.3.0 release required";
  if (!has("wails_extract"))
    GTEST_SKIP() << "pinned native path policy unavailable";
  open(Selected);
  const auto Profile = neverd::web::WailsProfile;
  const auto E = take(neverd_web_wails_extract_json(
      S, Revision.data(), Revision.size(), Artifact.data(), Artifact.size(),
      Profile.data(), Profile.size()));
  const auto ID = field(E, "extraction_id");
  const auto M = take(
      neverd_web_wails_records_json(S, Revision.data(), Revision.size(),
                                    ID.data(), ID.size(), "members", 7, 7, 1));
  const auto JS =
      field((*M.getAsObject()->getArray("items"))[0], "selection_id");
  const auto Destination = Root / "zentro";
  const auto R = exportSource(Destination, "module", JS);
  ASSERT_EQ(field(R, "readable_status"), "verified_same_parser_tree");
  EXPECT_EQ(field(R, "original_bytes"), "1552742");
  EXPECT_EQ(field(R, "original_sha256"),
            "550cdaae8e65f0cbd2a75132a0dc5073fc8a7883811d0d7abdd6a59f71bd0e1e");
  EXPECT_EQ(R.getAsObject()->getInteger("syntax_nodes"), 449790);
  EXPECT_EQ(field(R, "readable_bytes"), "1897987");
  EXPECT_EQ(field(R, "readable_sha256"),
            "962afafa77d08adbc53e4f721456773a4bbaa5b16e86c7317bc1c6f1812dd8f7");
  EXPECT_EQ(hash(read(Destination / "original.bin")),
            field(R, "original_sha256"));
  EXPECT_EQ(hash(read(Destination / "readable.js")),
            field(R, "readable_sha256"));
  EXPECT_EQ(
      R.getAsObject()->getObject("original_origin")->getString("byte_offset"),
      "16727680");
  expectNoInteractiveSource();
}
#ifdef NEVERD_WEB_TEST_CLI
TEST_F(WebReadableExportSDK, CLIUsesOnlyNeverDWithAnUnusableExternalToolPath) {
  capture("function CANARY(x){if(x){return x+1}return 0}");
  const auto Destination = (Root / "cli").string(),
             Out = (Root / "out").string(), Err = (Root / "err").string();
  const llvm::StringRef Args[]{
      NEVERD_WEB_TEST_CLI, "web", "readable-export", Path, "module",
      Destination};
  const llvm::StringRef Env[]{"PATH=/neverd-no-external-tools", "LC_ALL=C"};
  const std::optional<llvm::StringRef> Redirects[]{std::nullopt, Out, Err};
  ASSERT_EQ(
      llvm::sys::ExecuteAndWait(NEVERD_WEB_TEST_CLI, Args, Env, Redirects, 30),
      0);
  EXPECT_EQ(read(Out).find("CANARY"), std::string::npos);
  EXPECT_EQ(read(Err).find("CANARY"), std::string::npos);
  auto R = llvm::json::parse(read(Out));
  ASSERT_TRUE(bool(R));
  EXPECT_EQ(field(*R, "readable_status"), "verified_same_parser_tree");
  EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(Destination) /
                                      "readable.js"));
}
#endif
#else
TEST_F(WebReadableExportSDK, OmittedParserRefusesBeforeLocalWrites) {
  EXPECT_FALSE(has("source_readable_export"));
  capture("const CANARY=42;");
  const auto Destination = Root / "unavailable";
  EXPECT_EQ(code(exportSource(Destination)), "capability_unavailable");
  EXPECT_FALSE(std::filesystem::exists(Destination));
  expectNoInteractiveSource();
}
#endif
} // namespace
