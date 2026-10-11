//===- SourceFeatureTests.cpp - Client source pattern evidence ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Self-authored client patterns, opaque boundaries and source-only links.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/SourceFeatures.h"
#include "neverd/web/SourceNavigation.h"

#include <algorithm>
#include <set>

namespace {
using namespace neverd::web;
using Domain = SourceFeatureDomain;
using Pattern = SourceFeaturePattern;

struct Inspection {
  std::string Text;
  SourceAnalysis Source;
  SourceBindingAnalysis Bindings;
  SourceNavigation Navigation;
  SourceFeatures Features;
  explicit Inspection(std::string Text) : Text(std::move(Text)) {
    Source = inspectJavaScript(identity("feature-test", {this->Text}),
                               this->Text, "script");
    EXPECT_EQ(Source.ParseStatus, "parsed");
    Bindings = analyzeSourceBindings(Source);
    Navigation = analyzeSourceNavigation(Source, Bindings);
    Features = analyzeClientSourceFeatures(Source, Bindings, Navigation);
  }
  const SourceFeatures &features() const { return Features; }
  std::string_view text(uint32_t Node) const {
    const auto &N = Source.Nodes.at(Node);
    return std::string_view(Text).substr(N.Start, N.End - N.Start);
  }
};

TEST(WebSourceFeatures, FiveDomainsRetainExactSyntaxAndCallbackAnchors) {
  Inspection I(R"JS(function callback(arg) { return arg; }
    function client() {
      const tool = {name: 'PRIVATE_TOOL', inputSchema: {}, call: callback};
      server.registerTool('PRIVATE_TOOL', {description: 'PRIVATE_DESC'}, callback);
      const config = loadSettings('PRIVATE_PATH');
      if (checkPermissions(tool)) dispatchTool(tool);
      server.setRequestHandler(PRIVATE_SCHEMA, callback);
      runHooks('PRIVATE_EVENT');
      const settings = {permissions: {allow: ['PRIVATE_RULE']},
        mcpServers: {}, hooks: {PreToolUse: [{command: 'PRIVATE_COMMAND'}]}};
    })JS");
  ASSERT_EQ(I.features().Status, "partial");
  ASSERT_EQ(I.features().Records.size(), 11);
  std::set<Domain> Domains;
  bool Definition = false, Registration = false, Permission = false;
  for (const auto &R : I.features().Records) {
    Domains.insert(R.Domain);
    EXPECT_NE(R.Function, NoSourceIndex);
    ASSERT_NE(R.Trigger, NoSourceIndex);
    EXPECT_GE(I.Source.Nodes[R.Trigger].Start, I.Source.Nodes[R.Node].Start);
    EXPECT_LE(I.Source.Nodes[R.Trigger].End, I.Source.Nodes[R.Node].End);
    if (R.Pattern == Pattern::ToolDefinition ||
        R.Pattern == Pattern::ToolRegistrationCall) {
      Definition |= R.Pattern == Pattern::ToolDefinition;
      Registration |= R.Pattern == Pattern::ToolRegistrationCall;
      EXPECT_NE(R.CallbackBinding, NoSourceIndex);
      EXPECT_EQ(R.CallbackFunction, 0);
      EXPECT_EQ(I.text(R.Callback), "callback");
      EXPECT_EQ(R.NameStatus, "literal_string_redacted");
    }
    if (R.Pattern == Pattern::PermissionCall) {
      Permission = true;
      ASSERT_NE(R.Condition, NoSourceIndex);
      EXPECT_EQ(I.text(R.Condition), "checkPermissions(tool)");
    }
  }
  EXPECT_EQ(Domains.size(), 5);
  EXPECT_TRUE(Definition && Registration && Permission);
  const auto Again = analyzeSourceNavigation(I.Source, I.Bindings);
  EXPECT_EQ(Again.ID, I.Navigation.ID);
  const auto AgainFeatures =
      analyzeClientSourceFeatures(I.Source, I.Bindings, Again);
  EXPECT_EQ(AgainFeatures.ID, I.features().ID);
  for (size_t N = 0; N < I.features().Records.size(); ++N)
    EXPECT_EQ(AgainFeatures.Records[N].ID, I.features().Records[N].ID);
  // Feature composition does not change the existing navigation identity.
  EXPECT_EQ(I.Navigation.ID, identity("source-navigation",
                                      {I.Source.ID, JavaScriptNavigationProfile,
                                       I.Bindings.ID, I.Bindings.Status}));
}

TEST(WebSourceFeatures, EscapedStaticNamesMatchButComputedNamesStayUnresolved) {
  Inspection I(R"JS(server['registerTool']('PRIVATE_NAME', {}, () => 1);
    server[dynamicName]('PRIVATE_NAME', {}, () => 2);
    server['register' + 'Tool']('PRIVATE_NAME', {}, () => 3);
    function l\u006fadSettings() {} loadSettings();
    const unrelated = {PreToolUse: [], PostToolUse: []};
    new registerTool('PRIVATE_NAME', {}, () => 4);)JS");
  ASSERT_EQ(I.features().Status, "partial");
  ASSERT_EQ(I.features().Records.size(), 2);
  EXPECT_EQ(I.features().ComputedMemberCalls, 2);
  const auto &R = I.features().Records[0];
  EXPECT_EQ(R.Pattern, Pattern::ToolRegistrationCall);
  EXPECT_EQ(R.Basis, "callee_member_spelling");
  EXPECT_NE(R.CallbackFunction, NoSourceIndex);
  EXPECT_EQ(R.CallbackBinding, NoSourceIndex);
  EXPECT_EQ(I.features().Records[1].Pattern, Pattern::ConfigurationCall);
}

TEST(WebSourceFeatures, SpreadAccessorsDuplicatesAndOpaqueSectionsKeepReasons) {
  Inspection I(R"JS(server.registerTool(...PRIVATE_ARGS);
    const spread = {name:'PRIVATE', inputSchema:{}, call:()=>1, ...rest};
    const duplicate = {name:'PRIVATE', inputSchema:{}, call:()=>1, call:()=>2};
    const getter = {name:'PRIVATE', get inputSchema(){throw PRIVATE_ERROR}, call:()=>1};
    const computed = {name:'PRIVATE', inputSchema:{}, call:()=>1, [PRIVATE_KEY]: 1};
    const sections = {hooks: PRIVATE_HOOKS, mcpServers: PRIVATE_SERVERS};)JS");
  ASSERT_EQ(I.features().Status, "partial");
  ASSERT_EQ(I.features().Records.size(), 7);
  std::set<std::string> Reasons;
  for (const auto &R : I.features().Records) {
    EXPECT_EQ(R.Status, "unresolved_shape");
    Reasons.insert(R.Reason);
    EXPECT_EQ(R.Callback, NoSourceIndex);
    EXPECT_EQ(R.CallbackFunction, NoSourceIndex);
  }
  EXPECT_EQ(Reasons, (std::set<std::string>{"spread_arguments", "spread_object",
                                            "duplicate_shape_key",
                                            "accessor_or_method_shape_property",
                                            "computed_shape_key",
                                            "section_value_not_object"}));
}

TEST(WebSourceFeatures, CallbackLinksRemainLexicalAfterWritesAndShadowing) {
  Inspection I(R"JS(let callback = () => 1;
    callback = PRIVATE_REPLACEMENT;
    server.registerTool('PRIVATE_A', {}, callback);
    { const callback = () => 2; server.registerTool('PRIVATE_B', {}, callback); }
    server.registerTool('PRIVATE_C', {}, PRIVATE_EXTERNAL);
    with (PRIVATE_SCOPE) { server.registerTool('PRIVATE_D', {}, callback); }
  )JS");
  ASSERT_EQ(I.features().Records.size(), 4);
  const auto &R = I.features().Records;
  EXPECT_EQ(R[0].CallbackFunction, 0);
  EXPECT_EQ(R[1].CallbackFunction, 1);
  EXPECT_NE(R[0].CallbackBinding, R[1].CallbackBinding);
  EXPECT_EQ(R[2].CallbackFunction, NoSourceIndex);
  EXPECT_EQ(R[3].CallbackFunction, NoSourceIndex);
  // The retained initializer relationship is never a runtime target proof.
}

TEST(WebSourceFeatures, PermissionContainmentCannotCrossNestedFunctionBodies) {
  Inspection I(R"JS(if ((() => checkPermission())()) dispatchTool();
    const decision = checkPermission() ? 1 : 2;
    if (outer()) { checkPermissions(); }
  )JS");
  std::vector<uint32_t> Conditions;
  for (const auto &R : I.features().Records)
    if (R.Domain == Domain::Permission)
      Conditions.push_back(R.Condition);
  ASSERT_EQ(Conditions.size(), 3);
  EXPECT_EQ(Conditions[0], NoSourceIndex);
  ASSERT_NE(Conditions[1], NoSourceIndex);
  EXPECT_EQ(I.text(Conditions[1]), "checkPermission()");
  EXPECT_EQ(Conditions[2], NoSourceIndex);
}

TEST(WebSourceFeatures, EventLiteralsAreSyntaxCandidatesEvenInUnrelatedCalls) {
  Inspection I(R"JS(logger('PreToolUse', 'PostToolUse');
    PRIVATE_DISPATCH(...PRIVATE_ARGS, 'Stop');
    logger('Pre' + 'ToolUse');
  )JS");
  ASSERT_EQ(I.features().Records.size(), 3);
  const auto &R = I.features().Records;
  EXPECT_EQ(R[0].Pattern, Pattern::HookEventArgument);
  EXPECT_EQ(R[0].ArgumentOrdinal, 0);
  EXPECT_EQ(R[1].ArgumentOrdinal, 1);
  EXPECT_EQ(R[0].Arguments, 2);
  EXPECT_EQ(R[1].Arguments, 2);
  EXPECT_NE(R[0].ID, R[1].ID);
  EXPECT_EQ(R[2].Status, "unresolved_shape");
  EXPECT_EQ(R[2].Reason, "spread_arguments");
  EXPECT_EQ(R[2].CallbackFunction, NoSourceIndex);
}

TEST(WebSourceFeatures, FeatureRecordBudgetDoesNotInvalidateNavigation) {
  std::string Text;
  for (uint64_t I = 0; I <= MaxJavaScriptClientFeatureRecords; ++I)
    Text += "loadSettings();";
  Inspection I(std::move(Text));
  EXPECT_EQ(I.Navigation.Status, "ok");
  EXPECT_EQ(I.Navigation.Calls.size(), MaxJavaScriptClientFeatureRecords + 1);
  EXPECT_EQ(I.features().Status, "budget_exceeded");
  EXPECT_TRUE(I.features().Records.empty());
  ASSERT_EQ(I.features().Diagnostics.size(), 1);
  EXPECT_EQ(I.features().Diagnostics[0].Code,
            "source_feature_record_budget_exceeded");
}

TEST(WebSourceFeatures, UnavailableOrForeignModelsPublishNoCandidates) {
  Inspection I("server.registerTool('PRIVATE', {}, () => 1);");
  auto N = I.Navigation;
  N.Status = "unavailable";
  auto A = analyzeClientSourceFeatures(I.Source, I.Bindings, N);
  EXPECT_EQ(A.Status, "unavailable");
  EXPECT_TRUE(A.Records.empty());
  N = I.Navigation;
  N.SourceID = "foreign";
  A = analyzeClientSourceFeatures(I.Source, I.Bindings, N);
  EXPECT_EQ(A.Status, "unavailable");
  EXPECT_TRUE(A.Records.empty());
  N = I.Navigation;
  N.Functions[0].Node = NoSourceIndex;
  A = analyzeClientSourceFeatures(I.Source, I.Bindings, N);
  EXPECT_EQ(A.Status, "unavailable");
  EXPECT_TRUE(A.Records.empty());
}
} // namespace
