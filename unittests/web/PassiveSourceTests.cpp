//===- PassiveSourceTests.cpp - Static and observed method boundaries ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Fixed source grammar, containment, unknown dispatch and private canaries.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/web/PassiveReport.h"

#include <set>

using namespace neverd::web;
namespace {
struct Model {
  SourceAnalysis S;
  SourceBindingAnalysis B;
  SourceNavigation N;
  SourceFeatures F;
  SourceInterfaces I;
  InterfaceSourceRelations R;
  explicit Model(std::string_view Code)
      : S(inspectJavaScript("a", Code, "module")), B(analyzeSourceBindings(S)),
        N(analyzeSourceNavigation(S, B)),
        F(analyzeClientSourceFeatures(S, B, N)),
        I(analyzeSourceInterfaces(S, B, analyzeSourceValues(S))),
        R(analyzeInterfaceSourceRelations(S, B, I, N)) {}
};
} // namespace

TEST(WebPassiveSource, TransformContainmentKeepsExactRequestAndInputAnchors) {
  const std::string Code = R"(async function f() {
 const r = await fetch('https://CANARY.invalid/a', {method:'POST',
  body:btoa(JSON.stringify(CANARY))});
 const value = JSON.parse(await r.text());
 fetch(url, {body:crypto.subtle.encrypt(CANARY_KEY, CANARY_IV, data)});
 fetch(url, {body:zlib.gzip(CANARY)});
})";
  const Model M(Code);
  ASSERT_EQ(M.S.ParseStatus, "parsed");
  ASSERT_EQ(M.R.Status, "partial") << M.R.Reason;
  std::map<std::string, uint64_t> Kinds;
  for (const auto &R : M.R.Records) {
    if (R.Kind.find("transform") == std::string::npos)
      continue;
    ++Kinds[R.MemberKind];
    ASSERT_LT(R.InputNode, M.S.Nodes.size());
    ASSERT_LT(R.Request, M.I.Records.size());
    const auto &At = M.S.Nodes[R.Node];
    EXPECT_NE(Code.substr(At.Start, At.End - At.Start).find('('),
              std::string::npos);
  }
  EXPECT_EQ(Kinds, (std::map<std::string, uint64_t>{{"base64_encode", 1},
                                                    {"json_stringify", 1},
                                                    {"json_parse", 1},
                                                    {"crypto_encrypt", 1},
                                                    {"gzip", 1}}));
}

TEST(WebPassiveSource, DeferredAliasesAndComputedTransformsDoNotGainLinks) {
  const Model M(R"(const r = await fetch(url); const alias = r;
 JSON.parse(await alias.text()); JSON['parse'](await r.text());
 fetch(url, {body:()=>btoa(CANARY)});
 const opts={body:JSON.stringify(CANARY)}; fetch(url,opts);
 fetch(url,{...opts,body:JSON.stringify(CANARY)});
)");
  ASSERT_EQ(M.R.Status, "partial") << M.R.Reason;
  for (const auto &R : M.R.Records)
    EXPECT_EQ(R.Kind.find("transform"), std::string::npos);
}

TEST(WebPassiveSource, MCPMethodsAreCandidatesAndAllFeatureDomainsSurvive) {
  const Model M(R"(function dispatch() {
 const cfg={permissions:{},mcpServers:{},hooks:{PreToolUse:[]}};
 checkPermission(CANARY); executeTool(CANARY); runHooks(CANARY);
 transport.send({jsonrpc:'2.0',method:'tools/call',params:{CANARY:'CANARY'}});
 client.listTools();
})");
  const auto Source = projectPassiveSource(M.S, M.B, M.N, M.F, M.I, M.R);
  const auto Stream = inspectStream(
      "stream",
      R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"CANARY":"CANARY"}}
{"jsonrpc":"2.0","id":2,"method":"tools/list"}
)",
      "mcp-2025-06-18-stdio-jsonl-v1");
  const auto Report = composePassiveReport(&Source, &M.I, nullptr, {}, &Stream);
  EXPECT_EQ(Report.MethodCandidates, 2u);
  std::set<std::string> Domains;
  for (const auto &R : Report.Records) {
    if (R.Kind == "source_client_feature")
      Domains.insert(R.Domain);
    if (R.Kind == "mcp_same_method_candidate") {
      EXPECT_EQ(R.EvidenceClass, "candidate_correlation");
      EXPECT_FALSE(R.ParentID.empty());
      EXPECT_FALSE(R.RelatedID.empty());
    }
  }
  EXPECT_EQ(Domains, (std::set<std::string>{"tool", "configuration",
                                            "permission", "mcp", "hook"}));
  const auto Plain =
      inspectStream("stream", R"({"jsonrpc":"2.0","id":1,"method":"tools/call"}
)",
                    "jsonl-metadata-v1");
  EXPECT_EQ(
      composePassiveReport(&Source, &M.I, nullptr, {}, &Plain).MethodCandidates,
      0u);
}

TEST(WebPassiveSource, UnknownDuplicateSpreadAndDynamicMethodsNeverMatch) {
  const Model M(R"(send({jsonrpc:'2.0',method:CANARY});
 send({jsonrpc:'2.0',method:'tools/call',...CANARY});
 send({jsonrpc:'2.0',method:'tools/call',method:'tools/list'});
 send({jsonrpc:'2.0',method:'CANARY_PRIVATE'});
 client.callTool(...CANARY);
)");
  const auto Source = projectPassiveSource(M.S, M.B, M.N, M.F, M.I, M.R);
  const auto Stream =
      inspectStream("s", R"({"jsonrpc":"2.0","id":1,"method":"tools/call"}
)",
                    "mcp-2025-06-18-stdio-jsonl-v1");
  EXPECT_EQ(composePassiveReport(&Source, &M.I, nullptr, {}, &Stream)
                .MethodCandidates,
            0u);
  for (const auto &R : Source.Records)
    EXPECT_EQ(R.Method.find("CANARY"), std::string::npos);
}
