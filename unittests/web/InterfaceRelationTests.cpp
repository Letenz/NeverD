//===- InterfaceRelationTests.cpp - Request relationship boundaries ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent source anchors, lexical exclusions and atomic graph bounds.
///
//===----------------------------------------------------------------------===//

#include "../../lib/web/Internal.h"
#include "gtest/gtest.h"

#include "neverd/web/InterfaceRelations.h"

#include <cstdlib>
#include <filesystem>

using namespace neverd::web;

namespace {
struct Models {
  SourceAnalysis S;
  SourceBindingAnalysis B;
  SourceInterfaces I;
  SourceNavigation N;

  explicit Models(std::string_view Code)
      : S(inspectJavaScript("a", Code, "module")), B(analyzeSourceBindings(S)),
        I(analyzeSourceInterfaces(S, B, analyzeSourceValues(S))),
        N(analyzeSourceNavigation(S, B)) {}

  InterfaceSourceRelations
  relations(InterfaceRelationLimits Limits = {}) const {
    return analyzeInterfaceSourceRelations(S, B, I, N, Limits);
  }
};

const InterfaceSourceRelation *
find(const InterfaceSourceRelations &A, std::string_view Kind,
     std::string_view Member = "not_applicable") {
  for (const auto &R : A.Records)
    if (R.Kind == Kind && R.MemberKind == Member)
      return &R;
  return nullptr;
}

std::string_view anchor(const Models &M, std::string_view Code, uint32_t Node) {
  const auto &N = M.S.Nodes.at(Node);
  return Code.substr(N.Start, N.End - N.Start);
}
} // namespace

TEST(WebInterfaceRelations, ExactReturnAwaitBindingAndConsumerAnchors) {
  const std::string Code = R"(
async function load() {
  const result = await fetch('https://private.invalid/a');
  result.json();
  return (await fetch('https://private.invalid/b')).text();
}
const wrapper = url => fetch(url);
)";
  const Models M(Code);
  ASSERT_EQ(M.S.ParseStatus, "parsed");
  ASSERT_EQ(M.I.Records.size(), 3u);
  const auto A = M.relations();
  ASSERT_EQ(A.Status, "partial") << A.Reason;
  ASSERT_EQ(A.Records.size(), 7u);
  const auto *Binding = find(A, "await_result_const_binding");
  ASSERT_NE(Binding, nullptr);
  EXPECT_EQ(anchor(M, Code, Binding->Node),
            "result = await fetch('https://private.invalid/a')");
  EXPECT_EQ(anchor(M, Code, Binding->ReceiverNode), "result");
  EXPECT_NE(Binding->Binding, NoSourceIndex);
  const auto *Use = find(A, "bound_result_member_call", "json");
  ASSERT_NE(Use, nullptr);
  EXPECT_EQ(anchor(M, Code, Use->Node), "result.json()");
  EXPECT_EQ(Use->Request, Binding->Request);
  EXPECT_EQ(Use->Binding, Binding->Binding);
  const auto *Inline = find(A, "awaited_result_member_call", "text");
  ASSERT_NE(Inline, nullptr);
  EXPECT_EQ(anchor(M, Code, Inline->ReceiverNode),
            "await fetch('https://private.invalid/b')");
  EXPECT_EQ(Inline->Request, 1u);
  const auto *Return = find(A, "direct_request_return");
  ASSERT_NE(Return, nullptr);
  EXPECT_EQ(anchor(M, Code, Return->Node), "url => fetch(url)");
  EXPECT_EQ(Return->Request, 2u);
  for (const auto &R : A.Records) {
    ASSERT_LT(R.Request, M.I.Records.size());
    if (R.FunctionNode == NoSourceIndex)
      continue;
    const auto &F = M.S.Nodes[R.FunctionNode];
    const auto &At = M.S.Nodes[R.Node];
    EXPECT_LE(F.Start, At.Start);
    EXPECT_GE(F.End, At.End);
  }
}

