//===- SourceXHRTests.cpp - XMLHttpRequest syntax evidence boundaries --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Inert C++ fixtures for lexical identity, spread, callbacks and refusal.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/web/Interfaces.h"

using namespace neverd::web;

namespace {
SourceInterfaces analyze(std::string_view Code,
                         std::string_view Type = "module") {
  const auto S = inspectJavaScript("xhr-fixture", Code, Type);
  EXPECT_EQ(S.ParseStatus, "parsed");
  return analyzeSourceInterfaces(S, analyzeSourceBindings(S),
                                 analyzeSourceValues(S));
}
} // namespace

TEST(WebSourceXHR, ConstructorAndConstReceiverSitesHaveExactAnchors) {
  const std::string Code = R"(
const request = new XMLHttpRequest();
request.open('post', 'https://example.com/' + 'api', true, 'user', 'password');
request.setRequestHeader('Authorization', 'secret');
request.send(payload);
request.onload = function () { read(request.responseText); };
request.addEventListener('readystatechange', event => consume(event));
new XMLHttpRequest().open('GET', `https://example.com/${'direct'}`);
)";
  const auto S = inspectJavaScript("xhr-fixture", Code, "module");
  const auto B = analyzeSourceBindings(S);
  const auto A = analyzeSourceInterfaces(S, B, analyzeSourceValues(S));
  ASSERT_EQ(A.Status, "partial");
  ASSERT_EQ(A.Records.size(), 6u);
  const auto &Open = A.Records[0];
  EXPECT_EQ(Open.Kind, "xhr_open");
  EXPECT_EQ(Open.Method, "POST");
  EXPECT_EQ(Open.Endpoint.Path, "/api");
  EXPECT_EQ(Open.CredentialArgumentsStatus, "present");
  EXPECT_EQ(Open.ArgumentsStatus, "positional");
  EXPECT_NE(Open.AsyncNode, NoSourceIndex);
  ASSERT_LT(Open.ReceiverBinding, B.Bindings.size());
  for (size_t I = 0; I != 5; ++I) {
    const auto &R = A.Records[I];
    EXPECT_EQ(R.ReceiverBinding, Open.ReceiverBinding);
    EXPECT_EQ(R.ConstructorNode, Open.ConstructorNode);
    EXPECT_EQ(R.ReceiverStatus, "const_binding_syntax");
    ASSERT_LT(R.Node, S.Nodes.size());
    const auto &N = S.Nodes[R.Node];
    EXPECT_EQ(Code.substr(N.Start, 7), "request");
    EXPECT_GT(N.End, N.Start);
  }
  EXPECT_EQ(A.Records[1].Kind, "xhr_set_request_header");
  EXPECT_EQ(A.Records[1].HeaderKind, "sensitive");
  EXPECT_EQ(A.Records[2].Kind, "xhr_send");
  EXPECT_NE(A.Records[2].BodyNode, NoSourceIndex);
  EXPECT_EQ(A.Records[3].HandlerStatus, "function_syntax");
  EXPECT_EQ(A.Records[4].EventKind, "readystatechange");
  EXPECT_EQ(A.Records[4].HandlerStatus, "function_syntax");
  EXPECT_EQ(A.Records[5].ReceiverStatus, "direct_constructor_syntax");
  EXPECT_EQ(A.Records[5].ReceiverBinding, NoSourceIndex);
  EXPECT_EQ(A.Records[5].Endpoint.Path, "/direct");
  for (const auto &R : A.Records) {
    ASSERT_LT(R.ConstructorNode, S.Nodes.size());
    const auto &N = S.Nodes[R.ConstructorNode];
    EXPECT_EQ(Code.substr(N.Start, N.End - N.Start), "new XMLHttpRequest()");
    for (auto I : {R.URLNode, R.MethodNode, R.HeaderNameNode, R.HeaderValueNode,
                   R.BodyNode, R.HandlerNode, R.AsyncNode}) {
      if (I == NoSourceIndex)
        continue;
      ASSERT_LT(I, S.Nodes.size());
      EXPECT_GE(S.Nodes[I].Start, S.Nodes[R.Node].Start);
      EXPECT_LE(S.Nodes[I].End, S.Nodes[R.Node].End);
    }
  }
}

TEST(WebSourceXHR, OpenDoesNotInventRequiredArgumentsOrResolveDynamicURLs) {
  const auto A = analyze(R"(
const x = new XMLHttpRequest();
x.open();
x.open('GET');
x.open('TRACE', 'https://x/a');
x.open('patch', `/route/${unknown}`);
x.open(method, url);
x.open('GET', 'https://user:password@x/a?token=secret');
x.send();
)");
  ASSERT_EQ(A.Records.size(), 7u);
  EXPECT_EQ(A.Records[0].Method, "unknown");
  EXPECT_EQ(A.Records[0].MethodStatus, "missing_required_argument");
  EXPECT_EQ(A.Records[1].URLStatus, "missing_required_argument");
  EXPECT_EQ(A.Records[2].MethodStatus, "xhr_forbidden_method");
  EXPECT_EQ(A.Records[3].Method, "other");
  EXPECT_EQ(A.Records[3].URLStatus, "unresolved_template");
  EXPECT_EQ(A.Records[3].TemplateHoles, 1u);
  EXPECT_EQ(A.Records[4].MethodStatus, "dynamic_or_unavailable");
  EXPECT_EQ(A.Records[4].URLStatus, "dynamic_or_unavailable");
  EXPECT_TRUE(A.Records[5].Endpoint.CredentialsPresent);
  EXPECT_FALSE(A.Records[5].Endpoint.comparable());
  EXPECT_EQ(A.Records[5].CredentialArgumentsStatus, "absent");
  EXPECT_EQ(A.Records[6].BodyStatus, "absent_argument");
}

