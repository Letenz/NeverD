//===- NodeBehaviorSDKTests.cpp - Node evidence publication contracts ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Verify selection, privacy, revision, container joins and offline CLI parity.
///
//===----------------------------------------------------------------------===//

#include "BunFixture.h"
#include "PackageArchiveFixture.h"
#include "SEAFixture.h"
#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPIWeb.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Program.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>

namespace {
using Value = llvm::json::Value;
using namespace neverd::web::test;
Value parse(std::string_view Text) {
  EXPECT_EQ(Text.find("CANARY"), std::string_view::npos);
  auto V = llvm::json::parse(Text);
  if (!V) {
    llvm::consumeError(V.takeError());
    throw std::runtime_error("invalid response");
  }
  return std::move(*V);
}
Value take(const char *Owned) {
  if (!Owned)
    throw std::runtime_error("missing response");
  const std::string Text(Owned);
  neverd_free_string(Owned);
  return parse(Text);
}
std::string field(const Value &V, const char *Key) {
  return V.getAsObject()->getString(Key).value_or("").str();
}
std::string code(const Value &V) {
  EXPECT_EQ(field(V, "status"), "error");
  const auto *E = V.getAsObject()->getObject("error");
  return E ? E->getString("code").value_or("").str() : "";
}
class WebNodeBehaviorSDK : public ::testing::Test {
protected:
  static constexpr std::string_view Script =
      "const fs=require('node:fs');"
      "fs.writeFileSync('CANARY_PATH','CANARY_DATA');";
  neverd_web_session_t S = nullptr;
  std::filesystem::path Root, Input;
  std::string Revision;
  std::vector<std::string> Artifacts;
  void SetUp() override {
    if (!NEVERD_TEST_WEB_JAVASCRIPT)
      GTEST_SKIP() << "JavaScript parser omitted";
    llvm::SmallString<128> Directory;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-node-sdk", Directory));
    Root = Directory.str().str();
    Input = Root / "input";
    std::filesystem::create_directory(Input);
    for (unsigned I = 0; I < 16; ++I) {
      const auto Name =
          "a" + (I < 10 ? std::string("0") : "") + std::to_string(I) + ".js";
      std::ofstream(Input / Name) << Script << "/* " << I << " */";
    }
    llvm::json::Object Bins;
    for (unsigned I = 0; I < 18; ++I)
      Bins["CANARY_BIN_" + std::to_string(I)] = "a00.js";
    std::ofstream(Input / "package.json")
        << llvm::formatv("{0}",
                         Value(llvm::json::Object{{"name", "CANARY_PACKAGE"},
                                                  {"main", "a00.js"},
                                                  {"bin", std::move(Bins)}}))
               .str();
    S = neverd_web_session_create();
    ASSERT_NE(S, nullptr);
    open(Input);
    ASSERT_EQ(Artifacts.size(), 18u);
  }
  void TearDown() override {
    neverd_web_session_destroy(S);
    if (!Root.empty()) {
      std::error_code EC;
      std::filesystem::remove_all(Root, EC);
    }
  }
  void open(const std::filesystem::path &Path) {
    const auto Text = Path.string();
    const auto P = take(neverd_web_import_preview_json(
        S, Text.data(), Text.size(), nullptr, 0));
    const auto Token = field(P, "preview_token");
    Revision = field(
        take(neverd_web_import_commit_json(S, Token.data(), Token.size())),
        "revision");
    auto Page = take(
        neverd_web_artifacts_json(S, Revision.data(), Revision.size(), 0, 128));
    Artifacts.clear();
    for (const auto &I : *Page.getAsObject()->getArray("items"))
      Artifacts.push_back(field(I, "artifact_id"));
  }
  std::string source(std::string_view Artifact) {
    return field(take(neverd_web_source_analyze_json(
                     S, Revision.data(), Revision.size(), Artifact.data(),
                     Artifact.size(), "commonjs", 8)),
                 "source_id");
  }
  std::string packages(std::string_view Artifact,
                       std::string_view Kind = "package-json") {
    return field(take(neverd_web_packages_analyze_json(
                     S, Revision.data(), Revision.size(), Artifact.data(),
                     Artifact.size(), Kind.data(), Kind.size())),
                 "package_analysis_id");
  }
  Value analyze(std::string_view Source) {
    return take(neverd_web_node_behavior_analyze_json(
        S, Revision.data(), Revision.size(), Source.data(), Source.size()));
  }
  Value records(std::string_view ID, uint64_t Offset = 0,
                uint64_t Limit = 128) {
    return take(neverd_web_node_behavior_records_json(
        S, Revision.data(), Revision.size(), ID.data(), ID.size(), Offset,
        Limit));
  }
  Value aggregate(std::string_view Package, std::vector<std::string> Sources) {
    llvm::json::Array IDs;
    for (const auto &ID : Sources)
      IDs.push_back(ID);
    const auto Options =
        llvm::formatv("{0}",
                      Value(llvm::json::Object{{"schema_version", 1},
                                               {"source_ids", std::move(IDs)}}))
            .str();
    return take(neverd_web_package_behavior_analyze_json(
        S, Revision.data(), Revision.size(), Package.data(), Package.size(),
        Options.data(), Options.size()));
  }
  Value rows(std::string_view ID, std::string_view Kind = "sources",
             uint64_t Offset = 0, uint64_t Limit = 128) {
    return take(neverd_web_package_behavior_records_json(
        S, Revision.data(), Revision.size(), ID.data(), ID.size(), Kind.data(),
        Kind.size(), Offset, Limit));
  }
  // Archive members need the pinned ICU path policy, as in
  // PackageArchiveSDKTests; without it extraction reports unavailability.
  static bool packageArchivesAvailable() {
    const auto Caps = take(neverd_web_capabilities_json());
    for (const auto &A : *Caps.getAsObject()->getArray("analysis"))
      if (field(A, "kind") == "package_archive")
        return A.getAsObject()->getBoolean("available").value_or(false);
    return false;
  }
  Value install(std::string_view Package, std::string_view Behavior = {}) {
    llvm::json::Object Options{{"schema_version", 1}};
    if (!Behavior.empty())
      Options["package_behavior_id"] = std::string(Behavior);
    const auto Text = llvm::formatv("{0}", Value(std::move(Options))).str();
    return take(neverd_web_package_install_analyze_json(
        S, Revision.data(), Revision.size(), Package.data(), Package.size(),
        Text.data(), Text.size()));
  }
  Value installRows(std::string_view ID, std::string_view Kind,
                    uint64_t Offset = 0, uint64_t Limit = 128) {
    return take(neverd_web_package_install_records_json(
        S, Revision.data(), Revision.size(), ID.data(), ID.size(), Kind.data(),
        Kind.size(), Offset, Limit));
  }
  static std::string read(const std::filesystem::path &Path) {
    std::ifstream F(Path, std::ios::binary);
    return {std::istreambuf_iterator<char>(F), {}};
  }
  std::vector<Value> run(std::vector<std::string> Args, int Expected) {
#ifdef NEVERD_WEB_TEST_CLI
    Args.insert(Args.begin(), {NEVERD_WEB_TEST_CLI, "web"});
    std::vector<llvm::StringRef> Refs;
    for (const auto &A : Args)
      Refs.emplace_back(A);
    const auto Out = (Root / "stdout").string(),
               Err = (Root / "stderr").string();
    std::array<std::optional<llvm::StringRef>, 3> Redirects{std::nullopt, Out,
                                                            Err};
    std::array<llvm::StringRef, 1> Env{"PATH=/neverd-no-external-tools"};
    std::ofstream(Out, std::ios::trunc).close();
    std::ofstream(Err, std::ios::trunc).close();
    EXPECT_EQ(llvm::sys::ExecuteAndWait(NEVERD_WEB_TEST_CLI, Refs,
                                        llvm::ArrayRef(Env), Redirects, 60),
              Expected)
        << read(Err);
    EXPECT_EQ(read(Err).find("CANARY"), std::string::npos);
    std::vector<Value> Results;
    std::ifstream Lines(Out);
    for (std::string Line; std::getline(Lines, Line);)
      if (!Line.empty())
        Results.push_back(parse(Line));
    return Results;
#else
    return {};
#endif
  }
};

TEST_F(WebNodeBehaviorSDK, InstallTraceOptionalSelectionPrivacyAndCache) {
  std::ofstream(Input / "package.json")
      << R"({"name":"CANARY_PACKAGE","scripts":{
    "install":"node a00.js CANARY_ARG","preinstall":"node CANARY_MISSING.js",
    "postinstall":"node inspect a00.js"}})";
  open(Input);
  const auto P = packages(Artifacts[17]);
  const auto Empty = install(P);
  ASSERT_EQ(field(Empty, "status"), "ok");
  EXPECT_EQ(Empty.getAsObject()->getInteger("script_count"), 3);
  EXPECT_EQ(Empty.getAsObject()->getInteger("source_count"), 0);
  const auto EmptyID = field(Empty, "package_install_id");
  const auto Decl = installRows(EmptyID, "scripts");
  const auto &Items = *Decl.getAsObject()->getArray("items");
  ASSERT_EQ(Items.size(), 3u);
  EXPECT_EQ(field(Items[0], "status"), "entry_source_unselected");
  EXPECT_EQ(field(Items[1], "status"), "node_option_or_subcommand_boundary");
  EXPECT_EQ(field(Items[2], "status"), "not_captured_or_unavailable");
  const auto B = aggregate(P, {source(Artifacts[1])});
  const auto BID = field(B, "package_behavior_id");
  const auto A = install(P, BID);
  const auto ID = field(A, "package_install_id");
  ASSERT_FALSE(ID.empty());
  EXPECT_NE(ID, EmptyID);
  EXPECT_EQ(A.getAsObject()->getInteger("association_count"), 1);
  for (const auto *Key :
       {"runtime_resolution_verified", "reachability_verified",
        "executable_identity_verified", "selection_complete",
        "lifecycle_activation_verified"})
    EXPECT_EQ(A.getAsObject()->getBoolean(Key), false);
  for (const auto *Kind : {"scripts", "sources", "requests", "associations"})
    EXPECT_EQ(field(installRows(ID, Kind), "status"), "ok");
  const auto Associations = installRows(ID, "associations");
  ASSERT_EQ(Associations.getAsObject()->getArray("items")->size(), 1u);
  const auto &Call = (*Associations.getAsObject()->getArray("items"))[0];
  // Package behavior uses a temporary model. This report must remain useful
  // when the independently queried Node behavior cache is empty.
  const auto NodeID = field(Call, "node_behavior_id");
  EXPECT_EQ(code(take(neverd_web_node_behavior_records_json(
                S, Revision.data(), Revision.size(), NodeID.data(),
                NodeID.size(), 0, 1))),
            "unknown_node_behavior");
  const auto OriginalPage = rows(BID, "records");
  ASSERT_EQ(OriginalPage.getAsObject()->getArray("items")->size(), 1u);
  const auto &Original = (*OriginalPage.getAsObject()->getArray("items"))[0];
  EXPECT_EQ(field(Call, "behavior_record_id"), field(Original, "record_id"));
  for (const auto *Key : {"node_id", "api_class", "kind", "origin_evidence",
                          "byte_offset", "byte_length"})
    EXPECT_EQ(field(Call, Key), field(Original, Key));
  EXPECT_EQ(field(install(P, BID), "package_install_id"), ID);
  EXPECT_EQ(code(installRows(ID, "paths")), "invalid_record_kind");
  EXPECT_EQ(code(installRows(ID, "scripts", 0, 513)), "invalid_page");
  const auto B2 = aggregate(P, {source(Artifacts[2])});
  EXPECT_EQ(code(install(P, field(B2, "package_behavior_id"))),
            "package_install_cache_budget_exceeded");
  const std::string Bad = R"({"schema_version":1,"execute":true})";
  EXPECT_EQ(code(take(neverd_web_package_install_analyze_json(
                S, Revision.data(), Revision.size(), P.data(), P.size(),
                Bad.data(), Bad.size()))),
            "invalid_package_install_options");
  const auto OldRevision = Revision;
  open(Input);
  EXPECT_EQ(code(take(neverd_web_package_install_records_json(
                S, OldRevision.data(), OldRevision.size(), ID.data(), ID.size(),
                "scripts", 7, 0, 1))),
            "stale_revision");
  EXPECT_EQ(code(installRows(ID, "scripts")), "unknown_package_install");
  const auto CLI = run(
      {"package-install", Input.string(), "17", "package-json", "1:commonjs"},
      0);
  ASSERT_EQ(CLI.size(), 5u);
  EXPECT_EQ(CLI[0].getAsObject()->getInteger("association_count"), 1);
  EXPECT_EQ(CLI[0].getAsObject()->getInteger("script_count"), 3);
}

