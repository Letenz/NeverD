//===- SourceRenameTests.cpp - Lexical rename evidence contracts --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent source expectations and refusal of name-dependent semantics.
///
//===----------------------------------------------------------------------===//

#include "Internal.h"
#include "gtest/gtest.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Bun.h"
#include "neverd/web/Error.h"
#include "neverd/web/SourceRewrite.h"

#include <cstdlib>
#include <set>

namespace {
using namespace neverd::web;

SourceRewrite rename(std::string_view Text, std::string_view Type = "script") {
  return rewriteJavaScript(identity("rename-test", {Text}), Text, Type,
                           "scope_renaming");
}

void expectRenamed(std::string_view Before, std::string_view After) {
  const auto A = rename(Before);
  ASSERT_EQ(A.Status, "verified_local_rewrites") << A.Reason;
  EXPECT_EQ(A.Text, After);
  EXPECT_EQ(A.SourceHash, sha256(Before));
  EXPECT_EQ(A.CandidateHash, sha256(After));
  EXPECT_EQ(undoSourceRewrite(A, A.Text), Before);
  const auto Again = rename(Before);
  EXPECT_EQ(Again.ID, A.ID);
  EXPECT_EQ(Again.Text, A.Text);
  std::set<std::string> IDs;
  for (const auto &R : A.Records) {
    EXPECT_TRUE(IDs.insert(R.ID).second);
    EXPECT_FALSE(R.BindingID.empty());
    if (R.Status == "accepted") {
      EXPECT_TRUE(R.Role == "declaration" || R.Role == "reference");
      EXPECT_EQ(Before.substr(R.SourceStart, R.SourceEnd - R.SourceStart),
                R.Before);
      EXPECT_EQ(After.substr(R.Start, R.End - R.Start), R.After);
    }
  }
}

TEST(WebSourceRename, CompleteLocalBindingsKeepAllOtherSourceBytes) {
  expectRenamed("/*license*/function f(a){ const b=a+1; return b; }",
                "/*license*/function f(neverd_arg_2){ const "
                "neverd_local_3=neverd_arg_2+1; return neverd_local_3; }");
  expectRenamed("function f(a){return {a: a, method: obj.a};}",
                "function f(neverd_arg_2){return {a: neverd_arg_2, "
                "method: obj.a};}");
}

TEST(WebSourceRename, NestedClosuresRetainDistinctShadowedBindings) {
  expectRenamed("function f(a){return function g(a){return a;};}",
                "function f(neverd_arg_2){return function "
                "g(neverd_arg_5){return neverd_arg_5;};}");
  expectRenamed("function f(a){return ()=>a;}",
                "function f(neverd_arg_2){return ()=>neverd_arg_2;}");
}

TEST(WebSourceRename, FreshNamesAvoidPropertyLabelAndDecodedIdentifierNames) {
  expectRenamed("function f(a){return obj.neverd_arg_2+a;}",
                "function f(neverd_arg_2_){return "
                "obj.neverd_arg_2+neverd_arg_2_;}");
  expectRenamed(R"(function f(\u0061){return \u0061;})",
                "function f(neverd_arg_2){return neverd_arg_2;}");
  expectRenamed("function f(a){neverd_arg_2:while(false){break neverd_arg_2;}"
                "return a;}",
                "function f(neverd_arg_2_){neverd_arg_2:while(false){break "
                "neverd_arg_2;}return neverd_arg_2_;}");
}

TEST(WebSourceRename, ShorthandAndWritesNeverProducePartialBindingRenames) {
  for (const auto *Text :
       {"function f(a){return {a};}", "function f(a){a++;return a;}",
        "function f(a){a=()=>1;return a.name;}",
        "function f(){let a=1;({a}=obj);return a;}",
        "function f(){const {a}=obj;return a;}",
        "function f(){const [a]=obj;return a;}",
        "function f(){var a;var a;return a;}"}) {
    const auto A = rename(Text);
    EXPECT_EQ(A.Status, "no_eligible_rewrites") << Text << ": " << A.Reason;
    EXPECT_EQ(A.Text, Text);
    EXPECT_EQ(A.Accepted, 0U);
    EXPECT_GT(A.Refused, 0U);
  }
}

TEST(WebSourceRename, FunctionAndClassNamesRetainObservableInference) {
  for (const auto *Text : {"function f(){const a=()=>1;return a.name;}",
                           "function f(){let a=(function(){});return a.name;}",
                           "function f(){var a=class {};return a.name;}",
                           "function f(){function a(){} return a.name;}",
                           "function f(){class a{} return a.name;}"}) {
    const auto A = rename(Text);
    EXPECT_EQ(A.Status, "no_eligible_rewrites") << Text << ": " << A.Reason;
    EXPECT_EQ(A.Text, Text);
    std::set<std::string> IDs;
    for (const auto &R : A.Records)
      EXPECT_TRUE(IDs.insert(R.ID).second);
  }
}

TEST(WebSourceRename, NonSimpleParametersRetainImplicitParameterToVarCopies) {
  // The lexical model has two binding identities but the runtime also copies
  // the argument value into the same-named body var. Graph shape alone is not
  // an equivalence proof. These sources must remain byte-for-byte unchanged.
  for (const auto *Text : {"function f(a,b=0){var a;return a;}",
                           "function f(a,b=()=>a){var a;return b;}",
                           "function f(a,{b}){const c=a;return c;}",
                           "function f(a,...rest){let c=a;return c;}"}) {
    const auto A = rename(Text);
    EXPECT_EQ(A.Status, "no_eligible_rewrites") << Text << ": " << A.Reason;
    EXPECT_EQ(A.Text, Text);
    EXPECT_EQ(A.Accepted, 0U);
  }
}

TEST(WebSourceRename, RootBindingsAndPublicModuleNamesStayUnchanged) {
  for (const auto *Type : {"script", "module", "commonjs"}) {
    const std::string Text = "var a=1;let b=2;const c=3;use(a,b,c);";
    const auto A = rename(Text, Type);
    EXPECT_EQ(A.Status, "no_eligible_rewrites") << Type << ": " << A.Reason;
    EXPECT_EQ(A.Text, Text);
  }
  const std::string Module =
      "import {a as b} from 'private';export const c=b;export {b};";
  const auto A = rename(Module, "module");
  EXPECT_EQ(A.Status, "no_eligible_rewrites") << A.Reason;
  EXPECT_EQ(A.Text, Module);
}

TEST(WebSourceRename, DynamicAndUncertainScopesNeverPublishCandidates) {
  for (const auto *Text : {"function f(a){eval(code);return a;}",
                           "function f(a){with(obj){use(a);}}",
                           "function f(a){if(a){function g(){}}return a;}",
                           "function f(a){let a;return a;}", "function ("}) {
    const auto A = rename(Text);
    EXPECT_EQ(A.Status, "unavailable") << Text << ": " << A.Reason;
    EXPECT_TRUE(A.Text.empty());
    EXPECT_TRUE(A.CandidateHash.empty());
    EXPECT_TRUE(A.Records.empty());
  }
}

TEST(WebSourceRename, RecordAndComparisonBudgetsPublishNoPartialBinding) {
  std::string Many = "function f(){";
  for (unsigned I = 0; I < MaxSourceRewriteRecords + 1; ++I)
    Many += "let a" + std::to_string(I) + "=0;";
  Many += '}';
  const auto R = rename(Many);
  EXPECT_EQ(R.Status, "budget_exceeded") << R.Reason;
  EXPECT_EQ(R.Reason, "source_rewrite_record_budget_exceeded");
  EXPECT_TRUE(R.Text.empty());
  EXPECT_TRUE(R.Records.empty());

  std::string OneBinding = "function f(a){";
  for (unsigned I = 0; I < MaxSourceRewriteRecords; ++I)
    OneBinding += "a;";
  OneBinding += '}';
  const auto Partial = rename(OneBinding);
  EXPECT_EQ(Partial.Reason, "source_rewrite_record_budget_exceeded");
  EXPECT_EQ(Partial.Accepted, 0U);
  EXPECT_TRUE(Partial.Text.empty());
  EXPECT_TRUE(Partial.Records.empty());

  std::string Expensive = "function f(){";
  const std::string Prefix(750, 'a');
  for (unsigned I = 0; I < 1000; ++I)
    Expensive += "let " + Prefix + std::to_string(I) + "=0;";
  Expensive += '}';
  ASSERT_LT(Expensive.size(), MaxSourceRewriteBytes);
  const auto W = rename(Expensive);
  EXPECT_EQ(W.Status, "budget_exceeded") << W.Reason;
  EXPECT_TRUE(W.Text.empty());
  EXPECT_TRUE(W.Records.empty());
}

TEST(WebSourceRename, InputOutputAndLexemeLimitsPublishNoCandidate) {
  const auto Big = rename(std::string(MaxSourceRewriteBytes + 1, ' '));
  EXPECT_EQ(Big.Status, "budget_exceeded");
  EXPECT_TRUE(Big.Text.empty());
  const std::string Body = "function f(a){return a;}";
  const std::string Expanded =
      "/*" + std::string(MaxSourceRewriteBytes - Body.size() - 4, ' ') + "*/" +
      Body;
  ASSERT_EQ(Expanded.size(), MaxSourceRewriteBytes);
  const auto Output = rename(Expanded);
  EXPECT_EQ(Output.Status, "budget_exceeded") << Output.Reason;
  EXPECT_TRUE(Output.Text.empty());
  EXPECT_TRUE(Output.Records.empty());
  std::string Comments;
  for (uint64_t I = 0; I <= MaxJavaScriptLexemes; ++I)
    Comments += "/**/";
  Comments += Body;
  const auto Lex = rename(Comments);
  EXPECT_EQ(Lex.Reason, "source_rewrite_lexemes_unavailable");
  EXPECT_TRUE(Lex.Text.empty());
  EXPECT_TRUE(Lex.Records.empty());
}

TEST(WebSourceRename, ClaudeCode21296OriginalMembersRetainSuccessAndBudgets) {
  const auto *Path = std::getenv("NEVERD_CLAUDE_CODE_21296_ELF");
  if (!Path)
    GTEST_SKIP() << "Optional hash-pinned official original not supplied";
  const auto Snapshot = capture(Path, Limits{});
  ASSERT_EQ(Snapshot.Artifacts.size(), 1U);
  const auto &Original = Snapshot.Artifacts.front();
  ASSERT_EQ(Original.BlobHash,
            "24972e3bc859fab2b46ed4c1e51f7d6130f06d3bd550811a114640de3370d0de");
  const auto E = extractBun(Original);
  ASSERT_GT(E.Modules.size(), 1U);
  const auto &M = E.Modules[1];
  const auto Bytes = bunSourceBytes(E, M, MaxSourceRewriteBytes);
  ASSERT_EQ(Bytes.size(), 1362U);
  ASSERT_EQ(sha256(Bytes),
            "e8dacd18472cc93ede5f22396d1d86545754a2a9cfc6cbe6a9ba07dbb0b8cc55");
  const auto A = rewriteJavaScript(M.SourceArtifactID, Bytes,
                                   M.Format == 1 ? "module" : "commonjs",
                                   "scope_renaming");
  ASSERT_EQ(A.Status, "verified_local_rewrites") << A.Reason;
  EXPECT_EQ(A.Accepted, 46U);
  EXPECT_EQ(A.Refused, 17U);
  EXPECT_EQ(undoSourceRewrite(A, A.Text), Bytes);
  EXPECT_NE(A.CandidateHash, A.SourceHash);
  EXPECT_NE(A.Text.find("function Nd(...n){return r(n.join(\"\"))}"),
            std::string::npos);
  EXPECT_NE(
      A.Text.find("export{b,d,pe,Br,$q,Nd,uVn,gl,Jy,kn,zb,vOo,eie,tfe,WPr};"),
      std::string::npos);

  ASSERT_GT(E.Modules.size(), 228U);
  const auto &LargeModule = E.Modules[228];
  const auto LargeBytes = bunSourceBytes(E, LargeModule, MaxSourceRewriteBytes);
  ASSERT_EQ(LargeBytes.size(), 245964U);
  ASSERT_EQ(sha256(LargeBytes),
            "4285cc2ba53ddf1faa24373004ecfb63041d48048bd9733a18c5025d213befd9");
  const auto Limited = rewriteJavaScript(
      LargeModule.SourceArtifactID, LargeBytes,
      LargeModule.Format == 1 ? "module" : "commonjs", "scope_renaming");
  EXPECT_EQ(Limited.Status, "budget_exceeded");
  EXPECT_EQ(Limited.Reason, "source_rewrite_record_budget_exceeded");
  EXPECT_EQ(Limited.Accepted, 0U);
  EXPECT_TRUE(Limited.CandidateHash.empty());
  EXPECT_TRUE(Limited.Text.empty());
  EXPECT_TRUE(Limited.Records.empty());
}

TEST(WebSourceRename, InverseBindsPassBindingRoleAndExactCandidate) {
  auto A = rename("function f(a){return a;}");
  ASSERT_EQ(A.Status, "verified_local_rewrites") << A.Reason;
  const auto Original = A;
  for (auto &R : A.Records)
    if (R.Status == "accepted") {
      R.BindingID = "foreign";
      break;
    }
  EXPECT_THROW(undoSourceRewrite(A, A.Text), Error);
  A = Original;
  for (auto &R : A.Records)
    if (R.Status == "accepted") {
      R.Role = "property_key";
      break;
    }
  EXPECT_THROW(undoSourceRewrite(A, A.Text), Error);
  A = Original;
  A.Pass = "constant_folding";
  EXPECT_THROW(undoSourceRewrite(A, A.Text), Error);
  EXPECT_THROW(undoSourceRewrite(Original, Original.Text + " "), Error);
}
} // namespace
