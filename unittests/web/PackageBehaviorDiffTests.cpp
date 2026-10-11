//===- PackageBehaviorDiffTests.cpp - Selected behavior comparison -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercise independent evidence anchors, degraded coverage and file grouping.
///
//===----------------------------------------------------------------------===//

#include "Internal.h"
#include "gtest/gtest.h"

#include "neverd/web/Error.h"
#include "neverd/web/PackageArchive.h"
#include "neverd/web/PackageBehaviorDiff.h"
#include "neverd/web/SourceValues.h"

#include "llvm/Support/FileSystem.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>

namespace {
using namespace neverd::web;

struct SelectedPackage {
  Snapshot Captured;
  PackageAnalysis P;
  PackageBehavior B;
  std::vector<SourceAnalysis> Sources;
  std::vector<NodeBehavior> Behaviors;

  const Artifact &file(std::string_view Path) const {
    for (const auto &F : Captured.Artifacts)
      if (F.MemberPath == Path)
        return F;
    throw std::runtime_error("missing fixture member");
  }
  void select(const std::vector<std::pair<std::string, std::string>> &Paths) {
    Sources.clear();
    Behaviors.clear();
    Sources.reserve(Paths.size());
    Behaviors.reserve(Paths.size());
    std::vector<PackageBehaviorInput> Input;
    for (const auto &[Path, Type] : Paths) {
      const auto &F = file(Path);
      Sources.push_back(inspectJavaScript(
          F.ID, F.Content.read(0, F.Content.size(), MaxJavaScriptBytes), Type));
      Behaviors.push_back(analyzeNodeBehavior(Sources.back()));
      Input.push_back({&Sources.back(), &Behaviors.back(), F.Content.size()});
    }
    B = analyzePackageBehavior(P, Input, {});
  }
};

class WebPackageBehaviorDiff : public ::testing::Test {
protected:
  std::filesystem::path Root;
  SelectedPackage Before, After;
  static constexpr std::string_view Write =
      "require('fs').writeFileSync('CANARY_PATH','CANARY_DATA');";

