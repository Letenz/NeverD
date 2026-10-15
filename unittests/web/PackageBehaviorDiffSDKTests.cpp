//===- PackageBehaviorDiffSDKTests.cpp - Behavior comparison contracts ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Verify original anchors, paging, cache invalidation and unusable-PATH CLI.
///
//===----------------------------------------------------------------------===//

#include "PackageArchiveFixture.h"
#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPIWeb.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Program.h"

#include <array>
#include <filesystem>
#include <fstream>

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
std::string field(const Value &V, const char *Key) {
  return V.getAsObject()->getString(Key).value_or("").str();
}
std::string code(const Value &V) {
  EXPECT_EQ(field(V, "status"), "error");
  return V.getAsObject()->getObject("error")->getString("code")->str();
}
class WebPackageBehaviorDiffSDK : public ::testing::Test {
protected:
  std::filesystem::path Root, Input;
  neverd_web_session_t S = nullptr;
  std::string Revision, Before, After, Base, BB, AB;
  std::vector<std::string> Artifacts;
  static constexpr std::string_view Call =
      "require('fs').writeFileSync('CANARY_PATH','CANARY_DATA');";

  void SetUp() override {
    if (!NEVERD_TEST_WEB_JAVASCRIPT)
      GTEST_SKIP() << "JavaScript parser omitted";
    llvm::SmallString<128> Dir;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-behavior-diff-sdk", Dir));
    Root = Dir.str().str();
    Input = Root / "input";
    for (const auto *Side : {"before", "after"}) {
      std::filesystem::create_directories(Input / Side);
      std::ofstream(Input / Side / "package.json")
          << R"({"name":"CANARY_PACKAGE","scripts":{"install":"CANARY_COMMAND"}})";
      std::ofstream(Input / Side / "a.js") << Call;
    }
    S = neverd_web_session_create();
    ASSERT_NE(S, nullptr);
  }
  void TearDown() override {
    neverd_web_session_destroy(S);
    std::error_code EC;
    if (!Root.empty())
      std::filesystem::remove_all(Root, EC);
  }
  void open(const std::filesystem::path &Path) {
    const auto Text = Path.string();
    const auto P = take(neverd_web_import_preview_json(
        S, Text.data(), Text.size(), nullptr, 0));
    const auto Token = field(P, "preview_token");
    Revision = field(
        take(neverd_web_import_commit_json(S, Token.data(), Token.size())),
        "revision");
    const auto Page = take(
        neverd_web_artifacts_json(S, Revision.data(), Revision.size(), 0, 128));
    Artifacts.clear();
    for (const auto &I : *Page.getAsObject()->getArray("items"))
      Artifacts.push_back(field(I, "artifact_id"));
  }
  std::string package(std::string_view ID) {
    return field(take(neverd_web_packages_analyze_json(
                     S, Revision.data(), Revision.size(), ID.data(), ID.size(),
                     "package-json", 12)),
                 "package_analysis_id");
  }
  Value source(std::string_view ID) {
    return take(neverd_web_source_analyze_json(S, Revision.data(),
                                               Revision.size(), ID.data(),
                                               ID.size(), "commonjs", 8));
  }
  std::string behavior(std::string_view P, std::string_view Artifact) {
    const auto Source = field(source(Artifact), "source_id");
    const auto Options =
        llvm::formatv("{0}", Value(llvm::json::Object{
                                 {"schema_version", 1},
                                 {"source_ids", llvm::json::Array{Source}}}))
            .str();
    return field(take(neverd_web_package_behavior_analyze_json(
                     S, Revision.data(), Revision.size(), P.data(), P.size(),
                     Options.data(), Options.size())),
                 "package_behavior_id");
  }
  void prepare(bool Select = true) {
    open(Input);
    ASSERT_EQ(Artifacts.size(), 7u);
    Before = package(Artifacts[6]);
    After = package(Artifacts[3]);
    Base = field(take(neverd_web_packages_compare_json(
                     S, Revision.data(), Revision.size(), Before.data(),
                     Before.size(), After.data(), After.size())),
                 "package_diff_id");
    if (Select) {
      BB = behavior(Before, Artifacts[5]);
      AB = behavior(After, Artifacts[2]);
      ASSERT_FALSE(BB.empty());
      ASSERT_FALSE(AB.empty());
    }
  }
  Value compare(std::string_view B = {}, std::string_view A = {}) {
    llvm::json::Object Options{{"schema_version", 1}};
    if (!B.empty())
      Options["before_package_behavior_id"] = std::string(B);
    if (!A.empty())
      Options["after_package_behavior_id"] = std::string(A);
    const auto Text = llvm::formatv("{0}", Value(std::move(Options))).str();
    return take(neverd_web_package_behavior_diff_analyze_json(
        S, Revision.data(), Revision.size(), Base.data(), Base.size(),
        Text.data(), Text.size()));
  }
  Value rows(std::string_view ID, std::string_view Kind, uint64_t Offset = 0,
             uint64_t Limit = 128) {
    return take(neverd_web_package_behavior_diff_records_json(
        S, Revision.data(), Revision.size(), ID.data(), ID.size(), Kind.data(),
        Kind.size(), Offset, Limit));
  }
  std::vector<Value> cli(std::vector<std::string> Args, int ExitCode = 0) {
#ifdef NEVERD_WEB_TEST_CLI
    Args.insert(Args.begin(), {NEVERD_WEB_TEST_CLI, "web"});
    std::vector<llvm::StringRef> Refs(Args.begin(), Args.end());
    const auto Out = (Root / "stdout").string(),
               Err = (Root / "stderr").string();
    // ExecuteAndWait redirects can preserve the tail of a longer prior reply.
    std::ofstream(Out, std::ios::trunc).close();
    std::ofstream(Err, std::ios::trunc).close();
    const std::array<std::optional<llvm::StringRef>, 3> Redirects{
        llvm::StringRef(""), llvm::StringRef(Out), llvm::StringRef(Err)};
    const std::array<llvm::StringRef, 1> Env{"PATH=/neverd-no-external-tools"};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(NEVERD_WEB_TEST_CLI, Refs,
                                        llvm::ArrayRef(Env), Redirects, 60),
              ExitCode);
    std::vector<Value> Result;
    std::ifstream Stream(Out);
    for (std::string Line; std::getline(Stream, Line);) {
      EXPECT_EQ(Line.find("CANARY"), std::string::npos);
      auto Parsed = llvm::json::parse(Line);
      if (!Parsed) {
        llvm::consumeError(Parsed.takeError());
        throw std::runtime_error("invalid CLI response");
      }
      Result.push_back(std::move(*Parsed));
    }
    return Result;
#else
    return {};
#endif
  }
};

