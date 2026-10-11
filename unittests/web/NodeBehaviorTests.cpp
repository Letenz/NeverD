//===- NodeBehaviorTests.cpp - Static Node API evidence contracts -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercise exact import chains, uncertain overloads and bounded source joins.
///
//===----------------------------------------------------------------------===//

#include "Internal.h"
#include "gtest/gtest.h"

#include "neverd/web/Bun.h"
#include "neverd/web/Error.h"
#include "neverd/web/NodeBehavior.h"
#include "neverd/web/PackageArchive.h"
#include "neverd/web/SourceOrigins.h"

#include <algorithm>
#include <cstdlib>
#include <set>

namespace {
using namespace neverd::web;

struct Inspection {
  std::string Text;
  SourceAnalysis Source;
  NodeBehavior Behavior;
  explicit Inspection(std::string Input, std::string_view Type = "commonjs")
      : Text(std::move(Input)) {
    Source = inspectJavaScript(identity("node-test", {Text}), Text, Type);
    Behavior = analyzeNodeBehavior(Source);
  }
  std::vector<std::string> APIs() const {
    std::vector<std::string> Result;
    for (const auto &R : Behavior.Records)
      Result.push_back(R.API);
    return Result;
  }
};

TEST(WebNodeBehavior, ExactModuleChainsCoverDistinctOperationFamilies) {
  Inspection I(R"JS(
    const cp = require('node:child_process');
    const launch = cp.execFile; launch('CANARY_TOOL', options);
    const {spawn: start} = cp; start('CANARY_TOOL', []);
    const fs = require('fs'); fs.writeFileSync('CANARY_PATH', 'CANARY_DATA');
    fs.promises.rename('CANARY_OLD', 'CANARY_NEW');
    const {unlink: remove} = require('node:fs/promises'); remove('CANARY_PATH');
    require('http').request(options); require('node:https').get('CANARY_URL');
    require('node:process').dlopen(module, 'CANARY_ADDON');
  )JS");
  ASSERT_EQ(I.Behavior.Status, "partial") << I.Behavior.Reason;
  EXPECT_EQ(I.APIs(),
            (std::vector<std::string>{
                "child_process.execFile", "child_process.spawn",
                "fs.writeFileSync", "fs.promises.rename", "fs.promises.unlink",
                "http.request", "https.get", "process.dlopen"}));
  for (const auto &R : I.Behavior.Records) {
    EXPECT_EQ(R.RequestID.size(), 64u);
    EXPECT_EQ(R.RequestNodeID.size(), 64u);
    EXPECT_EQ(R.ID.size(), 64u);
    EXPECT_LT(R.Start, R.End);
    EXPECT_EQ(R.API.find("CANARY"), std::string::npos);
  }
  EXPECT_EQ(analyzeNodeBehavior(I.Source).ID, I.Behavior.ID);
}

TEST(WebNodeBehavior, ESMDefaultProjectionRetainsItsExactImportOccurrence) {
  Inspection I(R"JS(
    import fs from 'node:fs'; fs.writeFile(path, data);
    import {default as other, writeFile as named} from 'fs';
    other.writeFile(path, data); named(path, data);
    import * as space from 'fs'; space.writeFile(path, data);
    space.default.writeFile(path, data); fs.default.writeFile(path, data);
    import promises from 'node:fs/promises'; promises.writeFile(path, data);
  )JS",
               "module");
  ASSERT_EQ(I.Behavior.Status, "partial") << I.Behavior.Reason;
  ASSERT_EQ(I.Behavior.Records.size(), 5u);
  std::set<std::string> Imports;
  for (const auto &R : I.Behavior.Records) {
    EXPECT_FALSE(R.ImportID.empty());
    EXPECT_EQ(R.OriginEvidence, "esm_import_syntax");
    Imports.insert(R.ImportID);
  }
  EXPECT_EQ(Imports.size(), 5u);
  Inspection CJS("const fs=require('fs');fs.default.writeFile(p,d);"
                 "fs.default.default.writeFile(p,d);");
  EXPECT_TRUE(CJS.Behavior.Records.empty());
}