  void SetUp() override {
#ifdef _WIN32
    GTEST_SKIP() << "POSIX capture profile required";
#endif
    llvm::SmallString<128> Dir;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-behavior-diff", Dir));
    Root = Dir.str().str();
    for (const auto *Side : {"before", "after"}) {
      write(Side, "package.json", R"({"name":"CANARY_PACKAGE"})");
      write(Side, "a.js", Write);
      write(Side, "b.js", "function ordinary(x){return x+1;}");
    }
  }
  void TearDown() override {
    std::error_code EC;
    if (!Root.empty())
      std::filesystem::remove_all(Root, EC);
  }
  void write(std::string_view Side, std::string_view Path,
             std::string_view Text) {
    const auto P = Root / Side / Path;
    std::filesystem::create_directories(P.parent_path());
    std::ofstream Stream(P, std::ios::binary);
    Stream.write(Text.data(), Text.size());
    ASSERT_TRUE(Stream.good());
  }
  void prepare(std::string_view BeforeMode = "commonjs",
               std::string_view AfterMode = "commonjs") {
    Before.Captured = capture((Root / "before").string(), Limits{});
    After.Captured = capture((Root / "after").string(), Limits{});
    for (auto *S : {&Before, &After})
      S->P = analyzePackages(S->Captured, S->file("package.json").ID,
                             "package-json");
    Before.select({{"a.js", std::string(BeforeMode)}});
    After.select({{"a.js", std::string(AfterMode)}});
  }
  PackageBehaviorDiff run(bool B = true, bool A = true) {
    return comparePackageBehavior(comparePackages(Before.P, After.P), Before.P,
                                  After.P, B ? &Before.B : nullptr,
                                  A ? &After.B : nullptr);
  }
};

TEST_F(WebPackageBehaviorDiff,
       UnchangedCountsDoNotHideChangedArgumentsOrBytes) {
  write("after", "a.js", "require('fs').writeFileSync('CANARY_NEW','NEW');");
  prepare();
  const auto D = run();
  ASSERT_EQ(D.Pairs.size(), 1u);
  EXPECT_EQ(D.Pairs[0].Status, "compared_recognized_patterns");
  EXPECT_EQ(D.Pairs[0].Bytes, "different_supplied_file_bytes");
  ASSERT_EQ(D.Patterns.size(), 1u);
  EXPECT_EQ(D.Patterns[0].Before, 1u);
  EXPECT_EQ(D.Patterns[0].After, 1u);
  ASSERT_EQ(D.Occurrences.size(), 2u);
  for (const auto &O : D.Occurrences) {
    const auto &S = O.Side == "before" ? Before : After;
    const auto &R = S.Behaviors[0].Records.at(0);
    EXPECT_EQ(O.RecordID, R.ID);
    EXPECT_EQ(O.NodeID, R.NodeID);
    EXPECT_EQ(O.Start, R.Start);
    EXPECT_EQ(O.End, R.End);
    EXPECT_EQ(O.PackageBehaviorID, S.B.ID);
    EXPECT_EQ(O.ArtifactID, S.Sources[0].ArtifactID);
    EXPECT_EQ(O.GroupID, D.Patterns[0].ID);
  }
}

TEST_F(WebPackageBehaviorDiff,
       AddedProcessNetworkAndWritePatternsStaySyntactic) {
  write("before", "a.js", "function harmless(x){return x;}");
  write("after", "a.js",
        "require('child_process').execFile('CANARY_BUILD_TOOL');"
        "require('https').request('CANARY_URL');"
        "require('fs').writeFileSync('CANARY_PATH','CANARY_DATA');");
  prepare();
  const auto D = run();
  ASSERT_EQ(D.ComparedPairs, 1u);
  ASSERT_EQ(D.Patterns.size(), 3u);
  for (const auto &G : D.Patterns) {
    EXPECT_EQ(G.Before, 0u);
    EXPECT_EQ(G.After, 1u);
  }
}

TEST_F(WebPackageBehaviorDiff, CountsCannotCancelAcrossDifferentFiles) {
  write("after", "a.js", "function ordinary(x){return x+1;}");
  write("after", "b.js", Write);
  prepare();
  Before.select({{"a.js", "commonjs"}, {"b.js", "commonjs"}});
  After.select({{"b.js", "commonjs"}, {"a.js", "commonjs"}});
  const auto D = run();
  ASSERT_EQ(D.ComparedPairs, 2u);
  ASSERT_EQ(D.Patterns.size(), 2u);
  EXPECT_NE(D.Patterns[0].PairID, D.Patterns[1].PairID);
  std::vector<int64_t> Deltas;
  for (const auto &G : D.Patterns)
    Deltas.push_back(int64_t(G.After) - int64_t(G.Before));
  std::sort(Deltas.begin(), Deltas.end());
  EXPECT_EQ(Deltas, (std::vector<int64_t>{-1, 1}));
  std::reverse(Before.B.Sources.begin(), Before.B.Sources.end());
  std::reverse(After.B.Sources.begin(), After.B.Sources.end());
  const auto Again = run();
  EXPECT_EQ(Again.ID, D.ID);
  ASSERT_EQ(Again.Patterns.size(), D.Patterns.size());
  for (size_t I = 0; I < D.Patterns.size(); ++I)
    EXPECT_EQ(D.Patterns[I].ID, Again.Patterns[I].ID);
}

TEST_F(WebPackageBehaviorDiff,
       ValueBudgetDegradationCannotInventEncodingDelta) {
  std::string Text =
      "const {Buffer:B}=require('node:buffer'); B.from('QQ==','base64');";
  write("before", "a.js", Text);
  for (unsigned N = 0; N < 8; ++N) {
    Text += "const x" + std::to_string(N) + " = ''";
    for (unsigned I = 0; I < 32; ++I)
      Text += "+'" + std::string(1024, 'a') + "'";
    Text += ";";
  }
  write("after", "a.js", Text);
  prepare();
  ASSERT_EQ(Before.Behaviors[0].Status, "partial");
  ASSERT_EQ(After.Behaviors[0].Status, "partial");
  ASSERT_EQ(After.Behaviors[0].ValueStatus, "budget_exceeded");
  const auto D = run();
  ASSERT_EQ(D.Pairs.size(), 1u);
  EXPECT_EQ(D.Pairs[0].After.ValueStatus, "budget_exceeded");
  EXPECT_EQ(D.Pairs[0].After.Status, "upstream_coverage_unavailable");
  EXPECT_EQ(D.ComparedPairs, 0u);
  EXPECT_TRUE(D.Patterns.empty());
  EXPECT_TRUE(D.Occurrences.empty());
}

TEST_F(WebPackageBehaviorDiff, AbsentAndUnselectedCoverageNeverBecomesZero) {
  prepare();
  EXPECT_TRUE(run(false, false).Pairs.empty());
  auto D = run(true, false);
  ASSERT_EQ(D.Pairs.size(), 1u);
  EXPECT_EQ(D.Pairs[0].After.Status, "behavior_selection_not_supplied");
  EXPECT_TRUE(D.Patterns.empty());
  After.select({{"b.js", "commonjs"}});
  D = run();
  ASSERT_EQ(D.Pairs.size(), 2u);
  EXPECT_EQ(D.ComparedPairs, 0u);
  EXPECT_TRUE(D.Occurrences.empty());
  std::filesystem::remove(Root / "after" / "a.js");
  After.Captured = capture((Root / "after").string(), Limits{});
  After.P = analyzePackages(After.Captured, After.file("package.json").ID,
                            "package-json");
  After.select({{"b.js", "commonjs"}});
  D = run();
  EXPECT_TRUE(std::any_of(D.Pairs.begin(), D.Pairs.end(), [](const auto &P) {
    return P.After.Status == "not_captured_or_unavailable";
  }));
  EXPECT_TRUE(D.Patterns.empty());
}

TEST_F(WebPackageBehaviorDiff,
       UnavailableAndDifferentSourceModesAreIncomparable) {
  prepare("script", "commonjs");
  EXPECT_EQ(run().Pairs.at(0).Status, "source_type_mismatch");
  write("after", "a.js", "const =");
  prepare();
  const auto D = run();
  ASSERT_EQ(D.Pairs.size(), 1u);
  EXPECT_EQ(D.Pairs[0].After.Status, "behavior_unavailable");
  EXPECT_TRUE(D.Patterns.empty());
}

TEST_F(WebPackageBehaviorDiff, RootIdentityAndPlatformContextRetainBoundaries) {
  for (const auto Manifest : {R"({"name":"CANARY_OTHER"})",
                              R"({"name":"CANARY_PACKAGE","os":["linux"]})"}) {
    write("after", "package.json", Manifest);
    prepare();
    const auto D = run();
    EXPECT_EQ(D.Pairs.at(0).Status, "incompatible_package_context");
    EXPECT_TRUE(D.Patterns.empty());
  }
  write("before", "package.json", "{}");
  write("after", "package.json", "{}");
  prepare();
  const auto D = run();
  EXPECT_EQ(D.RootIdentity, "name_not_supplied");
  EXPECT_EQ(D.ComparedPairs, 1u);
}

TEST_F(WebPackageBehaviorDiff, InvalidOwnersAndBudgetsFailAtomically) {
  prepare();
  auto Base = comparePackages(Before.P, After.P);
  // Identical complete captures deliberately have identical content IDs.
  // Use an unrelated owner, not the other identical snapshot, as the negative.
  Base.AfterID = identity("test-unrelated-package", {});
  EXPECT_THROW(
      comparePackageBehavior(Base, Before.P, After.P, &Before.B, &After.B),
      Error);
  auto Saved = After.B;
  After.B.PackageAnalysisID = Base.AfterID;
  EXPECT_THROW(run(), Error);
  After.B = Saved;
  After.B.Sources[0].SourceHash = std::string(64, '0');
  EXPECT_THROW(run(), Error);
  After.B = Saved;
  After.B.Sources[0].Records[0].End = After.B.Sources[0].SourceBytes + 1;
  EXPECT_THROW(run(), Error);
  After.B = Saved;
  After.B.Sources.resize(MaxPackageBehaviorSources + 1, Saved.Sources[0]);
  EXPECT_THROW(run(), Error);
  After.B = Saved;
  After.B.Sources[0].Records.resize(MaxPackageBehaviorRecords + 1,
                                    Saved.Sources[0].Records[0]);
  EXPECT_THROW(run(), Error);
  After.B = Saved;
  After.P.Files.resize(MaxPackageRecords + 1, After.P.Files[0]);
  EXPECT_THROW(run(), Error);
}

TEST_F(WebPackageBehaviorDiff, InventoryComparisonWorkHasAnAggregateCeiling) {
  prepare();
  const auto Template = After.P.Files[0];
  After.P.Files.clear();
  for (unsigned I = 0; I < 4096; ++I) {
    auto F = Template;
    F.Path = std::string(2048, 'p') + std::to_string(I);
    F.ArtifactID = identity("test-budget-artifact", {F.Path});
    F.ID = identity("test-budget-file", {F.ArtifactID});
    After.P.Files.push_back(std::move(F));
  }
  try {
    (void)run(false, false);
    FAIL() << "Comparison retained an over-budget inventory";
  } catch (const Error &E) {
    EXPECT_STREQ(E.what(), "package_behavior_diff_work_budget_exceeded");
  }
}

TEST_F(WebPackageBehaviorDiff, EmbeddedSourcesHaveSeparateUnpairedCoverage) {
  prepare();
  const auto &F = Before.file("a.js");
  const auto S =
      inspectJavaScript(identity("test-embedded", {F.ID}), Write, "commonjs");
  const auto B = analyzeNodeBehavior(S);
  const std::array Inputs{PackageBehaviorInput{&S, &B, Write.size()}};
  const std::array Origins{PackageBehaviorOrigin{
      S.ArtifactID, F.ID, identity("test-origin", {F.ID}), "bun_source"}};
  Before.B = analyzePackageBehavior(Before.P, Inputs, Origins);
  const auto D = run();
  ASSERT_EQ(D.Pairs.size(), 2u);
  EXPECT_EQ(D.ComparedPairs, 0u);
  EXPECT_TRUE(D.Patterns.empty());
  EXPECT_TRUE(std::any_of(D.Pairs.begin(), D.Pairs.end(), [](const auto &P) {
    return P.Status == "embedded_source_boundary" && P.Before.PackageID.empty();
  }));
}

TEST(WebPackageBehaviorDiffReal,
     ClaudeCode21295And21296KeepAllSelectedOccurrences) {
  const auto *Dir = std::getenv("NEVERD_CLAUDE_CODE_BEHAVIOR_DIFF_DIR");
  if (!Dir || !packageArchiveAvailable() || !packageGzipAvailable())
    GTEST_SKIP()
        << "Optional wrapper pair, archive path policy or zlib unavailable";
  std::array<SelectedPackage, 2> Sides;
  const std::array Versions{"2.1.295", "2.1.296"};
  const std::array Hashes{
      "fe0d5efb8136f23e7292e227d14a1fc04f3fb05981dd9d0585d03be7bd8d863a",
      "f6c375d51d4c22a7a850e185a0d7ddda85173f89dbbeb3262aa7971e3bd670b5"};
  for (unsigned I = 0; I < 2; ++I) {
    const auto Original = capture(
        std::string(Dir) + "/claude-code-" + Versions[I] + ".tgz", Limits{});
    ASSERT_EQ(Original.Artifacts[0].BlobHash, Hashes[I]);
    const auto Archive = extractPackageArchive(Original.Artifacts[0], "tgz");
    ASSERT_EQ(Archive.Members.size(), 7u);
    auto &S = Sides[I];
    S.Captured = packageArchiveNamespace(Archive);
    S.P = analyzePackages(S.Captured, Archive.Members[3].ID, "package-json");
    S.select({{Archive.Members[0].Path, "commonjs"},
              {Archive.Members[1].Path, "commonjs"}});
    ASSERT_EQ(S.B.RecordCount, 8u);
  }
  const auto &B = Sides[0], &A = Sides[1];
  const auto D =
      comparePackageBehavior(comparePackages(B.P, A.P), B.P, A.P, &B.B, &A.B);
  ASSERT_EQ(D.ComparedPairs, 2u);
  ASSERT_EQ(D.Occurrences.size(), 16u);
  for (const auto &O : D.Occurrences) {
    const auto &Side = O.Side == "before" ? B : A;
    const auto Source =
        std::find_if(Side.B.Sources.begin(), Side.B.Sources.end(),
                     [&](const auto &S) { return S.SourceID == O.SourceID; });
    ASSERT_NE(Source, Side.B.Sources.end());
    const auto R =
        std::find_if(Source->Records.begin(), Source->Records.end(),
                     [&](const auto &R) { return R.ID == O.RecordID; });
    ASSERT_NE(R, Source->Records.end());
    EXPECT_EQ(O.NodeID, R->NodeID);
    EXPECT_EQ(O.Start, R->Start);
    EXPECT_EQ(O.End, R->End);
  }
}
} // namespace
