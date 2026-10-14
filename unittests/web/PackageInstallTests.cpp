//===- PackageInstallTests.cpp - Offline installation trace contracts ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercise exact captured entry candidates, opaque chains and unknown
/// coverage.
///
//===----------------------------------------------------------------------===//

#include "Internal.h"
#include "PackageArchiveFixture.h"
#include "ZipFixture.h"
#include "gtest/gtest.h"

#include "neverd/web/Error.h"
#include "neverd/web/PackageArchive.h"
#include "neverd/web/PackageInstall.h"
#include "neverd/web/SourceOrigins.h"

#include "llvm/Support/FileSystem.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace {
using namespace neverd::web;

class WebPackageInstall : public ::testing::Test {
protected:
  std::filesystem::path Root;
  Snapshot Captured;
  PackageAnalysis P;
  PackageBehavior B;
  std::vector<SourceAnalysis> Sources;
  std::vector<NodeBehavior> Behaviors;
  std::vector<PackageInstallInput> Input;

  void SetUp() override {
    llvm::SmallString<128> Dir;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-install", Dir));
    Root = Dir.str().str();
    write("package.json", R"({"scripts":{"install":"node entry.js"}})");
    write("entry.js", "require('./dep.js');");
    write("dep.js",
          "require('fs').writeFileSync('CANARY_PATH','CANARY_DATA');");
  }
  void TearDown() override {
    std::error_code EC;
    if (!Root.empty())
      std::filesystem::remove_all(Root, EC);
  }
  void write(std::string_view Path, std::string_view Text) {
    const auto F = Root / Path;
    std::filesystem::create_directories(F.parent_path());
    std::ofstream Stream(F, std::ios::binary);
    Stream.write(Text.data(), Text.size());
    ASSERT_TRUE(Stream.good());
  }
  const Artifact &file(std::string_view Path) const {
    for (const auto &F : Captured.Artifacts)
      if (F.MemberPath == Path)
        return F;
    throw std::runtime_error("missing fixture member");
  }
  void captureAll(std::string_view Manifest = "package.json",
                  std::string_view Kind = "package-json") {
    Captured = capture(Root.string(), Limits{});
    P = analyzePackages(Captured, file(Manifest).ID, Kind);
    Sources.clear();
    Behaviors.clear();
    Input.clear();
  }
  void select(std::vector<std::pair<std::string, std::string>> Paths) {
    Sources.reserve(Paths.size());
    Behaviors.reserve(Paths.size());
    std::vector<PackageBehaviorInput> Join;
    for (const auto &[Path, Type] : Paths) {
      const auto &F = file(Path);
      Sources.push_back(inspectJavaScript(
          F.ID, F.Content.read(0, F.Content.size(), MaxJavaScriptBytes), Type));
      Behaviors.push_back(analyzeNodeBehavior(Sources.back()));
      Join.push_back({&Sources.back(), &Behaviors.back(), F.Content.size()});
      Input.push_back({&Sources.back(), F.Content.size()});
    }
    B = analyzePackageBehavior(P, Join, {});
  }
  PackageInstall run(bool Selected = true) {
    return analyzePackageInstall(
        P, Captured, Selected ? &B : nullptr,
        Selected ? std::span(Input) : std::span<const PackageInstallInput>{});
  }
};

