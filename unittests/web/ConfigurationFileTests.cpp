//===- ConfigurationFileTests.cpp - Minified configuration syntax --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Literal anchors, lexical links, unknown array terms and independent bounds.
///
//===----------------------------------------------------------------------===//

#include "SourceModel.h"
#include "features/configuration/FilesPublication.h"
#include "gtest/gtest.h"

#include "neverd/web/Artifact.h"

#include <algorithm>

namespace {
using namespace neverd::web;
struct Inspection {
  SourceAnalysis S;
  SourceBindingAnalysis B;
  SourceNavigation N;
  ConfigurationFileNavigation A;
  explicit Inspection(std::string_view Text, std::string_view Mode = "script") {
    S = inspectJavaScript(identity("config-file-fixture", {Text}), Text, Mode);
    EXPECT_EQ(S.ParseStatus, "parsed");
    B = analyzeSourceBindings(S);
    N = analyzeSourceNavigation(S, B);
    A = analyzeConfigurationFiles(S, B, N);
  }
};

TEST(WebConfigurationFiles, MinifiedCallsKeepLexicalSourceAndCaseAnchors) {
  Inspection I(R"JS(function x(k){switch(k){
    case 'projectSettings':return j('CANARY_ROOT','settings.json');
    case 'localSettings':return j('CANARY_ROOT','settings.local.json');}}
    const y=()=>j('CANARY_ROOT','managed-settings.json');
    x('projectSettings'); y(); x=unknown; x('localSettings');
  )JS");
  ASSERT_EQ(I.A.Status, "partial") << I.A.Reason;
  ASSERT_EQ(I.A.Sites.size(), 7u);
  ASSERT_EQ(I.A.Calls.size(), 3u);
  EXPECT_TRUE(I.A.Sequences.empty());
  for (const auto &Link : I.A.Calls) {
    EXPECT_NE(Link.TargetFunction, NoSourceIndex);
    EXPECT_TRUE(I.N.Calls[Link.Call].Kind == "call");
  }
  EXPECT_NE(I.A.Sites[1].Case, NoSourceIndex);
  EXPECT_EQ(I.A.Sites[0].Case, I.A.Sites[1].Case);
  const auto Again = analyzeConfigurationFiles(I.S, I.B, I.N);
  EXPECT_EQ(Again.ID, I.A.ID);
  EXPECT_EQ(Again.Calls.back().ID, I.A.Calls.back().ID);
  const auto Summary = configuration_files::summary(I.A);
  EXPECT_EQ(Summary.getString("runtime_targets"), "not_proven");
  EXPECT_EQ(Summary.getString("file_layer_precedence"), "not_analyzed");
  for (size_t J = 0; J < I.A.Sites.size() + I.A.Calls.size(); ++J) {
    const auto Row = configuration_files::record(I.A, I.S, I.N, J);
    std::string Text;
    llvm::raw_string_ostream(Text)
        << llvm::json::Value(llvm::json::Object(Row));
    EXPECT_EQ(Text.find("CANARY"), std::string::npos);
    EXPECT_EQ(Text.find("settings.json"), std::string::npos);
    EXPECT_EQ(Row.getString("confidence"), "source_syntax_only");
  }
}

TEST(WebConfigurationFiles, ArrayOrderKeepsSpreadsHolesAndUnknownTerms) {
  Inspection I(R"JS(const a=['userSettings',,...CANARY,'projectSettings',
    dynamic(),'localSettings'];)JS");
  ASSERT_EQ(I.A.Status, "partial") << I.A.Reason;
  ASSERT_EQ(I.A.Sequences.size(), 1u);
  const auto &Terms = I.A.Sequences[0].Terms;
  ASSERT_EQ(Terms.size(), 6u);
  EXPECT_EQ(Terms[0].Role, "user_layer");
  EXPECT_TRUE(Terms[1].Role.empty());
  EXPECT_TRUE(Terms[2].Role.empty());
  EXPECT_EQ(Terms[3].Role, "project_layer");
  EXPECT_TRUE(Terms[4].Role.empty());
  EXPECT_EQ(Terms[5].Role, "local_layer");
  for (uint32_t J = 0; J < Terms.size(); ++J)
    EXPECT_EQ(Terms[J].Ordinal, J);
  EXPECT_EQ(I.A.Terms, 6u);
  const auto Row = configuration_files::record(I.A, I.S, I.N, I.A.Sites.size());
  EXPECT_EQ(Row.getArray("terms")->size(), 6u);
  EXPECT_EQ(Row.getString("file_layer_precedence"), "not_analyzed");
  auto &Array = I.S.Nodes[I.A.Sequences[0].Node];
  Array.Children[1].Ordinal = Array.Children[0].Ordinal;
  const auto Bad = analyzeConfigurationFiles(I.S, I.B, I.N);
  EXPECT_EQ(Bad.Status, "unavailable");
  EXPECT_TRUE(Bad.Sites.empty());
  EXPECT_TRUE(Bad.Sequences.empty());
}