TEST_F(WebNodeBehaviorSDK,
       ManifestAndLockOnlyInstallCoverageNeedsNoFakeSource) {
  const auto Manifest = Input / "package.json";
  std::ofstream(Manifest)
      << R"({"scripts":{"install":"node CANARY_ABSENT.js"}})";
  open(Manifest);
  auto P = packages(Artifacts[0]);
  auto A = install(P);
  auto Page = installRows(field(A, "package_install_id"), "scripts");
  EXPECT_EQ(field((*Page.getAsObject()->getArray("items"))[0], "status"),
            "no_directory_inventory");
  auto CLI =
      run({"package-install", Manifest.string(), "0", "package-json"}, 0);
  ASSERT_EQ(CLI.size(), 5u);
  EXPECT_EQ(CLI[0].getAsObject()->getInteger("source_count"), 0);
  const auto Lock = Root / "lock.json";
  std::ofstream(Lock) << R"({"lockfileVersion":3,"packages":{
    "":{"hasInstallScript":true}}})";
  open(Lock);
  P = packages(Artifacts[0], "npm-lock");
  A = install(P);
  EXPECT_EQ(A.getAsObject()->getInteger("install_presence_count"), 1);
  Page = installRows(field(A, "package_install_id"), "scripts");
  const auto &Row = (*Page.getAsObject()->getArray("items"))[0];
  EXPECT_EQ(field(Row, "status"), "manifest_not_supplied");
  EXPECT_TRUE(Row.getAsObject()->get("script_id")->getAsNull());
  CLI = run({"package-install", Lock.string(), "0", "npm-lock"}, 0);
  ASSERT_EQ(CLI.size(), 5u);
  EXPECT_EQ(CLI[0].getAsObject()->getInteger("install_presence_count"), 1);
  if (!packageArchivesAvailable())
    GTEST_SKIP() << "Pinned archive path policy unavailable";
  const auto Archive = Root / "inert.tar";
  std::ofstream(Archive, std::ios::binary)
      << finishTar(tarMember("package/package.json",
                             R"({"scripts":{"install":"node index.js"}})") +
                   tarMember("package/index.js", Script));
  CLI = run({"archive-package-install", Archive.string(), "tar", "0", "0",
             "package-json", "1:commonjs"},
            0);
  ASSERT_EQ(CLI.size(), 5u);
  EXPECT_EQ(CLI[0].getAsObject()->getInteger("association_count"), 1);
}