TEST_F(WebPackageBehaviorDiffSDK, AnchorsResolveWithoutIndependentNodeCache) {
  prepare();
  const auto D = compare(BB, AB);
  ASSERT_EQ(field(D, "status"), "ok");
  EXPECT_EQ(D.getAsObject()->getInteger("compared_pair_count"), 1);
  const auto ID = field(D, "package_behavior_diff_id");
  const auto Page = rows(ID, "occurrences");
  const auto &Items = *Page.getAsObject()->getArray("items");
  ASSERT_EQ(Items.size(), 2u);
  for (const auto &O : Items) {
    const auto PID = field(O, "package_behavior_id"),
               BID = field(O, "node_behavior_id");
    EXPECT_EQ(code(take(neverd_web_node_behavior_records_json(
                  S, Revision.data(), Revision.size(), BID.data(), BID.size(),
                  0, 1))),
              "unknown_node_behavior");
    const auto Original = take(neverd_web_package_behavior_records_json(
        S, Revision.data(), Revision.size(), PID.data(), PID.size(), "records",
        7, 0, 1));
    const auto &R = (*Original.getAsObject()->getArray("items"))[0];
    EXPECT_EQ(field(O, "behavior_record_id"), field(R, "record_id"));
    for (const auto *Key : {"source_id", "artifact_id", "file_evidence_id",
                            "node_id", "byte_offset", "byte_length"})
      EXPECT_EQ(field(O, Key), field(R, Key));
  }
  for (const auto *Key :
       {"selection_complete", "runtime_api_verified",
        "call_correspondence_verified", "argument_values_compared",
        "release_alignment_verified", "reachability_verified"})
    EXPECT_EQ(D.getAsObject()->getBoolean(Key), false);
#ifdef NEVERD_WEB_TEST_CLI
  const auto CLI =
      cli({"package-behavior-diff", Input.string(), "6", "3", "package-json",
           "before:5:commonjs", "after:2:commonjs"});
  ASSERT_EQ(CLI.size(), 4u);
  EXPECT_EQ(field(CLI[0], "package_behavior_diff_id"), ID);
  cli({"package-behavior-diff", Input.string(), "6", "3", "package-json",
       "before:05:commonjs"},
      2);
#endif
}