TEST_F(WebPackageInstall, EntryToBehaviorRetainsExactRequestAndRecordAnchors) {
  captureAll();
  select({{"entry.js", "commonjs"}, {"dep.js", "commonjs"}});
  const auto A = run();
  ASSERT_EQ(A.Scripts.size(), 1u);
  EXPECT_EQ(A.Scripts[0].Status, "selected_entry_candidate");
  EXPECT_EQ(A.Scripts[0].CommandStart, 5u);
  EXPECT_EQ(A.Scripts[0].CommandEnd, 13u);
  ASSERT_EQ(A.Associations.size(), 1u);
  const auto &R = A.Associations[0];
  EXPECT_EQ(R.ScriptID, P.Scripts[0].ID);
  EXPECT_EQ(R.SourceID, Sources[1].ID);
  EXPECT_EQ(R.RecordID, Behaviors[1].Records.at(0).ID);
  const auto &Original = Behaviors[1].Records.at(0);
  EXPECT_EQ(R.NodeID, Original.NodeID);
  EXPECT_EQ(R.API, Original.API);
  EXPECT_EQ(R.Kind, Original.Kind);
  EXPECT_EQ(R.OriginEvidence, Original.OriginEvidence);
  EXPECT_EQ(R.Start, Original.Start);
  EXPECT_EQ(R.End, Original.End);
  ASSERT_EQ(R.RequestIDs.size(), 1u);
  const auto Q =
      std::find_if(A.Requests.begin(), A.Requests.end(),
                   [&](const auto &Q) { return Q.ID == R.RequestIDs[0]; });
  ASSERT_NE(Q, A.Requests.end());
  EXPECT_EQ(Q->SourceID, Sources[0].ID);
  EXPECT_EQ(Q->TargetSourceID, Sources[1].ID);
  EXPECT_EQ(Q->ArtifactID, file("dep.js").ID);
  EXPECT_EQ(Q->OriginEvidence, "caller_selected_commonjs_parameter");
  EXPECT_EQ(A.UnresolvedRequests, 1u); // Bare fs never becomes a captured file.
  std::reverse(Input.begin(), Input.end());
  const auto Again = run();
  EXPECT_EQ(A.ID, Again.ID);
  EXPECT_EQ(R.ID, Again.Associations.at(0).ID);
}

TEST_F(WebPackageInstall, EsmCyclesAndAlternativePathsChooseShortestChain) {
  write("entry.js", "import './mid.js'; import './dep.js';");
  write("mid.js", "export * from './dep.js';");
  write("dep.js",
        "require('./entry.js'); require('child_process').execFile(x);");
  captureAll();
  select({{"entry.js", "module"}, {"mid.js", "module"}, {"dep.js", "script"}});
  const auto A = run();
  ASSERT_EQ(A.Associations.size(), 1u);
  EXPECT_EQ(A.Associations[0].SourceID, Sources[2].ID);
  EXPECT_EQ(A.Associations[0].RequestIDs.size(), 1u);
  EXPECT_TRUE(
      std::any_of(A.Requests.begin(), A.Requests.end(), [](const auto &R) {
        return R.OriginEvidence == "external" &&
               R.Status == "selected_file_candidate";
      }));
}

TEST_F(WebPackageInstall,
       ShadowsWritesAndDynamicScopeNeverAcquireRequireOrigin) {
  for (const auto Text : {"function f(require){require('./dep.js');}",
                          "require('./dep.js'); require=other;",
                          "require('./dep.js'); eval(code);",
                          "require?.('./dep.js'); require('./dep.js', extra);",
                          "require.resolve('./dep.js'); import('./dep.js');",
                          "require(...args); require(path);"}) {
    write("entry.js", Text);
    captureAll();
    select({{"entry.js", "commonjs"}, {"dep.js", "commonjs"}});
    const auto A = run();
    EXPECT_TRUE(A.Associations.empty()) << Text;
    EXPECT_GT(A.UnresolvedRequests, 0u) << Text;
  }
}

TEST_F(WebPackageInstall, DeferredRequireRemainsOnlyASyntacticAssociation) {
  write("entry.js", "function neverCalled(){require('./dep.js');}");
  captureAll();
  select({{"entry.js", "commonjs"}, {"dep.js", "commonjs"}});
  EXPECT_EQ(run().Associations.size(), 1u);
}

TEST_F(WebPackageInstall, CompleteCommandGrammarRejectsOptionsAndShellSyntax) {
  write("inspect", "require('child_process').execFile('CANARY_OTHER');");
  for (const auto Command :
       {"node -r hook entry.js", "node inspect entry.js",
        "node -- inspect entry.js", "node", "node -",
        "node entry.js && node dep.js", "node 'entry.js'", "node entry.js $ARG",
        "node entry.js;other", "NODE_ENV=test node entry.js",
        "node entry.js *.js", "node entry.js #CANARY", "node entry.js %ARG%",
        "node entry.js @CANARY", "other entry.js"}) {
    write("package.json",
          std::string("{\"scripts\":{\"install\":\"") + Command + "\"}}");
    captureAll();
    const auto A = run(false);
    ASSERT_EQ(A.Scripts.size(), 1u);
    EXPECT_NE(A.Scripts[0].CommandStatus, "literal_node_script_candidate")
        << Command;
    EXPECT_TRUE(A.Scripts[0].ArtifactID.empty()) << Command;
    EXPECT_TRUE(A.Associations.empty());
  }
  write("inspect", "require('fs').writeFileSync(x,y);");
  write("package.json",
        R"({"scripts":{"install":"node -- ./inspect --arg CANARY"}})");
  captureAll();
  select({{"inspect", "commonjs"}});
  EXPECT_EQ(run().Associations.size(), 1u);
}