TEST_F(WebNodeBehaviorSDK,
       InstallAssociationPagingUsesActualIndependentCounts) {
  std::ofstream(Input / "package.json") << R"({"scripts":{
    "install":"node a00.js","postinstall":"node a00.js"}})";
  std::ofstream Source(Input / "a00.js");
  for (unsigned I = 0; I < 300; ++I)
    Source << "require('node:fs').writeFileSync('CANARY_PATH','CANARY_DATA');";
  Source.close();
  open(Input);
  const auto P = packages(Artifacts[17]);
  const auto B = aggregate(P, {source(Artifacts[1])});
  ASSERT_EQ(B.getAsObject()->getInteger("record_count"), 300);
  const auto A = install(P, field(B, "package_behavior_id"));
  ASSERT_EQ(A.getAsObject()->getInteger("association_count"), 600);
  const auto ID = field(A, "package_install_id");
  const auto First = installRows(ID, "associations", 0, 512);
  EXPECT_EQ(First.getAsObject()->getInteger("next_offset"), 512);
  EXPECT_EQ(First.getAsObject()->getArray("items")->size(), 512u);
  const auto Last = installRows(ID, "associations", 512, 512);
  EXPECT_EQ(Last.getAsObject()->getArray("items")->size(), 88u);
  EXPECT_EQ(Last.getAsObject()->getBoolean("page_complete"), true);
  EXPECT_TRUE(installRows(ID, "associations", 600)
                  .getAsObject()
                  ->getArray("items")
                  ->empty());
  EXPECT_EQ(code(installRows(ID, "associations", 601)), "invalid_page");
}

