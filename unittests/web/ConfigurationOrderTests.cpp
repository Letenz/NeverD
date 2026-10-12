//===- ConfigurationOrderTests.cpp - Config source order ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Known settings merges, lexical origins, private values and atomic bounds.
///
//===----------------------------------------------------------------------===//

#include "features/configuration/Publication.h"
#include "gtest/gtest.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/ConfigurationOrder.h"
#include "neverd/web/SourceNavigation.h"

#include <cstdlib>
#include <fstream>

namespace {
using namespace neverd::web;
struct Inspection {
  SourceAnalysis S;
  SourceBindingAnalysis B;
  SourceNavigation N;
  SourceFeatures F;
  ConfigurationSourceOrder A;
  explicit Inspection(std::string_view Text, std::string_view Mode = "script") {
    S = inspectJavaScript(identity("config-order-fixture", {Text}), Text, Mode);
    EXPECT_EQ(S.ParseStatus, "parsed");
    B = analyzeSourceBindings(S);
    N = analyzeSourceNavigation(S, B);
    F = analyzeClientSourceFeatures(S, B, N);
    A = analyzeConfigurationSourceOrder(S, B, N, F);
  }
};
} // namespace

TEST(WebConfigurationOrder,
     TermsLinkLoadingSyntaxAndKeepLaterOverwritesUnknown) {
  Inspection I(R"JS(const defaults=loadSettings('CANARY_SYSTEM');
    const project=readSettings('CANARY_PROJECT');
    const settings={...defaults,permissions:{deny:['CANARY']},...project,
                    permissionMode:'CANARY_MODE'};
  )JS");
  ASSERT_EQ(I.A.Status, "partial") << I.A.Reason;
  ASSERT_EQ(I.A.Objects.size(), 1u);
  const auto &O = I.A.Objects[0];
  ASSERT_EQ(O.Terms.size(), 4u);
  EXPECT_EQ(O.Terms[0].Kind, "object_spread");
  EXPECT_NE(O.Terms[0].Initializer, NoSourceIndex);
  EXPECT_NE(O.Terms[0].Feature, NoSourceIndex);
  EXPECT_NE(O.Terms[2].Initializer, NoSourceIndex);
  EXPECT_NE(O.Terms[2].Feature, NoSourceIndex);
  EXPECT_TRUE(O.Fields[unsigned(ConfigurationField::Permissions)].LaterUnknown);
  EXPECT_FALSE(
      O.Fields[unsigned(ConfigurationField::PermissionMode)].LaterUnknown);
  const auto Row = configuration_order::record(O, I.S, I.B, I.F);
  std::string Text;
  llvm::raw_string_ostream(Text) << llvm::json::Value(llvm::json::Object(Row));
  EXPECT_EQ(Text.find("CANARY"), std::string::npos);
  EXPECT_EQ(Row.getString("effective_configuration"), "not_analyzed");
  const auto Again = analyzeConfigurationSourceOrder(I.S, I.B, I.N, I.F);
  EXPECT_EQ(Again.Objects[0].ID, O.ID);
  EXPECT_EQ(Again.Objects[0].Terms[0].ID, O.Terms[0].ID);
}
TEST(WebConfigurationOrder,
     DuplicateDefinitionsComputedKeysAndAccessorsStaySyntax) {
  Inspection I(R"JS(({permissions:{},...CANARY,permissions:{}});
    ({hooks:{},[CANARY_KEY]:CANARY_VALUE});
    ({[CANARY_KEY]:CANARY_VALUE,hooks:{}});
    ({get permissions(){return CANARY_VALUE}});
  )JS");
  ASSERT_EQ(I.A.Objects.size(), 4u);
  EXPECT_EQ(I.A.Objects[0].Fields[0].DefinitionCount, 2u);
  EXPECT_FALSE(I.A.Objects[0].Fields[0].LaterUnknown);
  EXPECT_TRUE(I.A.Objects[1].Fields[3].LaterUnknown);
  EXPECT_FALSE(I.A.Objects[2].Fields[3].LaterUnknown);
  EXPECT_EQ(I.A.Objects[3].Terms[0].Kind, "accessor_or_method_property");
  const auto Summary = configuration_order::summary(I.A);
  EXPECT_EQ(Summary.getString("successful_object_creation"), "not_checked");
  EXPECT_EQ(Summary.getString("mutation_getters_and_proxies"), "not_analyzed");
}
TEST(WebConfigurationOrder,
     AmbiguousInitializersDynamicScopeAndWritesDoNotProveValues) {
  Inspection I(R"JS(var defaults=loadSettings('CANARY');
    var defaults=readSettings('CANARY');
    ({...defaults,permissions:{}});
    with(CANARY_OBJECT){({...defaults,permissions:{}});}
    const other=loadSettings('CANARY');other=CANARY;
    ({...other,hooks:{}});
    function mixed(){};var mixed=readSettings('CANARY');
    ({...mixed,hooks:{}});
  )JS");
  ASSERT_EQ(I.A.Objects.size(), 4u);
  EXPECT_EQ(I.A.Objects[0].Terms[0].Initializer, NoSourceIndex);
  EXPECT_EQ(I.A.Objects[1].Terms[0].Initializer, NoSourceIndex);
  const auto Ref = I.A.Objects[1].Terms[0].Reference;
  ASSERT_NE(Ref, NoSourceIndex);
  EXPECT_EQ(I.B.References[Ref].Resolution, "dynamic_with");
  EXPECT_NE(I.A.Objects[2].Terms[0].Initializer, NoSourceIndex);
  EXPECT_EQ(I.A.Objects[3].Terms[0].Initializer, NoSourceIndex);
  const auto Row = configuration_order::record(I.A.Objects[2], I.S, I.B, I.F);
  EXPECT_EQ(
      (*Row.getArray("terms"))[0].getAsObject()->getString("runtime_value"),
      "not_proven");
}
TEST(WebConfigurationOrder,
     ExhaustionAndForeignFeaturesDiscardAllOrderRecords) {
  std::string Text;
  for (uint64_t I = 0; I <= MaxConfigurationOrderRecords; ++I)
    Text += "({...CANARY,permissions:{}});";
  Inspection Many(Text);
  EXPECT_EQ(Many.A.Status, "budget_exceeded");
  EXPECT_EQ(Many.A.Reason, "configuration_order_record_budget_exceeded");
  EXPECT_TRUE(Many.A.Objects.empty());
  EXPECT_EQ(Many.A.Terms, 0u);
  EXPECT_EQ(Many.F.Status, "partial");
  Text = "({permissions:{},";
  for (uint64_t I = 0; I < MaxConfigurationOrderTerms; ++I)
    Text += "CANARY:0,";
  Text += "});";
  Inspection Wide(Text);
  EXPECT_EQ(Wide.A.Reason, "configuration_order_term_budget_exceeded");
  EXPECT_TRUE(Wide.A.Objects.empty());
  EXPECT_EQ(Wide.A.Terms, 0u);
  Inspection Small("({permissions:{}});");
  Small.F.SourceID = "foreign";
  EXPECT_EQ(analyzeConfigurationSourceOrder(Small.S, Small.B, Small.N, Small.F)
                .Status,
            "unavailable");
  Small.F.SourceID = Small.S.ID;
  Small.F.Records[0].Trigger = UINT32_MAX;
  EXPECT_EQ(analyzeConfigurationSourceOrder(Small.S, Small.B, Small.N, Small.F)
                .Reason,
            "configuration_order_invalid_features");
  Small = Inspection("({permissions:{}});");
  Small.N.BindingID = "foreign";
  EXPECT_EQ(analyzeConfigurationSourceOrder(Small.S, Small.B, Small.N, Small.F)
                .Reason,
            "configuration_order_evidence_unavailable");
}
TEST(WebConfigurationOrder, OptionalCapturedClaudeSettingsRetainCoverage) {
  const auto *Path = std::getenv("NEVERD_CLAUDE_CODE_21296_CONFIG_JS");
  if (!Path)
    GTEST_SKIP() << "preserved Claude configuration qualification unit absent";
  std::ifstream Input(Path, std::ios::binary);
  const std::string Text(std::istreambuf_iterator<char>(Input), {});
  Inspection I(Text, "module");
  ASSERT_EQ(I.A.Status, "partial") << I.A.Reason;
  EXPECT_EQ(I.S.BlobHash,
            "c377d5191165ae221a042d4ea22bd3333969097edb10eb0ef6791610378324da");
  EXPECT_EQ(I.A.Objects.size(), 1u);
  EXPECT_EQ(I.A.Terms, 4u);
  RecordProperty("source_sha256", I.S.BlobHash);
  RecordProperty("object_count", I.A.Objects.size());
  RecordProperty("term_count", I.A.Terms);
  uint64_t LoadingLinks = 0, Overwrites = 0;
  for (const auto &O : I.A.Objects) {
    for (const auto &T : O.Terms)
      LoadingLinks += T.Feature != NoSourceIndex;
    for (const auto &F : O.Fields)
      Overwrites += F.Seen && F.LaterUnknown;
  }
  RecordProperty("loading_syntax_links", LoadingLinks);
  RecordProperty("later_unknown_fields", Overwrites);
}