TEST(WebInterfaceRelations,
     AliasesShadowingMutationAndComputedUsesStayExcluded) {
  const Models M(R"(
let mutable = await fetch(url);
mutable.json();
const result = await fetch(url);
const alias = result;
alias.text();
result['json']();
new result.json();
function shadow(result) { result.json(); }
result?.json();
)");
  const auto A = M.relations();
  ASSERT_EQ(A.Status, "partial") << A.Reason;
  ASSERT_EQ(A.Records.size(), 2u);
  EXPECT_NE(find(A, "await_result_const_binding"), nullptr);
  EXPECT_NE(find(A, "bound_result_member_call", "json"), nullptr);
  const Models Written(
      "const r = await fetch(url); r = replacement; r.json();");
  EXPECT_EQ(Written.I.Records.size(), 1u);
  EXPECT_TRUE(Written.relations().Records.empty());
}

TEST(WebInterfaceRelations, ThenPreservesSyntaxWithoutCallbackParameterRoles) {
  const Models M(R"(
fetch(a).then(r => r.json());
fetch(b).then(handler);
fetch(c).then(...PRIVATE_ARGS, handler);
fetch(d).then(0);
fetch(e)['then'](handler);
new (fetch(f).then)(handler);
)");
  const auto A = M.relations();
  ASSERT_EQ(A.Status, "partial") << A.Reason;
  ASSERT_EQ(A.Records.size(), 4u);
  EXPECT_EQ(A.Records[0].HandlerStatus, "function_expression_syntax");
  EXPECT_EQ(A.Records[1].HandlerStatus, "identifier_syntax");
  EXPECT_EQ(A.Records[2].ArgumentsStatus, "spread_unresolved");
  EXPECT_EQ(A.Records[2].HandlerStatus, "spread_unresolved");
  EXPECT_EQ(A.Records[2].HandlerNode, NoSourceIndex);
  EXPECT_EQ(A.Records[3].HandlerStatus, "value_syntax");
  for (const auto &R : A.Records) {
    EXPECT_EQ(R.Kind, "then_callback_syntax");
    EXPECT_EQ(R.Binding, NoSourceIndex);
    EXPECT_EQ(R.MemberKind, "then");
  }
}

TEST(WebInterfaceRelations, UnresolvedMemberDispatchAndSpreadsKeepOriginOnly) {
  const Models M("const r = await client.fetch(...PRIVATE_ARGS); "
                 "r.json(...PRIVATE_FIELDS);");
  const auto A = M.relations();
  ASSERT_EQ(A.Status, "partial") << A.Reason;
  ASSERT_EQ(A.Records.size(), 2u);
  ASSERT_EQ(M.I.Records.size(), 1u);
  EXPECT_EQ(M.I.Records[0].Kind, "fetch_member_call");
  EXPECT_EQ(M.I.Records[0].URLNode, NoSourceIndex);
  EXPECT_EQ(M.I.Records[0].Method, "unknown");
  EXPECT_FALSE(M.I.Records[0].Endpoint.comparable());
  const auto *Use = find(A, "bound_result_member_call", "json");
  ASSERT_NE(Use, nullptr);
  EXPECT_EQ(Use->ArgumentsStatus, "spread_unresolved");
  EXPECT_EQ(Use->HandlerNode, NoSourceIndex);
}

TEST(WebInterfaceRelations, ExactAndShortBudgetsAreAtomicAndIndependent) {
  const Models M("async function f() { const r = await fetch(url); "
                 "r.json(); return fetch(url); }");
  const auto A = M.relations();
  ASSERT_EQ(A.Status, "partial") << A.Reason;
  ASSERT_GT(A.Steps, 0u);
  ASSERT_GT(A.Records.size(), 0u);
  const auto Exact = M.relations({A.Records.size(), A.Steps});
  ASSERT_EQ(Exact.Status, "partial") << Exact.Reason;
  EXPECT_EQ(Exact.Records.size(), A.Records.size());
  const auto ShortWork = M.relations({A.Records.size(), A.Steps - 1});
  EXPECT_EQ(ShortWork.Status, "budget_exceeded");
  EXPECT_TRUE(ShortWork.Records.empty());
  const auto ShortRecords = M.relations({A.Records.size() - 1, A.Steps});
  EXPECT_EQ(ShortRecords.Status, "budget_exceeded");
  EXPECT_TRUE(ShortRecords.Records.empty());
  EXPECT_EQ(M.I.Records.size(), 2u);
  EXPECT_EQ(M.I.Status, "partial");
  const auto Invalid = M.relations({MaxInterfaceSourceRelations + 1, A.Steps});
  EXPECT_EQ(Invalid.Status, "unavailable");
  EXPECT_EQ(Invalid.Reason, "invalid_interface_relation_limits");
}