TEST_F(WebNodeBehaviorSDK,
       InstallBudgetFailurePublishesNoResultOrCachedChains) {
  llvm::json::Object Scripts;
  for (const auto *Name :
       {"preinstall", "install", "postinstall", "prepare", "prepublish",
        "prepublishOnly", "prepack", "postpack", "publish"})
    Scripts[Name] = "node a00.js";
  std::ofstream(Input / "package.json")
      << llvm::formatv(
             "{0}", Value(llvm::json::Object{{"scripts", std::move(Scripts)}}))
             .str();
  std::ofstream Source(Input / "a00.js");
  for (unsigned I = 0; I < 1000; ++I)
    Source << "require('node:fs').writeFileSync('CANARY_PATH','CANARY_DATA');";
  Source.close();
  open(Input);
  const auto P = packages(Artifacts[17]);
  const auto B = aggregate(P, {source(Artifacts[1])});
  ASSERT_EQ(B.getAsObject()->getInteger("record_count"), 1000);
  const auto A = install(P, field(B, "package_behavior_id"));
  EXPECT_EQ(code(A), "package_install_association_budget_exceeded");
  const auto Meta = take(neverd_web_metadata_json(S));
  EXPECT_EQ(Meta.getAsObject()->getInteger("package_install_count"), 0);
  EXPECT_EQ(field(install(P), "status"), "ok");
}