TEST_F(WebPackageInstall, MissingUnselectedAndUnsafeEntriesRemainExplicit) {
  for (const auto &[Command, Status] :
       std::vector<std::pair<std::string, std::string>>{
           {"node missing.js", "not_captured_or_unavailable"},
           {"node entry.js", "entry_source_unselected"},
           {"node ../entry.js", "unsupported_script_path"},
           {"node /entry.js", "unsupported_script_path"},
           {"node sub", "directory_target"}}) {
    write("sub/inner.js", "");
    write("package.json", "{\"scripts\":{\"install\":\"" + Command + "\"}}");
    captureAll();
    EXPECT_EQ(run(false).Scripts.at(0).Status, Status);
  }
  const auto Single = capture((Root / "package.json").string(), Limits{});
  const auto Only =
      analyzePackages(Single, Single.Artifacts.at(0).ID, "package-json");
  EXPECT_EQ(
      analyzePackageInstall(Only, Single, nullptr, {}).Scripts.at(0).Status,
      "no_directory_inventory");
}

TEST_F(WebPackageInstall, LockOnlyPresenceDoesNotInventACommandOrScript) {
  write("package-lock.json", R"({"lockfileVersion":3,"packages":{
    "":{},"node_modules/CANARY":{"version":"1.0.0","hasInstallScript":true}}})");
  captureAll("package-lock.json", "npm-lock");
  const auto A = run(false);
  EXPECT_EQ(A.LifecycleDeclarations, 1u);
  EXPECT_EQ(A.PresenceDeclarations, 1u);
  const auto &R = A.Scripts.back();
  EXPECT_EQ(R.Kind, "install_presence_declaration");
  EXPECT_EQ(R.Status, "manifest_not_supplied");
  EXPECT_TRUE(R.ScriptID.empty());
  EXPECT_FALSE(R.CommandStart);
  EXPECT_TRUE(A.Associations.empty());
}

TEST_F(WebPackageInstall, ExactNamespaceAndSelectionIdentitiesAreRequired) {
  captureAll();
  select({{"entry.js", "commonjs"}});
  auto Other = Captured;
  Other.ID = identity("different-test-namespace", {});
  EXPECT_THROW(analyzePackageInstall(P, Other, &B, Input), Error);
  B.PackageAnalysisID = Other.ID;
  EXPECT_THROW(run(), Error);
  B.PackageAnalysisID = P.ID;
  const auto BehaviorID = B.Sources[0].BehaviorID;
  B.Sources[0].BehaviorID = Other.ID;
  EXPECT_THROW(run(), Error);
  B.Sources[0].BehaviorID = BehaviorID;
  Input[0].SourceBytes += 1;
  EXPECT_THROW(run(), Error);
  Input[0].SourceBytes -= 1;
  Sources[0].BlobHash = Other.ID;
  EXPECT_THROW(run(), Error);
  EXPECT_THROW(analyzePackageInstall(P, Captured, nullptr, Input), Error);
}

TEST_F(WebPackageInstall,
       UnavailableArchiveMembersNeverBecomeMissingFileProof) {
  if (!packageArchiveAvailable())
    GTEST_SKIP() << "Archive path policy unavailable";
  using namespace neverd::web::test;
  const std::string Manifest = R"({"scripts":{"install":"node entry.js"}})";
  for (const auto &[Format, Bytes] :
       std::vector<std::pair<std::string, std::string>>{
           {"tar", finishTar(tarMember("package/package.json", Manifest) +
                             tarMember("package/entry.js", "", '2', "dep.js"))},
           {"zip", ZipFixture({{"package/package.json", Manifest},
                               {"package/entry.js", "inert", 0, 1}})
                       .Bytes},
           {"zip", ZipFixture({{"package/package.json", Manifest},
                               {"package/entry.js", "inert", 99}})
                       .Bytes},
           {"tar", finishTar(tarMember("package/package.json", Manifest))}}) {
    write("input.archive", Bytes);
    const auto Original = capture((Root / "input.archive").string(), Limits{});
    const auto Archive = extractPackageArchive(Original.Artifacts[0], Format);
    const auto N = packageArchiveNamespace(Archive);
    const auto P = analyzePackages(N, Archive.Members[0].ID, "package-json");
    const auto A = analyzePackageInstall(P, N, nullptr, {});
    ASSERT_EQ(A.Scripts.size(), 1u);
    EXPECT_EQ(A.Scripts[0].Status, "not_captured_or_unavailable");
    EXPECT_TRUE(A.Scripts[0].ArtifactID.empty());
    EXPECT_TRUE(A.Associations.empty());
  }
}