TEST(WebSourceXHR, EverySpreadCallWithholdsParameterRolesAndPresenceClaims) {
  const auto A = analyze(R"(
const x = new XMLHttpRequest();
x.open(...args);
x.open('POST', ...args, false, 'user', 'password');
x.setRequestHeader('Authorization', ...values);
x.send(...body);
x.addEventListener('load', ...handlers);
)");
  ASSERT_EQ(A.Records.size(), 5u);
  for (const auto &R : A.Records) {
    EXPECT_EQ(R.ArgumentsStatus, "spread_unresolved");
    EXPECT_NE(R.ConstructorNode, NoSourceIndex);
    EXPECT_NE(R.ReceiverBinding, NoSourceIndex);
    for (auto I : {R.URLNode, R.MethodNode, R.HeaderNameNode, R.HeaderValueNode,
                   R.BodyNode, R.HandlerNode, R.AsyncNode, R.OptionsNode})
      EXPECT_EQ(I, NoSourceIndex);
    EXPECT_EQ(R.Method, "unknown");
    EXPECT_FALSE(R.Endpoint.comparable());
  }
  EXPECT_EQ(A.Records[0].CredentialArgumentsStatus, "spread_unresolved");
  EXPECT_EQ(A.Records[1].URLStatus, "spread_unresolved");
  EXPECT_EQ(A.Records[2].HeaderKind, "spread_unresolved");
  EXPECT_EQ(A.Records[3].BodyStatus, "spread_unresolved");
  EXPECT_EQ(A.Records[4].EventKind, "spread_unresolved");
  EXPECT_EQ(A.Records[4].HandlerStatus, "spread_unresolved");
  EXPECT_EQ(A.Records[4].OptionsStatus, "spread_unresolved");
  // The new argument-role field is scoped to XHR; legacy call adapters do
  // not acquire a positional-argument claim by inheriting its default.
  const auto Direct = analyze("fetch(...args);new WebSocket(...args);");
  ASSERT_EQ(Direct.Records.size(), 2u);
  for (const auto &R : Direct.Records)
    EXPECT_EQ(R.ArgumentsStatus, "not_applicable");
}

TEST(WebSourceXHR, ForbiddenMethodsRemainForbiddenAcrossASCIICase) {
  for (const auto *Method : {"CONNECT", "connect", "TrAcE", "TRACK", "track"}) {
    SCOPED_TRACE(Method);
    const auto A = analyze(std::string("new XMLHttpRequest().open('") + Method +
                           "','https://x/a');fetch('https://x/a',{method:'" +
                           Method + "'});");
    ASSERT_EQ(A.Records.size(), 2u);
    EXPECT_EQ(A.Records[0].MethodStatus, "xhr_forbidden_method");
    EXPECT_EQ(A.Records[1].MethodStatus, "fetch_forbidden_method");
  }
}

TEST(WebSourceXHR, EventValuesAreSyntaxAndOnlyPlainAssignmentIsAdmitted) {
  const auto A = analyze(R"(
const x = new XMLHttpRequest();
x.onload = null;
x.onload = callback;
x.onload += callback;
x.onerror ||= callback;
x.ontimeout &&= callback;
x.onloadend ??= callback;
x.addEventListener('load', {handleEvent(event) { consume(event); }});
x.addEventListener('load');
x.addEventListener('progress', null, {once:true});
x.addEventListener('custom', callback);
x.addEventListener(eventName, callback);
x.oncustom = callback;
x['onload'] = callback;
)");
  ASSERT_EQ(A.Records.size(), 5u);
  EXPECT_EQ(A.Records[0].HandlerStatus, "null_value_syntax");
  EXPECT_EQ(A.Records[1].HandlerStatus, "value_syntax");
  EXPECT_EQ(A.Records[2].HandlerStatus, "object_value_syntax");
  EXPECT_EQ(A.Records[3].HandlerStatus, "absent");
  EXPECT_EQ(A.Records[4].EventKind, "progress");
  EXPECT_EQ(A.Records[4].HandlerStatus, "null_value_syntax");
  EXPECT_NE(A.Records[4].OptionsNode, NoSourceIndex);
}