TEST_F(WebNodeBehaviorSDK,
       MetadataAndPagingExposeOnlyFixedVocabularyAndAnchors) {
  const auto Source = source(Artifacts[1]);
  const auto A = analyze(Source);
  ASSERT_EQ(field(A, "analysis_status"), "partial");
  EXPECT_EQ(A.getAsObject()->getInteger("record_count"), 1);
  EXPECT_EQ(A.getAsObject()->getBoolean("runtime_api_verified"), false);
  EXPECT_EQ(field(A, "safety_verdict"), "not_assessed");
  const auto ID = field(A, "node_behavior_id");
  const auto Page = records(ID, 0, 1);
  const auto *Items = Page.getAsObject()->getArray("items");
  ASSERT_NE(Items, nullptr);
  ASSERT_EQ(Items->size(), 1u);
  EXPECT_EQ(field((*Items)[0], "api_class"), "fs.writeFileSync");
  EXPECT_EQ(field((*Items)[0], "parameter_roles"), "not_resolved");
  EXPECT_EQ((*Items)[0].getAsObject()->getArray("argument_anchors")->size(),
            2u);
  EXPECT_TRUE(records(ID, 1).getAsObject()->getArray("items")->empty());
  for (auto Bad : {records(ID, 2), records(ID, 0, 0), records(ID, 0, 513)})
    EXPECT_EQ(code(Bad), "invalid_page");
  EXPECT_EQ(code(records(std::string(64, '0'))), "unknown_node_behavior");
}