TEST_F(WebPackageInstall, CrossPackageFilesRetainIndependentOwnership) {
  write("package-lock.json", R"({"lockfileVersion":3,"packages":{
    "":{},"node_modules/dep":{"version":"1.0.0"}}})");
  write("node_modules/dep/package.json", R"({"version":"1.0.0"})");
  write("node_modules/dep/index.js", "require('child_process').execFile(x);");
  write("entry.js", "require('./node_modules/dep/index.js');");
  captureAll("package-lock.json", "npm-lock");
  select({{"entry.js", "commonjs"}, {"node_modules/dep/index.js", "commonjs"}});
  const auto A = run();
  ASSERT_EQ(A.Associations.size(), 1u);
  EXPECT_NE(A.Associations[0].PackageID, A.Scripts[0].PackageID);
  EXPECT_EQ(A.Associations[0].PackageID, P.Packages[1].ID);
  EXPECT_TRUE(P.Dependencies.empty());
}

TEST_F(WebPackageInstall, EmbeddedDeliveryDoesNotAcquireTheOuterInstallEntry) {
  captureAll();
  const auto S =
      inspectJavaScript(identity("embedded-install-test", {"source"}),
                        "require('fs').writeFile(x,y);", "commonjs");
  const auto B = analyzeNodeBehavior(S);
  const PackageBehaviorInput I{&S, &B, 29};
  const PackageBehaviorOrigin O{S.ArtifactID, file("entry.js").ID,
                                identity("embedded-install-test", {"origin"}),
                                "bun_source"};
  const auto PB = analyzePackageBehavior(P, std::span(&I, 1), std::span(&O, 1));
  const PackageInstallInput SI{&S, 29};
  const auto A = analyzePackageInstall(P, Captured, &PB, std::span(&SI, 1));
  EXPECT_EQ(A.EmbeddedSources, 1u);
  EXPECT_EQ(A.Sources.at(0).Status, "embedded_source_boundary");
  EXPECT_EQ(A.Scripts.at(0).Status, "entry_source_unselected");
  EXPECT_TRUE(A.Associations.empty());
}

TEST_F(WebPackageInstall, UnavailableSourceAndExactFileBoundariesStayVisible) {
  write("entry.js", "require('./dep.js'); require('./dep'); require('dep'); "
                    "require('../outside.js'); require('./dir');");
  write("dep.js", "const =");
  write("dir/index.js", "require('fs').writeFile(x,y);");
  captureAll();
  select({{"entry.js", "commonjs"}, {"dep.js", "commonjs"}});
  const auto A = run();
  EXPECT_EQ(A.UnavailableSources, 1u);
  EXPECT_EQ(A.DirectSources, 2u);
  ASSERT_EQ(A.Requests.size(), 5u);
  EXPECT_EQ(A.Requests[0].Status, "target_source_unavailable");
  EXPECT_EQ(A.Requests[1].Status, "not_captured_or_unavailable");
  EXPECT_EQ(A.Requests[2].Status, "unsupported_specifier");
  EXPECT_EQ(A.Requests[3].Status, "outside_or_unsupported_path");
  EXPECT_EQ(A.Requests[4].Status, "directory_target");
  EXPECT_TRUE(A.Associations.empty());
  write("package.json", R"({"scripts":{"install":"node dep.js"}})");
  captureAll();
  select({{"dep.js", "commonjs"}});
  EXPECT_EQ(run().Scripts.at(0).Status, "entry_source_unavailable");
}