TEST(WebNodeBehavior, MethodTablesDoNotInventPromiseOrSyncForms) {
  Inspection I(R"JS(
    const fs=require('fs'), p=require('fs/promises');
    fs.write(fd,data); fs.createWriteStream(path); fs.writeFileSync(path,data);
    fs.promises.writeFile(path,data); p.appendFile(path,data);
    fs.createWriteStreamSync(path); fs.promises.write(fd,data);
    fs.promises.createWriteStream(path); fs.promises.writeFileSync(path,data);
    p.write(fd,data); p.createWriteStream(path); p.appendFileSync(path,data);
    fs.open(path,flags); fs.readFile(path,callback);
    require('fs-other').writeFile(path,data); require('node:node:fs').rm(path);
  )JS");
  EXPECT_EQ(I.APIs(),
            (std::vector<std::string>{
                "fs.write", "fs.createWriteStream", "fs.writeFileSync",
                "fs.promises.writeFile", "fs.promises.appendFile"}));
}

TEST(WebNodeBehavior, ShadowsWritesDynamicScopeAndReturnedObjectsStayUnknown) {
  Inspection I(R"JS(
    const cp=require('child_process'); cp.exec(command);
    function f(cp) { cp.exec(command); }
    function g(require) { require('child_process').exec(command); }
    let alias=cp; alias=replacement; alias.spawn(command);
    with (object) { cp.exec(command); }
    wrap(cp).exec(command); cp[dynamic](command);
    const {exec=fallback,...rest}=cp; exec(command); rest.exec(command);
    new cp.exec(command); const instance=new cp.Other(); instance.exec(command);
  )JS");
  ASSERT_EQ(I.Behavior.Status, "partial") << I.Behavior.Reason;
  EXPECT_EQ(I.APIs(), std::vector<std::string>{"child_process.exec"});
  EXPECT_GT(I.Behavior.ConstructionCalls, 0u);
  EXPECT_GT(I.Behavior.UnattributedCalls, 0u);
  Inspection Eval("const cp=require('child_process');eval(code);cp.exec(cmd);");
  EXPECT_EQ(Eval.Behavior.Status, "unavailable");
  EXPECT_EQ(Eval.Behavior.OriginReason, "source_origin_dynamic_eval");
  EXPECT_TRUE(Eval.Behavior.Records.empty());
  Inspection Changed("require=replacement;require('child_process').exec(cmd);");
  EXPECT_TRUE(Changed.Behavior.Records.empty());
}

TEST(WebNodeBehavior, ArgumentAnchorsDoNotAssignOverloadedParameterRoles) {
  Inspection I(R"JS(
    const cp=require('child_process');
    cp.execFile('tool', opts); cp.execFile('tool', ['arg'], opts, callback);
    cp.execFile('tool', callback); cp.execFile('tool'); cp.execFile();
    cp.spawn?.('tool', ...options); cp.fork('module', {silent:true});
    require('http').request(opts);
    cp.exec('tool', 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17);
  )JS");
  const auto &R = I.Behavior.Records;
  ASSERT_EQ(R.size(), 9u);
  EXPECT_EQ(R[0].ArgumentCount, 2u);
  EXPECT_EQ(R[1].ArgumentCount, 4u);
  EXPECT_EQ(R[2].ArgumentCount, 2u);
  EXPECT_EQ(R[3].ArgumentCount, 1u);
  EXPECT_EQ(R[4].ArgumentCount, 0u);
  EXPECT_TRUE(R[5].Spread);
  EXPECT_TRUE(R[5].Optional);
  EXPECT_TRUE(R[5].Arguments[1].Spread);
  EXPECT_EQ(R[7].ArgumentCount, 1u);
  EXPECT_EQ(R[8].ArgumentCount, 18u);
  EXPECT_EQ(R[8].Arguments.size(), MaxNodeBehaviorArguments);
  for (const auto &Record : R)
    for (unsigned J = 0; J < Record.Arguments.size(); ++J) {
      EXPECT_EQ(Record.Arguments[J].Ordinal, J);
      EXPECT_EQ(Record.Arguments[J].NodeID.size(), 64u);
    }
}

