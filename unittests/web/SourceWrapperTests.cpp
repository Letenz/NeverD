//===- SourceWrapperTests.cpp - Lexical request forwarding boundaries ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exact caller/parameter anchors and refusal of unsupported wrapper flow.
///
//===----------------------------------------------------------------------===//

#include "../../lib/web/Internal.h"
#include "gtest/gtest.h"

#include "neverd/web/Interfaces.h"

#include <array>
#include <cstdlib>
#include <filesystem>

using namespace neverd::web;

namespace {
SourceInterfaces analyze(std::string_view Bytes,
                         std::string_view Goal = "module") {
  const auto S = inspectJavaScript("wrapper", Bytes, Goal);
  EXPECT_EQ(S.ParseStatus, "parsed");
  return analyzeSourceInterfaces(S, analyzeSourceBindings(S),
                                 analyzeSourceValues(S));
}
std::vector<SourceInterface> calls(const SourceInterfaces &A) {
  std::vector<SourceInterface> R;
  for (const auto &I : A.Records)
    if (I.Kind == "fetch_wrapper_call")
      R.push_back(I);
  return R;
}
} // namespace

TEST(WebSourceWrappers, ReorderedParametersRetainIndependentSourceAnchors) {
  const std::string Code = R"(
function request(options, url) { return fetch(url, options); }
request({method:'post', headers:{}, body:payload}, 'https://example.com/a');
)";
  const auto S = inspectJavaScript("wrapper", Code, "module");
  const auto B = analyzeSourceBindings(S);
  const auto A = analyzeSourceInterfaces(S, B, analyzeSourceValues(S));
  ASSERT_EQ(A.Status, "partial");
  ASSERT_EQ(A.WrapperDefinitions, 1u);
  ASSERT_EQ(A.Records.size(), 2u);
  EXPECT_EQ(A.Records[0].Kind, "fetch");
  EXPECT_EQ(A.Records[0].URLStatus, "dynamic_or_unavailable");
  const auto R = calls(A);
  ASSERT_EQ(R.size(), 1u);
  const auto &W = R[0];
  EXPECT_EQ(W.Method, "POST");
  EXPECT_EQ(W.Endpoint.Path, "/a");
  EXPECT_EQ(W.URLArgumentIndex, 1u);
  EXPECT_EQ(W.OptionsArgumentIndex, 0u);
  EXPECT_EQ(W.WrapperStatus, "single_return_lexical_forwarding_candidate");
  EXPECT_EQ(W.WrapperRequestNode, A.Records[0].Node);
  ASSERT_LT(W.WrapperBinding, B.Bindings.size());
  for (const auto Node : {W.Node, W.WrapperNode, W.WrapperRequestNode,
                          W.URLParameterNode, W.OptionsParameterNode, W.URLNode,
                          W.OptionsNode, W.MethodNode, W.BodyNode})
    ASSERT_LT(Node, S.Nodes.size());
  EXPECT_EQ(S.Nodes[W.WrapperNode].Kind, "FunctionDeclaration");
  EXPECT_LT(S.Nodes[W.WrapperRequestNode].End, S.Nodes[W.Node].Start);
  EXPECT_GE(S.Nodes[W.URLNode].Start, S.Nodes[W.Node].Start);
  EXPECT_LE(S.Nodes[W.URLNode].End, S.Nodes[W.Node].End);
  EXPECT_LT(S.Nodes[W.URLParameterNode].End,
            S.Nodes[W.WrapperRequestNode].Start);
  EXPECT_GE(S.Nodes[W.MethodNode].Start, S.Nodes[W.OptionsNode].Start);
  EXPECT_LE(S.Nodes[W.BodyNode].End, S.Nodes[W.OptionsNode].End);
  const auto Again = analyzeSourceInterfaces(S, B, analyzeSourceValues(S));
  EXPECT_EQ(Again.ID, A.ID);
  ASSERT_EQ(Again.Records.size(), A.Records.size());
  for (size_t I = 0; I < A.Records.size(); ++I)
    EXPECT_EQ(Again.Records[I].ID, A.Records[I].ID);
}

TEST(WebSourceWrappers, ConstArrowAndFunctionExpressionsShareFetchOptionRules) {
  const auto A = analyze(R"(
const arrow = url => fetch(url);
const expression = function(url, init) { return fetch(url, init); };
arrow('https://x/a');
expression('https://x/b', {method:'patch'});
expression('https://x/c', {get method(){return 'POST'}});
expression('https://x/d', {method:'POST',method:'GET'});
expression('https://x/e', {});
expression('https://x/f', {method:'TrAcE'});
expression('https://user:password@x/g', {method:'POST'});
)");
  ASSERT_EQ(A.WrapperDefinitions, 2u);
  const auto R = calls(A);
  ASSERT_EQ(R.size(), 7u);
  EXPECT_EQ(R[0].Method, "GET");
  EXPECT_EQ(R[0].OptionsArgumentIndex, NoSourceIndex);
  EXPECT_EQ(R[1].Method, "other");
  EXPECT_EQ(R[2].OptionsStatus, "unresolved");
  EXPECT_EQ(R[3].OptionsStatus, "unresolved");
  EXPECT_EQ(R[4].MethodStatus, "inherited_method_unknown");
  EXPECT_EQ(R[5].MethodStatus, "fetch_forbidden_method");
  EXPECT_EQ(R[6].Endpoint.Status, "fetch_credentials_rejected");
  EXPECT_FALSE(R[6].Endpoint.comparable());
}