TEST_F(WebPackageBehaviorDiffSDK,
       EmptyCoverageStrictSelectionAndCacheInvalidation) {
  prepare();
  EXPECT_EQ(code(compare(AB, BB)), "package_behavior_diff_selection_mismatch");
  const auto Empty = compare();
  ASSERT_EQ(field(Empty, "status"), "ok");
  EXPECT_EQ(Empty.getAsObject()->getInteger("source_pair_count"), 0);
  const auto D = compare(BB, AB);
  const auto ID = field(D, "package_behavior_diff_id");
  EXPECT_EQ(field(compare(BB, AB), "package_behavior_diff_id"), ID);
  EXPECT_EQ(code(compare(BB)), "package_behavior_diff_cache_budget_exceeded");
  EXPECT_EQ(code(rows(ID, "raw")), "invalid_record_kind");
  EXPECT_EQ(code(rows(ID, "pairs", 0, 513)), "invalid_page");
  for (const auto *Options :
       {R"({"schema_version":1,"execute":false})",
        R"({"schema_version":1,"before_package_behavior_id":null})"})
    EXPECT_EQ(code(take(neverd_web_package_behavior_diff_analyze_json(
                  S, Revision.data(), Revision.size(), Base.data(), Base.size(),
                  Options, std::string_view(Options).size()))),
              "invalid_package_behavior_diff_options");
  const auto Old = Revision;
  open(Input);
  EXPECT_EQ(
      code(take(neverd_web_package_behavior_diff_records_json(
          S, Old.data(), Old.size(), ID.data(), ID.size(), "pairs", 5, 0, 1))),
      "stale_revision");
  EXPECT_EQ(code(rows(ID, "pairs")), "unknown_package_behavior_diff");
  EXPECT_EQ(code(compare(BB, AB)), "unknown_package_diff");
}

TEST_F(WebPackageBehaviorDiffSDK, OccurrencePagingRetainsAllSixHundredAnchors) {
  for (const auto *Side : {"before", "after"}) {
    std::ofstream Stream(Input / Side / "a.js");
    for (unsigned I = 0; I < 300; ++I)
      Stream << Call;
  }
  prepare();
  const auto D = compare(BB, AB);
  ASSERT_EQ(D.getAsObject()->getInteger("occurrence_count"), 600);
  const auto ID = field(D, "package_behavior_diff_id");
  const auto First = rows(ID, "occurrences", 0, 512);
  ASSERT_EQ(First.getAsObject()->getArray("items")->size(), 512u);
  EXPECT_EQ(First.getAsObject()->getInteger("next_offset"), 512);
  const auto Last = rows(ID, "occurrences", 512, 512);
  EXPECT_EQ(Last.getAsObject()->getArray("items")->size(), 88u);
  EXPECT_EQ(Last.getAsObject()->getBoolean("page_complete"), true);
  EXPECT_TRUE(
      rows(ID, "occurrences", 600).getAsObject()->getArray("items")->empty());
  EXPECT_EQ(code(rows(ID, "occurrences", 601)), "invalid_page");
}

TEST_F(WebPackageBehaviorDiffSDK,
       ValueCoverageRegressionDoesNotBecomeBehaviorDelta) {
  std::string Text =
      "const {Buffer:B}=require('node:buffer'); B.from('QQ==','base64');";
  std::ofstream(Input / "before/a.js") << Text;
  for (unsigned N = 0; N < 8; ++N) {
    Text += "const x" + std::to_string(N) + " = ''";
    for (unsigned I = 0; I < 32; ++I)
      Text += "+'" + std::string(1024, 'a') + "'";
    Text += ";";
  }
  std::ofstream(Input / "after/a.js") << Text;
  prepare();
  const auto D = compare(BB, AB);
  ASSERT_EQ(field(D, "status"), "ok");
  EXPECT_EQ(D.getAsObject()->getInteger("compared_pair_count"), 0);
  EXPECT_EQ(D.getAsObject()->getInteger("pattern_group_count"), 0);
  const auto Page = rows(field(D, "package_behavior_diff_id"), "pairs");
  const auto &Pair = (*Page.getAsObject()->getArray("items"))[0];
  EXPECT_EQ(Pair.getAsObject()->getObject("after")->getString("value_status"),
            "budget_exceeded");
}