TEST_F(WebPackageInstall, NodeAdapterDoesNotChangeOlderEsmLinkProfile) {
  captureAll();
  select({{"entry.js", "commonjs"}});
  const auto &S = Sources[0];
  const auto Bindings = analyzeSourceBindings(S);
  const auto M = analyzeSourceModules(S, Bindings);
  auto O = analyzeSourceOrigins(S, Bindings, M);
  const auto Old = linkSourceModules(S, M, Captured);
  const auto Node = linkNodeSourceModules(S, M, O, Captured);
  EXPECT_EQ(Old.Profile, "admitted-relative-file-v1");
  EXPECT_EQ(Old.Requests.at(0).Status, "unverified_callee");
  EXPECT_TRUE(Old.Requests.at(0).OriginEvidence.empty());
  EXPECT_EQ(Node.Profile, "admitted-node-relative-file-v1");
  EXPECT_EQ(Node.Requests.at(0).Status, "exact_admitted_file_candidate");
  EXPECT_NE(Node.ID, Old.ID);
  O.ModuleID = identity("other-test-module", {});
  EXPECT_THROW(linkNodeSourceModules(S, M, O, Captured), Error);
}

TEST(WebPackageInstallReal, ClaudeCode21296NpmInstallUsesOnlyItsSelectedEntry) {
  const auto *Directory = std::getenv("NEVERD_CLAUDE_CODE_21296_NPM_DIR");
  if (!Directory || !packageArchiveAvailable() || !packageGzipAvailable())
    GTEST_SKIP()
        << "Optional npm wrapper, archive path policy or zlib unavailable";
  const auto Original =
      capture(std::string(Directory) + "/claude-code-2.1.296.tgz", Limits{});
  ASSERT_EQ(Original.Artifacts[0].BlobHash,
            "f6c375d51d4c22a7a850e185a0d7ddda85173f89dbbeb3262aa7971e3bd670b5");
  const auto Archive = extractPackageArchive(Original.Artifacts[0], "tgz");
  ASSERT_EQ(Archive.Members.size(), 7u);
  const auto N = packageArchiveNamespace(Archive);
  const auto P = analyzePackages(N, Archive.Members[3].ID, "package-json");
  std::vector<SourceAnalysis> Sources;
  std::vector<NodeBehavior> Behaviors;
  std::vector<PackageBehaviorInput> Inputs;
  std::vector<PackageInstallInput> Install;
  Sources.reserve(2);
  Behaviors.reserve(2);
  for (unsigned I = 0; I < 2; ++I) {
    const auto &F = Archive.Members[I];
    Sources.push_back(inspectJavaScript(
        F.ID, F.Content.read(0, F.Content.size(), MaxJavaScriptBytes),
        "commonjs"));
    Behaviors.push_back(analyzeNodeBehavior(Sources.back()));
    Inputs.push_back({&Sources.back(), &Behaviors.back(), F.Content.size()});
    Install.push_back({&Sources.back(), F.Content.size()});
  }
  const auto B = analyzePackageBehavior(P, Inputs, {});
  const auto A = analyzePackageInstall(P, N, &B, Install);
  ASSERT_EQ(A.Scripts.size(), 2u);
  EXPECT_EQ(A.Scripts[0].SourceID, Sources[1].ID);
  EXPECT_EQ(A.Scripts[0].Status, "selected_entry_candidate");
  EXPECT_EQ(A.Scripts[1].Status, "unsupported_command_syntax");
  ASSERT_EQ(A.Associations.size(), 6u);
  EXPECT_EQ(A.Requests.size(), 13u);
  EXPECT_EQ(A.UnresolvedRequests, 13u);
  for (size_t I = 0; I < 6; ++I) {
    const auto &R = A.Associations[I];
    EXPECT_EQ(R.RecordID, Behaviors[1].Records.at(I).ID);
    EXPECT_EQ(R.SourceID, Sources[1].ID);
    EXPECT_EQ(R.EntryFileID, R.FileID);
    EXPECT_TRUE(R.RequestIDs.empty());
  }
  EXPECT_EQ(A.ID,
            "45a9d309ab74aae4575efee5389ac46f10d84abbde32726c22d950185ff9b904");
}

TEST_F(WebPackageInstall, BudgetsRefuseAtomicallyAndNeverPublishPartialChains) {
  captureAll();
  select({{"entry.js", "commonjs"}, {"dep.js", "commonjs"}});
  const auto Script = P.Scripts.at(0);
  P.Scripts.assign(MaxPackageInstallScripts + 1, Script);
  EXPECT_THROW(run(), Error);
  P.Scripts.assign(1, Script);
  P.Scripts[0].Command.assign(MaxPackageInstallSteps, 'a');
  EXPECT_THROW(run(), Error);
  P.Scripts[0] = Script;
  Input[0].SourceBytes = MaxPackageBehaviorBytes + 1;
  EXPECT_THROW(run(), Error);
}
} // namespace
