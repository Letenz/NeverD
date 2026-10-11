//===- PackageBehaviorTests.cpp - Source inventory and delivery joins ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Qualify exact membership, unknown ownership and explicit source coverage.
///
//===----------------------------------------------------------------------===//

#include "Internal.h"
#include "gtest/gtest.h"

#include "neverd/web/Error.h"
#include "neverd/web/NodeBehavior.h"

#include "llvm/Support/FileSystem.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>

namespace {
using namespace neverd::web;

class WebPackageBehavior : public ::testing::Test {
protected:
  std::filesystem::path Root;
  Snapshot Captured;
  PackageAnalysis Packages;
  void SetUp() override {
#ifdef _WIN32
    GTEST_SKIP() << "POSIX capture profile required";
#endif
    llvm::SmallString<128> Dir;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-behavior", Dir));
    Root = Dir.str().str();
    write("package.json", R"({"name":"CANARY_PACKAGE","version":"1.0.0",
      "main":"index.js","scripts":{"postinstall":"CANARY_MUST_NOT_RUN"}})");
    write("package-lock.json", R"({"lockfileVersion":3,
      "packages":{"":{"name":"CANARY_PACKAGE","version":"1.0.0"}}})");
    write("index.js", "require('child_process').execFile('CANARY_TOOL',opts);");
    write("ordinary.js", "function ordinary(x){return x+1;}");
    write("broken.js", "const =");
    write("node_modules/unrecorded/index.js", "require('fs').writeFile(p,d);");
    write("helper.bin", "inert container placeholder");
    captureAll();
  }
  void TearDown() override {
    std::error_code EC;
    if (!Root.empty())
      std::filesystem::remove_all(Root, EC);
  }
  void write(std::string_view Path, std::string_view Bytes) {
    const auto P = Root / Path;
    std::filesystem::create_directories(P.parent_path());
    std::ofstream F(P, std::ios::binary);
    F.write(Bytes.data(), Bytes.size());
    ASSERT_TRUE(F.good());
  }
  void captureAll() {
    Captured = capture(Root.string(), Limits{});
    Packages =
        analyzePackages(Captured, file("package-lock.json").ID, "npm-lock");
  }
  const Artifact &file(std::string_view Path) {
    for (const auto &A : Captured.Artifacts)
      if (A.MemberPath == Path)
        return A;
    throw std::runtime_error("missing captured fixture");
  }
  SourceAnalysis source(std::string_view Path) {
    const auto &F = file(Path);
    return inspectJavaScript(
        F.ID, F.Content.read(0, F.Content.size(), MaxJavaScriptBytes),
        "commonjs");
  }
  static PackageBehaviorInput input(const SourceAnalysis &S,
                                    const NodeBehavior &B, uint64_t Size) {
    return {&S, &B, Size};
  }
};

TEST_F(WebPackageBehavior, DirectEntryAndUnrecordedOwnershipRemainDistinct) {
  const auto S = source("index.js"),
             U = source("node_modules/unrecorded/index.js");
  const auto B = analyzeNodeBehavior(S), UB = analyzeNodeBehavior(U);
  const std::array Input = {
      input(S, B, file("index.js").Content.size()),
      input(U, UB, file("node_modules/unrecorded/index.js").Content.size())};
  const auto A = analyzePackageBehavior(Packages, Input, {});
  ASSERT_EQ(A.Sources.size(), 2u);
  EXPECT_EQ(A.RecordCount, 2u);
  for (const auto &R : A.Sources) {
    EXPECT_EQ(R.Relation, "direct_file");
    EXPECT_EQ(R.DeliveryArtifactID, R.ArtifactID);
    if (R.SourceID == S.ID) {
      EXPECT_FALSE(R.PackageID.empty());
      ASSERT_EQ(R.EntryIDs.size(), 1u);
    } else {
      EXPECT_TRUE(R.PackageID.empty());
      EXPECT_TRUE(R.EntryIDs.empty());
    }
  }
  auto Reversed = Input;
  std::reverse(Reversed.begin(), Reversed.end());
  EXPECT_EQ(analyzePackageBehavior(Packages, Reversed, {}).ID, A.ID);
}

TEST_F(WebPackageBehavior, UnavailableAndZeroPatternSourcesAreRetained) {
  const auto S = source("broken.js"), O = source("ordinary.js");
  const auto B = analyzeNodeBehavior(S), OB = analyzeNodeBehavior(O);
  const std::array Input = {input(S, B, file("broken.js").Content.size()),
                            input(O, OB, file("ordinary.js").Content.size())};
  const auto A = analyzePackageBehavior(Packages, Input, {});
  EXPECT_EQ(A.UnavailableSources, 1u);
  EXPECT_EQ(A.RecordCount, 0u);
  EXPECT_EQ(A.Sources.size(), 2u);
}