TEST_F(WebPackageBehaviorDiffSDK, ArchiveCLIUsesSameExplicitComparison) {
#ifndef NEVERD_WEB_TEST_CLI
  GTEST_SKIP() << "CLI unavailable";
#else
  const auto Caps = take(neverd_web_capabilities_json());
  bool ArchivesAvailable = false;
  for (const auto &A : *Caps.getAsObject()->getArray("analysis"))
    if (field(A, "kind") == "package_archive")
      ArchivesAvailable =
          A.getAsObject()->getBoolean("available").value_or(false);
  if (!ArchivesAvailable)
    GTEST_SKIP() << "Pinned archive path policy unavailable";
  using namespace neverd::web::test;
  const auto Dir = Root / "archives";
  std::filesystem::create_directory(Dir);
  for (const auto *Name : {"before.tar", "after.tar"})
    std::ofstream(Dir / Name, std::ios::binary) << finishTar(
        tarMember("package/package.json", R"({"name":"CANARY_PACKAGE"})") +
        tarMember("package/a.js", Call));
  const auto Result =
      cli({"archive-package-behavior-diff", Dir.string(), "tar", "2", "0", "1",
           "0", "package-json", "before:1:commonjs", "after:1:commonjs"});
  ASSERT_EQ(Result.size(), 4u);
  EXPECT_EQ(Result[0].getAsObject()->getInteger("compared_pair_count"), 1);
  EXPECT_EQ(Result[0].getAsObject()->getInteger("occurrence_count"), 2);
  const auto Empty = cli({"archive-package-behavior-diff", Dir.string(), "tar",
                          "2", "0", "1", "0", "package-json"});
  ASSERT_EQ(Empty.size(), 4u);
  EXPECT_EQ(Empty[0].getAsObject()->getInteger("compared_pair_count"), 0);
#endif
}

TEST_F(WebPackageBehaviorDiffSDK, BothSidesShareTheExistingSourceCacheLimit) {
  for (unsigned I = 0; I < 15; ++I)
    std::ofstream(Input / ("zz" + std::to_string(I) + ".js")) << Call;
  open(Input);
  ASSERT_EQ(Artifacts.size(), 22u);
  Before = package(Artifacts[6]);
  After = package(Artifacts[3]);
  BB = behavior(Before, Artifacts[5]);
  AB = behavior(After, Artifacts[2]);
  ASSERT_FALSE(BB.empty());
  ASSERT_FALSE(AB.empty());
  for (unsigned I = 7; I < 21; ++I)
    ASSERT_EQ(field(source(Artifacts[I]), "status"), "ok");
  EXPECT_EQ(code(source(Artifacts[21])), "source_cache_budget_exceeded");
}

TEST_F(WebPackageBehaviorDiffSDK, AggregateWorkFailurePublishesNoCachedDiff) {
  for (const auto *Side : {"before", "after"}) {
    std::ofstream Stream(Input / Side / "a.js");
    for (unsigned I = 0; I < 3000; ++I)
      Stream << Call;
  }
  prepare();
  for (const auto &ID : {BB, AB}) {
    const auto Original = take(neverd_web_package_behavior_records_json(
        S, Revision.data(), Revision.size(), ID.data(), ID.size(), "sources", 7,
        0, 1));
    ASSERT_EQ(Original.getAsObject()->getInteger("record_count"), 3000);
  }
  EXPECT_EQ(code(compare(BB, AB)),
            "package_behavior_diff_work_budget_exceeded");
  const auto Meta = take(neverd_web_metadata_json(S));
  EXPECT_EQ(Meta.getAsObject()->getInteger("package_behavior_diff_count"), 0);
  EXPECT_EQ(field(compare(), "status"), "ok");
}
} // namespace