TEST(WebNodeBehavior, EncodedDataRequiresTheExactPrimitiveStringOverload) {
  Inspection I(R"JS(
    const {Buffer:B}=require('node:buffer');
    B.from('QQ==','BASE64'); B.from('QQ==','base64url'); B.from('00','hex');
    B.from(input,'base64'); B.from('QQ==',encoding); B.from('plain','utf8');
    B.from('QQ==','base64',extra); B.from(...args);
    Buffer.from('QQ==','base64'); require('buffer').from('QQ==','base64');
  )JS");
  const auto &R = I.Behavior.Records;
  ASSERT_EQ(R.size(), 8u);
  for (unsigned J = 0; J < 3; ++J)
    EXPECT_EQ(R[J].Kind, "encoded_data_conversion_call");
  EXPECT_EQ(R[0].Encoding, "base64");
  EXPECT_EQ(R[1].Encoding, "base64url");
  EXPECT_EQ(R[2].Encoding, "hex");
  EXPECT_EQ(R[3].Encoding, "overload_unresolved");
  EXPECT_EQ(R[4].Encoding, "encoding_unresolved");
  EXPECT_EQ(R[5].Encoding, "outside_profile");
  EXPECT_EQ(R[6].Encoding, "overload_unresolved");
  EXPECT_EQ(R[7].Encoding, "overload_unresolved");
}

TEST(WebNodeBehavior, IncompleteSourcesAndBudgetsNeverPublishCleanResults) {
  Inspection Broken("const =");
  EXPECT_EQ(Broken.Behavior.Status, "unavailable");
  EXPECT_TRUE(Broken.Behavior.Records.empty());
  Inspection None("function ordinary(x){return x+1;}");
  EXPECT_EQ(None.Behavior.Status, "partial");
  EXPECT_TRUE(None.Behavior.Records.empty());
  auto Invalid = None.Source;
  Invalid.Nodes[0].Children.front().Index = UINT32_MAX;
  EXPECT_EQ(analyzeNodeBehavior(Invalid).Status, "unavailable");
  std::string Many = "const cp=require('child_process');";
  for (uint64_t J = 0; J <= MaxNodeBehaviorRecords; ++J)
    Many += "cp.exec('x');";
  Inspection Limited(std::move(Many));
  EXPECT_EQ(Limited.Behavior.Status, "budget_exceeded")
      << Limited.Behavior.Reason;
  EXPECT_EQ(Limited.Behavior.Reason, "node_behavior_record_budget_exceeded");
  EXPECT_TRUE(Limited.Behavior.Records.empty());
}

TEST(WebNodeBehavior, ClaudeCode21296Module1897WhenSupplied) {
  const auto *Path = std::getenv("NEVERD_CLAUDE_CODE_21296_ELF");
  if (!Path)
    GTEST_SKIP() << "Optional hash-pinned standalone not supplied";
  const auto Snapshot = capture(Path, Limits{});
  ASSERT_EQ(Snapshot.Artifacts.size(), 1u);
  ASSERT_EQ(Snapshot.Artifacts.front().BlobHash,
            "24972e3bc859fab2b46ed4c1e51f7d6130f06d3bd550811a114640de3370d0de");
  const auto E = extractBun(Snapshot.Artifacts.front());
  const auto &M = E.Modules.at(1897);
  const auto Bytes = bunSourceBytes(E, M, MaxJavaScriptBytes);
  const auto S = inspectJavaScript(M.SourceArtifactID, Bytes,
                                   M.Format == 1 ? "module" : "commonjs");
  const auto B = analyzeNodeBehavior(S);
  ASSERT_EQ(B.Status, "partial") << B.Reason << ": " << B.OriginReason;
  EXPECT_TRUE(B.Records.empty());
  EXPECT_EQ(B.UnattributedCalls, 1461u);
  RecordProperty("recognized_calls", int(B.Records.size()));
  RecordProperty("unattributed_calls", int(B.UnattributedCalls));
  std::set<std::string> APIs;
  for (const auto &R : B.Records)
    APIs.insert(R.API);
  RecordProperty("fixed_api_classes", int(APIs.size()));
}