TEST_F(WebNodeBehaviorSDK,
       SixteenSourceSelectionDoesNotDependOnFourEntryCache) {
  std::vector<std::string> Sources;
  for (unsigned I = 1; I <= 16; ++I)
    Sources.push_back(source(Artifacts[I]));
  for (unsigned I = 0; I < 4; ++I)
    ASSERT_EQ(field(analyze(Sources[I]), "status"), "ok");
  EXPECT_EQ(code(analyze(Sources[4])), "node_behavior_cache_budget_exceeded");
  const auto P = packages(Artifacts[17]);
  const auto A = aggregate(P, Sources);
  ASSERT_EQ(field(A, "status"), "ok");
  EXPECT_EQ(A.getAsObject()->getInteger("source_count"), 16);
  EXPECT_EQ(A.getAsObject()->getInteger("record_count"), 16);
  EXPECT_EQ(A.getAsObject()->getBoolean("selection_complete"), false);
  const auto ID = field(A, "package_behavior_id");
  std::reverse(Sources.begin(), Sources.end());
  EXPECT_EQ(field(aggregate(P, Sources), "package_behavior_id"), ID);
  auto Page = rows(ID, "records", 1, 2);
  ASSERT_EQ(Page.getAsObject()->getArray("items")->size(), 2u);
  EXPECT_EQ(Page.getAsObject()->getInteger("next_offset"), 3);
  Page = rows(ID);
  bool FoundEntry = false;
  for (const auto &Row : *Page.getAsObject()->getArray("items")) {
    EXPECT_EQ(field(Row, "relation"), "direct_file");
    EXPECT_EQ(field(Row, "package_ownership"), "inventory_recorded");
    if (field(Row, "source_id") == Sources.back()) {
      FoundEntry = true;
      EXPECT_EQ(Row.getAsObject()->getInteger("declared_entry_count"), 19);
      EXPECT_EQ(Row.getAsObject()->getArray("declared_entry_ids")->size(), 16u);
      EXPECT_EQ(Row.getAsObject()->getBoolean("declared_entry_ids_truncated"),
                true);
    }
  }
  EXPECT_TRUE(FoundEntry);
  EXPECT_EQ(code(aggregate(P, {Sources[0], Sources[0]})),
            "duplicate_package_behavior_source");
  ASSERT_EQ(field(aggregate(P, {Sources[0]}), "status"), "ok");
  EXPECT_EQ(code(aggregate(P, {Sources[1]})),
            "package_behavior_cache_budget_exceeded");
  EXPECT_EQ(code(rows(ID, "wrong")), "invalid_record_kind");
  EXPECT_EQ(code(rows(ID, "records", 0, 513)), "invalid_page");
  EXPECT_EQ(code(rows(ID, "sources", 17)), "invalid_page");
  const auto OldRevision = Revision;
  open(Input);
  EXPECT_EQ(code(take(neverd_web_package_behavior_records_json(
                S, OldRevision.data(), OldRevision.size(), ID.data(), ID.size(),
                "sources", 7, 0, 1))),
            "stale_revision");
  EXPECT_EQ(code(rows(ID)), "unknown_package_behavior");
  EXPECT_EQ(code(analyze(Sources[0])), "unknown_source");
}