TEST(WebSourceWrappers, MissingAndSpreadArgumentsWithholdForwardedRoles) {
  const auto A = analyze(R"(
const request = (url, options) => fetch(url, options);
request();
request('https://x/a');
request(...args);
request('https://x/b', {method:'POST'}, ...extra);
request?.(`https://x/${dynamic}`, unknown);
)");
  const auto R = calls(A);
  ASSERT_EQ(R.size(), 5u);
  EXPECT_EQ(R[0].URLStatus, "wrapper_argument_missing");
  EXPECT_EQ(R[0].MethodStatus, "wrapper_argument_missing");
  EXPECT_EQ(R[0].URLArgumentIndex, NoSourceIndex);
  EXPECT_EQ(R[0].OptionsArgumentIndex, NoSourceIndex);
  EXPECT_EQ(R[1].URLStatus, "constant_string");
  EXPECT_EQ(R[1].Method, "unknown");
  EXPECT_EQ(R[1].OptionsStatus, "wrapper_argument_missing");
  for (size_t I = 2; I < 4; ++I) {
    EXPECT_EQ(R[I].ArgumentsStatus, "spread_unresolved");
    EXPECT_EQ(R[I].Method, "unknown");
    EXPECT_EQ(R[I].URLNode, NoSourceIndex);
    EXPECT_EQ(R[I].OptionsNode, NoSourceIndex);
    EXPECT_EQ(R[I].URLArgumentIndex, NoSourceIndex);
    EXPECT_EQ(R[I].OptionsArgumentIndex, NoSourceIndex);
    EXPECT_FALSE(R[I].Endpoint.comparable());
    EXPECT_NE(R[I].WrapperNode, NoSourceIndex);
    EXPECT_NE(R[I].URLParameterNode, NoSourceIndex);
  }
  EXPECT_TRUE(R[4].Optional);
  EXPECT_EQ(R[4].URLStatus, "unresolved_template");
  EXPECT_EQ(R[4].TemplateHoles, 1u);
}

TEST(WebSourceWrappers, UnsupportedBodiesParametersAndAliasesRemainUnresolved) {
  const char *Cases[] = {
      "let f = u => fetch(u); f('https://x');",
      "var f = u => fetch(u); f('https://x');",
      "const f = (u='https://x') => fetch(u); f();",
      "const f = (...args) => fetch(...args); f('https://x');",
      "const f = ({url}) => fetch(url); f({url:'https://x'});",
      "const f = async u => fetch(u); f('https://x');",
      "function* f(u) {return fetch(u);} f('https://x');",
      "const f = u => {sideEffect();return fetch(u);}; f('https://x');",
      "const f = u => {return awaitable(fetch(u));}; f('https://x');",
      "const f = u => fetch(transform(u)); f('https://x');",
      "const f = (u,i) => fetch(u,i,extra); f('https://x');",
      "const f = u => client.fetch(u); f('https://x');",
      "const f = u => fetch?.(u); f('https://x');",
      "const f = u => fetch(u); const alias=f; alias('https://x');",
      "const f = u => fetch(u); client.f('https://x');"};
  for (const auto *Code : Cases) {
    SCOPED_TRACE(Code);
    EXPECT_TRUE(calls(analyze(Code)).empty());
  }
  const auto Chain =
      calls(analyze("const f=u=>fetch(u); const g=u=>f(u); g('https://x');"));
  ASSERT_EQ(Chain.size(), 1u);
  EXPECT_EQ(Chain[0].URLStatus, "dynamic_or_unavailable");
  EXPECT_FALSE(Chain[0].Endpoint.comparable());
}

TEST(WebSourceWrappers, BindingWritesShadowingAndDuplicateDeclarationsRefuse) {
  const char *Cases[] = {
      "function f(u) {return fetch(u);} f=replacement; f('https://x');",
      "function f(u) {return fetch(u);} function f(u) {} f('https://x');",
      "const f = u => fetch(u); fetch=replacement; f('https://x');",
      "function outer(fetch) {const f=u=>fetch(u); f('https://x');}",
      "const f=u=>fetch(u); function outer(f) {f('https://x');}",
      "const f=u=>fetch(u); function outer(){let f; f('https://x');}",
      "function f(u,u) {return fetch(u);} f('https://x');"};
  for (const auto *Code : Cases) {
    SCOPED_TRACE(Code);
    EXPECT_TRUE(calls(analyze(Code, "script")).empty());
  }
  const auto Eval = analyze("const f=u=>fetch(u); eval(code); f('https://x');");
  EXPECT_EQ(Eval.Status, "unavailable");
  EXPECT_EQ(Eval.Reason, "interface_source_dynamic_eval");
  EXPECT_TRUE(Eval.Records.empty());
  EXPECT_EQ(Eval.WrapperDefinitions, 0u);
}