TEST(WebNodeBehavior, ClaudeCode21296NPMWrapperWhenSupplied) {
  const auto *Directory = std::getenv("NEVERD_CLAUDE_CODE_21296_NPM_DIR");
  if (!Directory || !packageGzipAvailable())
    GTEST_SKIP() << "Optional hash-pinned npm wrapper or zlib not supplied";
  const auto Snapshot =
      capture(std::string(Directory) + "/claude-code-2.1.296.tgz", Limits{});
  ASSERT_EQ(Snapshot.Artifacts.front().BlobHash,
            "f6c375d51d4c22a7a850e185a0d7ddda85173f89dbbeb3262aa7971e3bd670b5");
  const auto Archive = extractPackageArchive(Snapshot.Artifacts.front(), "tgz");
  ASSERT_EQ(Archive.Members.size(), 7u);
  const auto Packages = analyzePackages(packageArchiveNamespace(Archive),
                                        Archive.Members[3].ID, "package-json");
  ASSERT_EQ(Packages.Packages.size(), 1u);
  std::vector<SourceAnalysis> Sources;
  std::vector<NodeBehavior> Behaviors;
  std::vector<PackageBehaviorInput> Inputs;
  Sources.reserve(2);
  Behaviors.reserve(2);
  for (unsigned I = 0; I < 2; ++I) {
    const auto &Member = Archive.Members[I];
    const auto Bytes =
        Member.Content.read(0, Member.Content.size(), MaxJavaScriptBytes);
    Sources.push_back(inspectJavaScript(Member.ID, Bytes, "commonjs"));
    Behaviors.push_back(analyzeNodeBehavior(Sources.back()));
    ASSERT_EQ(Behaviors.back().Status, "partial") << Behaviors.back().Reason;
    Inputs.push_back({&Sources.back(), &Behaviors.back(), Bytes.size()});
    RecordProperty("member_" + std::to_string(I) + "_source_hash",
                   Sources.back().BlobHash);
    RecordProperty("member_" + std::to_string(I) + "_calls",
                   int(Behaviors.back().Records.size()));
    RecordProperty("member_" + std::to_string(I) + "_unattributed",
                   int(Behaviors.back().UnattributedCalls));
  }
  const auto A = analyzePackageBehavior(Packages, Inputs, {});
  EXPECT_EQ(A.Sources.size(), 2u);
  EXPECT_EQ(A.UnavailableSources, 0u);
  EXPECT_EQ(A.RecordCount, 8u);
  std::set<std::string> APIs;
  for (const auto &S : A.Sources) {
    EXPECT_EQ(S.Relation, "direct_file");
    EXPECT_FALSE(S.PackageID.empty());
    for (const auto &R : S.Records)
      APIs.insert(R.API);
  }
  RecordProperty("recognized_calls", int(A.RecordCount));
  RecordProperty("fixed_api_classes", int(APIs.size()));
  EXPECT_EQ(APIs, (std::set<std::string>{"child_process.spawnSync",
                                         "fs.unlinkSync", "fs.copyFileSync",
                                         "fs.writeFileSync", "fs.chmodSync"}));
}

} // namespace