TEST_F(WebPackageBehavior,
       EmbeddedModuleGetsDeliveryPackageWithoutDirectEntry) {
  const auto S =
      inspectJavaScript(identity("embedded-test", {"source"}),
                        "require('fs').writeFile(path,data);", "commonjs");
  const auto B = analyzeNodeBehavior(S);
  const std::array Input = {input(S, B, 34)};
  // The entry targets the container. It must not become an internal-module
  // declared entry or a claim that the delivery package owns that module.
  const std::array Origins = {PackageBehaviorOrigin{
      S.ArtifactID, file("index.js").ID, identity("extractor-test", {"module"}),
      "bun_source"}};
  const auto A = analyzePackageBehavior(Packages, Input, Origins);
  const auto &R = A.Sources.at(0);
  EXPECT_EQ(R.Relation, "embedded_source");
  EXPECT_FALSE(R.PackageID.empty());
  EXPECT_EQ(R.DeliveryArtifactID, file("index.js").ID);
  EXPECT_TRUE(R.EntryIDs.empty());
  EXPECT_EQ(R.OriginIDs.size(), 1u);
  const auto Inner = identity("extractor-test", {"inner-container"});
  const std::array Nested = {
      PackageBehaviorOrigin{S.ArtifactID, Inner,
                            identity("extractor-test", {"nested-module"}),
                            "bun_source"},
      PackageBehaviorOrigin{Inner, file("helper.bin").ID,
                            identity("extractor-test", {"outer-region"}),
                            "sea_region"}};
  EXPECT_EQ(analyzePackageBehavior(Packages, Input, Nested)
                .Sources.at(0)
                .OriginIDs.size(),
            2u);
}

TEST_F(WebPackageBehavior, ForeignDuplicateAndMismatchedEvidenceRefuse) {
  const auto S = source("index.js");
  auto B = analyzeNodeBehavior(S);
  const auto I = input(S, B, file("index.js").Content.size());
  EXPECT_THROW(analyzePackageBehavior(Packages, std::array{I, I}, {}), Error);
  auto Foreign = Packages;
  Foreign.Files.clear();
  EXPECT_THROW(analyzePackageBehavior(Foreign, std::array{I}, {}), Error);
  B.SourceHash = sha256("different");
  EXPECT_THROW(analyzePackageBehavior(Packages, std::array{I}, {}), Error);
  B = analyzeNodeBehavior(S);
  auto WrongSize = I;
  WrongSize.SourceBytes++;
  EXPECT_THROW(analyzePackageBehavior(Packages, std::array{WrongSize}, {}),
               Error);
  auto BadOwner = Packages;
  for (auto &F : BadOwner.Files)
    if (F.ArtifactID == S.ArtifactID)
      F.Package = 9999;
  EXPECT_THROW(analyzePackageBehavior(BadOwner, std::array{I}, {}), Error);
}

TEST_F(WebPackageBehavior, AncestryCyclesAndAmbiguityFailClosed) {
  const auto S =
      inspectJavaScript(identity("embedded-test", {"source"}), "1;", "script");
  const auto B = analyzeNodeBehavior(S);
  const std::array Input = {input(S, B, 2)};
  const auto Link = identity("extractor-test", {"link"});
  const PackageBehaviorOrigin O{S.ArtifactID, S.ArtifactID, Link, "bun_source"};
  EXPECT_THROW(analyzePackageBehavior(Packages, Input, std::array{O}), Error);
  const PackageBehaviorOrigin P{S.ArtifactID, file("helper.bin").ID, Link,
                                "bun_source"};
  EXPECT_THROW(analyzePackageBehavior(Packages, Input, std::array{P, P}),
               Error);
  std::vector<PackageBehaviorOrigin> Deep;
  std::string Child = S.ArtifactID;
  for (unsigned J = 0; J < 17; ++J) {
    const auto Parent = J == 16 ? file("helper.bin").ID
                                : identity("parent-test", {std::to_string(J)});
    Deep.push_back({Child, Parent, identity("edge-test", {std::to_string(J)}),
                    "sea_region"});
    Child = Parent;
  }
  EXPECT_THROW(analyzePackageBehavior(Packages, Input, Deep), Error);
}

TEST_F(WebPackageBehavior, SelectionAndRetainedRecordWorkRemainBounded) {
  const auto S = source("index.js");
  auto B = analyzeNodeBehavior(S);
  const auto I = input(S, B, file("index.js").Content.size());
  std::vector<PackageBehaviorInput> Many(17, I);
  EXPECT_THROW(analyzePackageBehavior(Packages, Many, {}), Error);
  auto Huge = I;
  Huge.SourceBytes = 8 * 1024 * 1024 + 1;
  EXPECT_THROW(analyzePackageBehavior(Packages, std::array{Huge}, {}), Error);
  B.Records.resize(MaxPackageBehaviorRecords + 1);
  EXPECT_THROW(analyzePackageBehavior(Packages, std::array{I}, {}), Error);
  B = analyzeNodeBehavior(S);
  B.Records.front().API = std::string(MaxPackageBehaviorSteps, 'x');
  EXPECT_THROW(analyzePackageBehavior(Packages, std::array{I}, {}), Error);
}

} // namespace