TEST(WebConfigurationOrder, OptionalCapturedClaudeMergeHasAnUnknownLaterTerm) {
  const auto *Path = std::getenv("NEVERD_CLAUDE_CODE_21296_CONFIG_UNKNOWN_JS");
  if (!Path)
    GTEST_SKIP() << "preserved Claude unknown-key qualification unit absent";
  std::ifstream Input(Path, std::ios::binary);
  const std::string Text(std::istreambuf_iterator<char>(Input), {});
  Inspection I(Text, "module");
  ASSERT_EQ(I.A.Status, "partial") << I.A.Reason;
  EXPECT_EQ(I.S.BlobHash,
            "5ec1302ba8afecce27dd404105070c2cbb3048d7c3a6dbf9265d6500b7d50daa");
  uint64_t Overwrites = 0;
  for (const auto &O : I.A.Objects)
    for (const auto &Field : O.Fields)
      if (Field.Seen && Field.LaterUnknown) {
        ++Overwrites;
        if (Overwrites == 1) {
          RecordProperty("object_byte_offset",
                         std::to_string(I.S.Nodes[O.Node].Start));
          RecordProperty(
              "object_byte_length",
              std::to_string(I.S.Nodes[O.Node].End - I.S.Nodes[O.Node].Start));
        }
      }
  EXPECT_GT(Overwrites, 0u);
  RecordProperty("source_sha256", I.S.BlobHash);
  RecordProperty("object_count", std::to_string(I.A.Objects.size()));
  RecordProperty("term_count", std::to_string(I.A.Terms));
  RecordProperty("later_unknown_fields", std::to_string(Overwrites));
}