TEST(WebInterfaceRelations, ForeignMalformedAndUnavailableModelsFailClearly) {
  const Models M("const r = await fetch(url); r.json();");
  const Models Other("fetch(other);");
  EXPECT_EQ(analyzeInterfaceSourceRelations(M.S, M.B, M.I, Other.N).Status,
            "unavailable");
  auto BadNavigation = M.N;
  ASSERT_FALSE(BadNavigation.Calls.empty());
  BadNavigation.Calls.push_back(BadNavigation.Calls[0]);
  const auto Bad =
      analyzeInterfaceSourceRelations(M.S, M.B, M.I, BadNavigation);
  EXPECT_EQ(Bad.Status, "unavailable");
  EXPECT_TRUE(Bad.Records.empty());
  auto Unavailable = M.N;
  Unavailable.Status = "budget_exceeded";
  const auto Refused =
      analyzeInterfaceSourceRelations(M.S, M.B, M.I, Unavailable);
  EXPECT_EQ(Refused.Reason, "interface_relation_navigation_unavailable");
  EXPECT_TRUE(Refused.Records.empty());
  auto Foreign = M.I;
  Foreign.Records[0].Node = M.S.Nodes.size();
  EXPECT_EQ(analyzeInterfaceSourceRelations(M.S, M.B, Foreign, M.N).Status,
            "unavailable");
  const Models Eval("eval(code); fetch(url);");
  EXPECT_TRUE(Eval.relations().Records.empty());
  EXPECT_EQ(M.I.Records.size(), 1u);
}

TEST(WebInterfaceRelations, ClaudeCode21296MemberRequestContextWhenSupplied) {
  const auto *Base = std::getenv("NEVERD_CLAUDE_CODE_21296_CROSS_DIR");
  if (!Base)
    GTEST_SKIP() << "Optional pinned NeverD-exported sources not supplied";
  const auto Path =
      std::filesystem::path(Base) / "darwin-arm64" / "recovered" / "m00928.js";
  const auto Snapshot = capture(Path.string(), Limits{});
  ASSERT_EQ(Snapshot.Artifacts.size(), 1u);
  const auto &Raw = Snapshot.Artifacts[0];
  ASSERT_EQ(Raw.BlobHash,
            "a5e52526ae954aa74deb80cf1bb2372d1f4b77e1886bba69d55e30c51efa2cd2");
  const auto Code = Raw.Content.read(0, Raw.Content.size(), MaxJavaScriptBytes);
  const Models M(Code);
  ASSERT_EQ(M.I.Records.size(), 1u);
  const auto A = M.relations();
  ASSERT_EQ(A.Status, "partial") << A.Reason;
  const auto *Context = find(A, "request_function_containment");
  ASSERT_NE(Context, nullptr);
  EXPECT_EQ(Context->Request, 0u);
  EXPECT_EQ(M.S.Nodes[Context->Node].Start, 150734u);
  EXPECT_EQ(M.S.Nodes[Context->Node].End - M.S.Nodes[Context->Node].Start, 36u);
  ASSERT_LT(Context->FunctionNode, M.S.Nodes.size());
  EXPECT_LE(M.S.Nodes[Context->FunctionNode].Start, 150734u);
  EXPECT_GE(M.S.Nodes[Context->FunctionNode].End, 150770u);
  ASSERT_EQ(A.Records.size(), 2u);
  const auto *Return = find(A, "direct_request_return");
  ASSERT_NE(Return, nullptr);
  EXPECT_EQ(Return->FunctionNode, Context->FunctionNode);
  EXPECT_EQ(M.S.Nodes[Return->Node].Start, 150728u);
  EXPECT_EQ(M.S.Nodes[Return->Node].End - M.S.Nodes[Return->Node].Start, 42u);
  RecordProperty("request_relations", A.Records.size());
}