TEST_F(WebNodeBehaviorSDK, UnavailableAndUnownedSourcesRemainVisible) {
  if (!packageArchivesAvailable())
    GTEST_SKIP() << "Pinned archive path policy unavailable";
  const auto Archive = Root / "unknown.tar";
  std::ofstream(Archive, std::ios::binary) << finishTar(
      tarMember("package/package-lock.json",
                R"({"lockfileVersion":3,"packages":{"":{"name":"CANARY"}}})") +
      tarMember("package/node_modules/unrecorded/index.js", Script) +
      tarMember("package/broken.js", "const =") +
      tarMember("package/ordinary.js", "function f(x){return x;}"));
  open(Archive);
  const auto A = take(neverd_web_package_archive_extract_json(
      S, Revision.data(), Revision.size(), Artifacts[0].data(),
      Artifacts[0].size(), "tar", 3));
  ASSERT_EQ(field(A, "status"), "ok") << llvm::formatv("{0}", A).str();
  const auto ArchiveID = field(A, "archive_id");
  const auto Page = take(neverd_web_package_archive_records_json(
      S, Revision.data(), Revision.size(), ArchiveID.data(), ArchiveID.size(),
      0, 128));
  ASSERT_EQ(field(Page, "status"), "ok") << llvm::formatv("{0}", Page).str();
  const auto &Items = *Page.getAsObject()->getArray("items");
  ASSERT_GE(Items.size(), 4u);
  const auto P = packages(field(Items[0], "member_id"), "npm-lock");
  std::vector<std::string> Sources;
  for (unsigned I = 1; I <= 3; ++I)
    Sources.push_back(source(field(Items[I], "member_id")));
  const auto B = aggregate(P, Sources);
  ASSERT_EQ(field(B, "status"), "ok");
  EXPECT_EQ(B.getAsObject()->getInteger("source_count"), 3);
  EXPECT_EQ(B.getAsObject()->getInteger("unavailable_source_count"), 1);
  EXPECT_EQ(B.getAsObject()->getInteger("record_count"), 1);
  const auto Rows = rows(field(B, "package_behavior_id"));
  for (const auto &R : *Rows.getAsObject()->getArray("items")) {
    if (field(R, "source_id") == Sources[0]) {
      EXPECT_EQ(field(R, "package_ownership"), "ownership_unrecorded");
      EXPECT_EQ(*R.getAsObject()->get("package_id"), Value(nullptr));
    }
    if (field(R, "source_id") == Sources[1])
      EXPECT_EQ(field(R, "analysis_status"), "unavailable");
  }
#ifdef NEVERD_WEB_TEST_CLI
  const auto CLI =
      run({"archive-package-behavior", Archive.string(), "tar", "0", "0",
           "npm-lock", "1:commonjs", "2:commonjs", "3:commonjs"},
          0);
  ASSERT_EQ(CLI.size(), 3u);
  EXPECT_EQ(field(CLI[0], "package_behavior_id"),
            field(B, "package_behavior_id"));
#endif
}