TEST(WebSourceXHR, ShadowingMutationAliasesAndComputedReceiversAreExcluded) {
  for (const auto *Code :
       {"function f(XMLHttpRequest) { new XMLHttpRequest().send(); }",
        "XMLHttpRequest = other; new XMLHttpRequest().send();",
        "const x = new XMLHttpRequest(); x = other; x.send();",
        "const x = new XMLHttpRequest(); x++; x.send();",
        "let x = new XMLHttpRequest(); x.send();",
        "var x = new XMLHttpRequest(); x.send();",
        "const {x} = new XMLHttpRequest(); x.send();",
        "const x = new window.XMLHttpRequest(); x.send();",
        "const X = XMLHttpRequest; new X().send();",
        "const x = new XMLHttpRequest(); const y = x; y.send();",
        "const x = new XMLHttpRequest(); x['send']();",
        "const x = new XMLHttpRequest(); function f(x) { x.send(); }",
        "const x = new XMLHttpRequest(); const send = x.send; send();",
        "const x = new XMLHttpRequest(); x.upload.onload = handler;"}) {
    SCOPED_TRACE(Code);
    const auto A = analyze(Code);
    EXPECT_EQ(A.Status, "partial");
    EXPECT_TRUE(A.Records.empty());
  }
  const auto With = analyze(
      "const x = new XMLHttpRequest(); with (other) { x.send(); }", "script");
  EXPECT_TRUE(With.Records.empty());
  const auto Eval =
      analyze("const x = new XMLHttpRequest(); eval(code); x.send();");
  EXPECT_EQ(Eval.Status, "unavailable");
  EXPECT_EQ(Eval.Reason, "interface_source_dynamic_eval");
  EXPECT_TRUE(Eval.Records.empty());
}

TEST(WebSourceXHR, LexicalSitesDoNotProveOrderRuntimeIdentityOrHARRequests) {
  const auto A = analyze(R"(
const x = new XMLHttpRequest();
x.open('GET', 'https://example.com/a');
if (condition) x.open('POST', 'https://example.com/b');
function later() { x?.send?.(payload); }
x.send = replacement;
x.send(payload);
fetch('https://example.com/a');
)");
  ASSERT_EQ(A.Records.size(), 5u);
  EXPECT_EQ(A.Records[0].ReceiverBinding, A.Records[3].ReceiverBinding);
  EXPECT_TRUE(A.Records[2].Optional);
  EXPECT_EQ(A.Records[3].Kind, "xhr_send");
  // A const binding does not freeze methods. These remain syntax candidates.
  EXPECT_EQ(A.Reason, "source_callee_and_receiver_not_runtime_identity");
  const auto H = inspectHAR("h", R"({"log":{"version":"1.2","entries":[
{"request":{"method":"GET","url":"https://example.com/a"}},
{"request":{"method":"POST","url":"https://example.com/b"}}
]}})");
  const auto Pairs = correlateInterfaces(A, H);
  ASSERT_EQ(Pairs.Pairs.size(), 1u);
  EXPECT_EQ(Pairs.Pairs[0].Source, 4u);
  EXPECT_EQ(Pairs.EligibleSources, 1u);
}

TEST(WebSourceXHR, ScopeIdentitySeparatesSameSpelledReceivers) {
  const auto A = analyze(R"(
const x = new XMLHttpRequest();
x.open('GET', '/outer');
{ const x = new XMLHttpRequest(); x.send(); }
function later() { x.send(); }
)");
  ASSERT_EQ(A.Records.size(), 3u);
  EXPECT_NE(A.Records[0].ReceiverBinding, A.Records[1].ReceiverBinding);
  EXPECT_EQ(A.Records[0].ReceiverBinding, A.Records[2].ReceiverBinding);
  EXPECT_NE(A.Records[0].ConstructorNode, A.Records[1].ConstructorNode);
}

TEST(WebSourceXHR, RecordBudgetAndMalformedModelsPublishNoPartialRecords) {
  std::string Code = "const x = new XMLHttpRequest();";
  for (uint64_t I = 0; I <= MaxInterfaceRecords; ++I)
    Code += "x.send();";
  const auto A = analyze(Code);
  EXPECT_EQ(A.Status, "budget_exceeded");
  EXPECT_EQ(A.Reason, "interface_record_budget_exceeded");
  EXPECT_TRUE(A.Records.empty());
  EXPECT_EQ(A.PrivateBytes, 0u);

  const auto S = inspectJavaScript(
      "a", "const x=new XMLHttpRequest();x.send();", "module");
  const auto V = analyzeSourceValues(S);
  auto B = analyzeSourceBindings(S);
  ASSERT_FALSE(B.References.empty());
  B.References.front().Node = S.Nodes.size();
  EXPECT_EQ(analyzeSourceInterfaces(S, B, V).Reason,
            "invalid_interface_binding_model");
  B = analyzeSourceBindings(S);
  ASSERT_FALSE(B.Declarations.empty());
  B.Declarations.front().Binding = B.Bindings.size();
  const auto Invalid = analyzeSourceInterfaces(S, B, V);
  EXPECT_EQ(Invalid.Reason, "invalid_interface_binding_model");
  EXPECT_TRUE(Invalid.Records.empty());
}