TEST(WebConfigurationFiles, AliasImportsDynamicKeysAndTextStayUnresolved) {
  Inspection I(R"JS(import ext from 'CANARY';
    function x(){return j('managed-settings.json');}
    const alias=x; alias(); ext('userSettings');
    obj[key]('projectSettings');
    describe('Documentation mentions settings.json');
    j('CANARY/settings.json'); j(`settings.json`);
    j('settings'+'.json');)JS",
               "module");
  ASSERT_EQ(I.A.Status, "partial") << I.A.Reason;
  EXPECT_TRUE(I.A.Calls.empty());
  ASSERT_EQ(I.A.Sites.size(), 3u);
  EXPECT_TRUE(I.A.Sequences.empty());
}

TEST(WebConfigurationFiles, NestedFunctionsDoNotInheritSwitchCases) {
  Inspection I(R"JS(function x(k){switch(k){case 'projectSettings':
    return ()=>j('settings.json');}} x('projectSettings');)JS");
  ASSERT_EQ(I.A.Sites.size(), 3u);
  EXPECT_NE(I.A.Sites[0].Case, NoSourceIndex);
  EXPECT_EQ(I.A.Sites[1].Case, NoSourceIndex);
  EXPECT_NE(I.A.Sites[0].Function, I.A.Sites[1].Function);
}

TEST(WebConfigurationFiles,
     SharedFunctionIndexRefusesAmbiguityAndForeignModels) {
  Inspection I(R"JS(function x(){j('settings.json');}
    function x(){j('settings.local.json');} x();
    function y(){j('managed-settings.json');} y();)JS");
  ASSERT_EQ(I.A.Status, "partial");
  ASSERT_EQ(I.A.Calls.size(), 1u);
  auto Index = indexSourceFunctions(I.S, I.B, I.N);
  const auto Binding = I.N.Functions.back().NameBinding;
  const auto TargetNode = I.N.Functions.back().Node;
  ASSERT_NE(Binding, NoSourceIndex);
  EXPECT_NE(Index.BindingFunctions[Binding], NoSourceIndex);
  std::reverse(I.N.Functions.begin(), I.N.Functions.end());
  const auto Reversed = indexSourceFunctions(I.S, I.B, I.N);
  EXPECT_EQ(I.N.Functions[Reversed.BindingFunctions[Binding]].Node, TargetNode);
  I.B.Bindings[Binding].Conflicting = true;
  EXPECT_EQ(indexSourceFunctions(I.S, I.B, I.N).BindingFunctions[Binding],
            NoSourceIndex);
  I.N.BindingID = "foreign";
  auto Bad = analyzeConfigurationFiles(I.S, I.B, I.N);
  EXPECT_EQ(Bad.Status, "unavailable");
  EXPECT_TRUE(Bad.Sites.empty());
  EXPECT_TRUE(Bad.Calls.empty());
  EXPECT_TRUE(Bad.Sequences.empty());
}

TEST(WebConfigurationFiles, RecordAndTermExhaustionDiscardTheWholeCandidate) {
  std::string Text;
  for (unsigned I = 0; I <= MaxConfigurationFileRecords; ++I)
    Text += "x('settings.json');";
  Inspection Many(Text);
  EXPECT_EQ(Many.A.Status, "budget_exceeded");
  EXPECT_TRUE(Many.A.Sites.empty());
  EXPECT_TRUE(Many.A.Calls.empty());
  Text = "['userSettings','projectSettings'";
  for (unsigned I = 0; I < MaxConfigurationFileTerms; ++I)
    Text += ",unknown";
  Text += "];";
  Inspection Terms(Text);
  EXPECT_EQ(Terms.A.Status, "budget_exceeded");
  EXPECT_TRUE(Terms.A.Sites.empty());
  EXPECT_TRUE(Terms.A.Sequences.empty());
  EXPECT_EQ(Terms.A.Terms, 0u);
}
} // namespace