TEST(WebSourceWrappers, SeparateLexicalDefinitionsNeverMergeBySpelling) {
  const auto A = analyze(R"(
const request=u=>fetch(u);
request('https://x/outer');
function nested() {
  const request=(init,u)=>fetch(u,init);
  request({method:'POST'},'https://x/inner');
}
request('https://x/outer-again');
)");
  const auto R = calls(A);
  ASSERT_EQ(R.size(), 3u);
  EXPECT_EQ(R[0].WrapperBinding, R[2].WrapperBinding);
  EXPECT_NE(R[0].WrapperBinding, R[1].WrapperBinding);
  EXPECT_EQ(R[0].Method, "GET");
  EXPECT_EQ(R[1].Method, "POST");
  EXPECT_EQ(R[1].Endpoint.Path, "/inner");
}

TEST(WebSourceWrappers, ForwardedCandidatesNeverUpgradeHARCorrelation) {
  const auto A = analyze("const f=u=>fetch(u); f('https://x/a');");
  ASSERT_EQ(calls(A).size(), 1u);
  const auto H = inspectHAR("h", R"({"log":{"version":"1.2","entries":[
    {"request":{"method":"GET","url":"https://x/a"}}]}})");
  const auto C = correlateInterfaces(A, H);
  EXPECT_EQ(C.EligibleSources, 0u);
  EXPECT_TRUE(C.Pairs.empty());
}

TEST(WebSourceWrappers, RecordBudgetExhaustionClearsAllForwardingEvidence) {
  std::string Code = "const f=u=>fetch(u);";
  for (uint64_t I = 0; I < MaxInterfaceRecords; ++I)
    Code += "f('https://x/a');";
  const auto A = analyze(Code);
  EXPECT_EQ(A.Status, "budget_exceeded");
  EXPECT_EQ(A.Reason, "interface_record_budget_exceeded");
  EXPECT_TRUE(A.Records.empty());
  EXPECT_EQ(A.PrivateBytes, 0u);
  EXPECT_EQ(A.WrapperDefinitions, 0u);
}

TEST(WebSourceWrappers,
     ClaudeCode21296ComplexDispatchWhenSuppliedStaysUnknown) {
  const auto *Base = std::getenv("NEVERD_CLAUDE_CODE_21296_CROSS_DIR");
  if (!Base)
    GTEST_SKIP() << "Optional NeverD-exported pinned platform sources absent";
  struct Case {
    const char *File;
    const char *Hash;
    uint64_t Size, Count;
    std::array<uint64_t, 3> Offsets, Lengths;
  };
  const Case Cases[] = {
      {"m01905.js",
       "157c010c3056606ebf89818348df35a547fde95dde1b130d2e96e26e80c5e34d",
       54991,
       3,
       {2693, 10795, 11101},
       {47, 29, 36}},
      {"m01920.js",
       "0a1c2aa1424babd95eeec371f67c875b6382db0c891b3b3e90f710f2102b16df",
       198048,
       1,
       {79329, 0, 0},
       {53, 0, 0}}};
  for (const auto &Input : Cases) {
    SCOPED_TRACE(Input.File);
    const auto Path =
        std::filesystem::path(Base) / "darwin-arm64" / "recovered" / Input.File;
    const auto Snapshot = capture(Path.string(), Limits{});
    ASSERT_EQ(Snapshot.Artifacts.size(), 1u);
    const auto &Raw = Snapshot.Artifacts[0];
    ASSERT_EQ(Raw.BlobHash, Input.Hash);
    ASSERT_EQ(Raw.Content.size(), Input.Size);
    const auto Bytes = Raw.Content.read(0, Input.Size, MaxJavaScriptBytes);
    const auto S = inspectJavaScript(Raw.ID, Bytes, "module");
    ASSERT_EQ(S.ParseStatus, "parsed");
    const auto A = analyzeSourceInterfaces(S, analyzeSourceBindings(S),
                                           analyzeSourceValues(S));
    ASSERT_EQ(A.Status, "partial");
    EXPECT_EQ(A.WrapperDefinitions, 0u);
    EXPECT_TRUE(calls(A).empty());
    ASSERT_EQ(A.Records.size(), Input.Count);
    for (size_t I = 0; I < A.Records.size(); ++I) {
      const auto &R = A.Records[I];
      EXPECT_EQ(R.Kind, "fetch");
      EXPECT_EQ(R.URLStatus, "dynamic_or_unavailable");
      EXPECT_EQ(R.OptionsStatus, "unresolved");
      EXPECT_EQ(R.Method, "unknown");
      EXPECT_FALSE(R.Endpoint.comparable());
      ASSERT_LT(R.Node, S.Nodes.size());
      EXPECT_EQ(S.Nodes[R.Node].Start, Input.Offsets[I]);
      EXPECT_EQ(S.Nodes[R.Node].End - S.Nodes[R.Node].Start, Input.Lengths[I]);
    }
  }
}