TEST_F(WebNodeBehaviorSDK,
       ExtractedSEAAndBunSourcesKeepDeliveryOnlyAssociation) {
  if (!packageArchivesAvailable())
    GTEST_SKIP() << "Pinned archive path policy unavailable";
  const auto Archive = Root / "native.tar";
  const auto SEABytes =
      neverd::web::sea_test::elf(neverd::web::sea_test::blob(0, Script));
  std::ofstream(Archive, std::ios::binary) << finishTar(
      tarMember("package/package.json",
                R"({"name":"CANARY","bin":{"CANARY":"sea"},"main":"bun"})") +
      tarMember("package/sea", SEABytes) +
      tarMember("package/bun", BunFixture().Bytes) +
      tarMember("other/package.json", R"({"name":"CANARY_OTHER"})") +
      tarMember("other/source.js", Script));
  open(Archive);
  const auto A = take(neverd_web_package_archive_extract_json(
      S, Revision.data(), Revision.size(), Artifacts[0].data(),
      Artifacts[0].size(), "tar", 3));
  ASSERT_EQ(field(A, "status"), "ok") << llvm::formatv("{0}", A).str();
  const auto ArchiveID = field(A, "archive_id");
  const auto Page = take(neverd_web_package_archive_records_json(
      S, Revision.data(), Revision.size(), ArchiveID.data(), ArchiveID.size(),
      0, 128));
  ASSERT_EQ(field(Page, "status"), "ok") << llvm::formatv("{0}", Page).str();
  const auto &Items = *Page.getAsObject()->getArray("items");
  ASSERT_GE(Items.size(), 5u);
  const auto P = packages(field(Items[0], "member_id"));
  const auto SEAArtifact = field(Items[1], "member_id");
  constexpr std::string_view SEAProfile = "node-sea-22.15.0-elf-x64-v1";
  const auto E = take(neverd_web_sea_extract_json(
      S, Revision.data(), Revision.size(), SEAArtifact.data(),
      SEAArtifact.size(), SEAProfile.data(), SEAProfile.size()));
  ASSERT_EQ(field(E, "status"), "ok") << llvm::formatv("{0}", E).str();
  const auto SEA = source(field(E, "source_artifact_id"));
  const auto BunArtifact = field(Items[2], "member_id");
  const auto Bun =
      take(neverd_web_bun_extract_json(S, Revision.data(), Revision.size(),
                                       BunArtifact.data(), BunArtifact.size()));
  const auto BID = field(Bun, "extraction_id");
  const auto Modules = take(
      neverd_web_bun_records_json(S, Revision.data(), Revision.size(),
                                  BID.data(), BID.size(), "modules", 7, 0, 1));
  const auto BS = source(field((*Modules.getAsObject()->getArray("items"))[0],
                               "source_artifact_id"));
  const auto B = aggregate(P, {SEA, BS});
  ASSERT_EQ(field(B, "status"), "ok")
      << llvm::formatv("{0}; SEA={1}; Bun={2}", B, E, Bun).str();
  const auto Rows = rows(field(B, "package_behavior_id"));
  for (const auto &R : *Rows.getAsObject()->getArray("items")) {
    EXPECT_EQ(field(R, "relation"), "embedded_source");
    EXPECT_EQ(field(R, "package_ownership"), "not_inferred");
    EXPECT_EQ(*R.getAsObject()->get("package_id"), Value(nullptr));
    EXPECT_FALSE(field(R, "delivery_package_id").empty());
    EXPECT_EQ(R.getAsObject()->getInteger("declared_entry_count"), 0);
    EXPECT_EQ(R.getAsObject()->getArray("origin_evidence_ids")->size(), 1u);
  }
  const auto Foreign = source(field(Items[4], "member_id"));
  EXPECT_EQ(code(aggregate(P, {Foreign})),
            "package_behavior_source_outside_inventory");
  // A failed foreign selection does not consume the second aggregate cache
  // slot.
  EXPECT_EQ(field(aggregate(P, {SEA}), "status"), "ok");
}

TEST_F(WebNodeBehaviorSDK, CLIUsesSharedEvidenceWithoutExternalRuntime) {
#ifndef NEVERD_WEB_TEST_CLI
  GTEST_SKIP() << "CLI unavailable";
#else
  const auto A = analyze(source(Artifacts[1]));
  const auto Direct = run({"node-behavior", Input.string(), "1:commonjs"}, 0);
  ASSERT_EQ(Direct.size(), 2u);
  EXPECT_EQ(field(Direct[0], "node_behavior_id"), field(A, "node_behavior_id"));
  const auto Package = run({"package-behavior", Input.string(), "17",
                            "package-json", "1:commonjs", "2:commonjs"},
                           0);
  ASSERT_EQ(Package.size(), 3u);
  EXPECT_EQ(Package[0].getAsObject()->getInteger("record_count"), 2);
  run({"node-behavior", Input.string(), "01:commonjs"}, 2);
  const auto Bin = Root / "bun";
  std::ofstream(Bin, std::ios::binary) << BunFixture().Bytes;
  const auto Embedded =
      run({"bun-node-behavior", Bin.string(), "0", "0:module"}, 0);
  ASSERT_EQ(Embedded.size(), 2u);
  EXPECT_EQ(field(Embedded[0], "analysis_status"), "partial");
#endif
}
} // namespace
