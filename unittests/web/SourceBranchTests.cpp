//===- SourceBranchTests.cpp - Boolean selection semantic boundaries ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent text, reference, syntax and inverse oracles without execution.
///
//===----------------------------------------------------------------------===//

#include "Internal.h"
#include "gtest/gtest.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Bun.h"
#include "neverd/web/Error.h"
#include "neverd/web/SourceBindings.h"
#include "neverd/web/SourceRewrite.h"

#include <algorithm>
#include <cstdlib>

namespace {
using namespace neverd::web;

SourceRewrite rewrite(std::string_view Text, std::string_view Type = "script") {
  return rewriteJavaScript(identity("branch-test", {Text}), Text, Type,
                           "boolean_branches");
}

void expectRewrite(std::string_view Before, std::string_view After,
                   std::string_view Type = "script") {
  const auto A = rewrite(Before, Type);
  ASSERT_EQ(A.Status, "verified_local_rewrites") << A.Reason << ": " << Before;
  EXPECT_EQ(A.Text, After);
  EXPECT_EQ(A.SourceHash, sha256(Before));
  EXPECT_EQ(A.CandidateHash, sha256(After));
  EXPECT_EQ(undoSourceRewrite(A, A.Text), Before);
  const auto Again = rewrite(Before, Type);
  EXPECT_EQ(A.ID, Again.ID);
  ASSERT_EQ(A.Records.size(), Again.Records.size());
  for (size_t I = 0; I < A.Records.size(); ++I)
    EXPECT_EQ(A.Records[I].ID, Again.Records[I].ID);
}

TEST(WebSourceBranch, ConditionalAndLogicalChoicesUseOnlyProvenBooleans) {
  for (const auto &[Before, After] :
       {std::pair{"true ? left : right", "(0,(left))"},
        {"false ? left : right", "(0,(right))"},
        {"true && right", "(0,(right))"},
        {"false && right", "(0,(false))"},
        {"true || right", "(0,(true))"},
        {"false || right", "(0,(right))"},
        {"1 === 1 ? left : right", "(0,(left))"},
        {"!0 ? left : right", "(0,(left))"}})
    expectRewrite(std::string(Before) + ";", std::string(After) + ";");
}

TEST(WebSourceBranch, ChosenCallsReadsAssignmentsAndThrowsAreNotDiscarded) {
  expectRewrite("const x=true ? obj.value : absent();",
                "const x=(0,(obj.value));");
  expectRewrite("const x=false ? absent() : effect();",
                "const x=(0,(effect()));");
  expectRewrite("function f(x){return true ? (x=effect()) : unused;}",
                "function f(x){return (0,(x=effect()));}");
  expectRewrite("true ? 1n/0n : other;", "(0,(1n/0n));");
  expectRewrite("true ? missing : other;", "(0,(missing));");
}

TEST(WebSourceBranch, SequenceOperandsRemainNestedUnderGetValueScaffolding) {
  for (const auto *Text : {"true ? (first(), second()) : fallback;",
                           "true && (first(), second());"}) {
    const auto A = rewrite(Text);
    ASSERT_EQ(A.Status, "verified_local_rewrites") << A.Reason;
    EXPECT_EQ(A.Text, "(0,(first(), second()));");
    const auto S = inspectJavaScript("sequence-test", A.Text, "script");
    ASSERT_EQ(S.ParseStatus, "parsed");
    const auto Outer =
        std::find_if(S.Nodes.begin(), S.Nodes.end(), [](const auto &N) {
          return N.Kind == "SequenceExpression";
        });
    ASSERT_NE(Outer, S.Nodes.end());
    ASSERT_EQ(Outer->Children.size(), 2U);
    EXPECT_EQ(S.Nodes[Outer->Children[0].Index].Kind, "NumericLiteral");
    const auto &Inner = S.Nodes[Outer->Children[1].Index];
    EXPECT_EQ(Inner.Kind, "SequenceExpression");
    EXPECT_EQ(Inner.Children.size(), 2U);
    EXPECT_EQ(undoSourceRewrite(A, A.Text), Text);
  }
}

TEST(WebSourceBranch, CallsOptionalCallsAndTaggedTemplatesRetainValueCallees) {
  expectRewrite("(true ? obj.method : other)();", "((0,(obj.method)))();");
  expectRewrite("(false || obj.method)?.();", "((0,(obj.method)))?.();");
  expectRewrite("(true ? obj.method : other)`tag`;",
                "((0,(obj.method)))`tag`;");
  expectRewrite("(true ? eval : other)('code');", "((0,(eval)))('code');");
}

TEST(WebSourceBranch, TypeofAndDeleteRetainReadAccessAndExternalResolution) {
  for (const auto &[Before, After] :
       {std::pair{"typeof (true ? missing : 0);", "typeof ((0,(missing)));"},
        {"delete (true ? obj.key : 0);", "delete ((0,(obj.key)));"}}) {
    expectRewrite(Before, After);
    const auto S = inspectJavaScript("read-test", After, "script");
    const auto B = analyzeSourceBindings(S);
    ASSERT_EQ(B.Status, "ok");
    ASSERT_EQ(B.References.size(), 1U);
    EXPECT_EQ(B.References[0].Access, "read");
    EXPECT_EQ(B.References[0].Resolution, "external");
  }
}

TEST(WebSourceBranch, RetainedLexicalScopesAndReferencesKeepTheirOwnership) {
  expectRewrite("function f(x){let y;{const z=1;"
                "y=true ? x+z : missing;}return y;}",
                "function f(x){let y;{const z=1;"
                "y=(0,(x+z));}return y;}");
  expectRewrite("export const x=true ? external : unused;",
                "export const x=(0,(external));", "module");
  expectRewrite("module.exports=false ? unused : require('kept');",
                "module.exports=(0,(require('kept')));", "commonjs");
}

TEST(WebSourceBranch, OutermostSelectionsPreserveNestedChosenSyntax) {
  const auto A = rewrite("true ? (false ? a : b) : c;"
                         "unknown ? (true ? d : e) : f;");
  ASSERT_EQ(A.Status, "verified_local_rewrites") << A.Reason;
  EXPECT_EQ(A.Text, "(0,(false ? a : b));unknown ? ((0,(d))) : f;");
  EXPECT_EQ(A.Accepted, 2U);
  EXPECT_EQ(A.Refused, 1U);
  EXPECT_EQ(std::count_if(A.Records.begin(), A.Records.end(),
                          [](const auto &R) { return R.Status == "covered"; }),
            1);
}

TEST(WebSourceBranch, UnknownCoerciveAndThrowingConditionsRemainUnchanged) {
  for (const auto *Text :
       {"unknown ? a : b;", "1 ? a : b;", "'' || a;", "null ?? a;",
        "call() ? a : b;", "(1n/0n) ? a : b;", "(void call(),true) ? a : b;"}) {
    const auto A = rewrite(Text);
    EXPECT_EQ(A.Status, "no_eligible_rewrites") << Text << ": " << A.Reason;
    EXPECT_EQ(A.Accepted, 0U);
    EXPECT_EQ(A.Text, Text);
    EXPECT_EQ(undoSourceRewrite(A, A.Text), Text);
  }
}

TEST(WebSourceBranch, ScopeAndDeclarationSyntaxAreNeverRemoved) {
  for (const auto *Text :
       {"true ? 1 : function(){};", "true ? (()=>1) : other;",
        "false && class { static x=1; };", "true ? 1 : {method(){}};"}) {
    const auto A = rewrite(Text);
    EXPECT_EQ(A.Status, "no_eligible_rewrites") << A.Reason << Text;
    ASSERT_FALSE(A.Records.empty());
    EXPECT_EQ(A.Records.front().Reason, "scope_or_declaration_preservation");
    EXPECT_EQ(A.Text, Text);
  }
  // An outer scope does not prohibit independent expression selection inside.
  expectRewrite("async function f(){return true ? 1 : other;}",
                "async function f(){return (0,(1));}");
  const auto Statements = rewrite("if(false){var x;}else effect();");
  EXPECT_EQ(Statements.Status, "no_eligible_rewrites");
  EXPECT_EQ(Statements.Text, "if(false){var x;}else effect();");
}

TEST(WebSourceBranch, UnreachableAwaitStillPreservesModuleAsynchrony) {
  for (const auto *Text : {"export const x = true ? 1 : await pending;",
                           "(false && await pending) ? left : right;",
                           "true ? await pending : other;"}) {
    const auto A = rewrite(Text, "module");
    EXPECT_EQ(A.Status, "no_eligible_rewrites") << A.Reason;
    EXPECT_EQ(A.Text, Text);
    ASSERT_FALSE(A.Records.empty());
    for (const auto &R : A.Records)
      EXPECT_EQ(R.Reason, "await_syntax_preservation");
  }
  expectRewrite("async function f(){true ? 1 : await pending;"
                "return false ? other : 2;}",
                "async function f(){true ? 1 : await pending;"
                "return (0,(2));}");
}

TEST(WebSourceBranch, CommentsDirectivesAndUnselectedBytesRemainExact) {
  const std::string Text =
      "#!/usr/bin/env node\n/*license*/true ? 1 /*keep*/ : 2;\n"
      "false ? unused : kept;//tail\n//# sourceMappingURL=private.map\n";
  expectRewrite(Text, "#!/usr/bin/env node\n/*license*/true ? 1 /*keep*/ : 2;\n"
                      "(0,(kept));//tail\n//# sourceMappingURL=private.map\n");
  expectRewrite("true ? 'use strict' : other;function f(a,a){}",
                "(0,('use strict'));function f(a,a){}");
}

TEST(WebSourceBranch, ASIAndDynamicScopeRefuseWithoutPartialPublication) {
  for (const auto *Text : {"foo()\ntrue ? 1 : 2;", "eval(code);true ? 1 : 2;",
                           "with(obj){true ? 1 : 2;}"}) {
    const auto A = rewrite(Text);
    EXPECT_EQ(A.Status, "unavailable") << A.Reason << Text;
    EXPECT_TRUE(A.Text.empty());
    EXPECT_TRUE(A.CandidateHash.empty());
    EXPECT_TRUE(A.Records.empty());
  }
  expectRewrite("foo();\ntrue ? 1 : 2;", "foo();\n(0,(1));");
  EXPECT_EQ(rewrite("true ?").Reason, "source_rewrite_source_not_parsed");
}

TEST(WebSourceBranch, ReceiptTamperingCannotRestoreAnotherSelection) {
  const auto A = rewrite("true ? left : right;");
  ASSERT_EQ(A.Status, "verified_local_rewrites") << A.Reason;
  EXPECT_THROW(undoSourceRewrite(A, A.Text + " "), Error);
  for (unsigned I = 0; I != 9; ++I) {
    auto Bad = A;
    auto &R = Bad.Records.front();
    switch (I) {
    case 0:
      R.ConditionID = "different";
      break;
    case 1:
      R.RetainedID = "different";
      break;
    case 2:
      R.Selection = "alternate";
      break;
    case 3:
      ++R.Condition;
      break;
    case 4:
      ++R.Retained;
      break;
    case 5:
      ++R.SourceStart;
      break;
    case 6:
      ++R.Start;
      break;
    case 7:
      R.After[1] = '1';
      break;
    case 8:
      Bad.Pass = "constant_folding";
      break;
    }
    EXPECT_THROW(undoSourceRewrite(Bad, A.Text), Error) << I;
  }
}

TEST(WebSourceBranch, ByteRecordAndLexerLimitsRemainAtomic) {
  EXPECT_EQ(rewrite(std::string(MaxSourceRewriteBytes + 1, ' ')).Status,
            "budget_exceeded");
  std::string Many;
  for (uint64_t I = 0; I <= MaxSourceRewriteRecords; ++I)
    Many += "true?1:2;";
  const auto Records = rewrite(Many);
  EXPECT_EQ(Records.Reason, "source_rewrite_record_budget_exceeded");
  EXPECT_TRUE(Records.Text.empty());
  EXPECT_TRUE(Records.Records.empty());
  const auto Expanded =
      rewrite("/*" + std::string(MaxSourceRewriteBytes - 10, 'x') + "*/!0&&a;");
  EXPECT_EQ(Expanded.Reason, "source_rewrite_byte_budget_exceeded");
  EXPECT_TRUE(Expanded.Text.empty());
  EXPECT_TRUE(Expanded.Records.empty());
  std::string Comments;
  for (uint64_t I = 0; I <= MaxJavaScriptLexemes; ++I)
    Comments += "/**/";
  const auto Lex = rewrite(Comments + "true?1:2;");
  EXPECT_EQ(Lex.Reason, "source_rewrite_lexemes_unavailable");
  EXPECT_TRUE(Lex.Text.empty());
  EXPECT_TRUE(Lex.Records.empty());
}

TEST(WebSourceBranch, WorkExhaustionDropsAllTentativeSelections) {
  std::string Large;
  for (unsigned I = 0; I < 16; ++I)
    Large += "true?'" + std::string(60000, 'x') + "':0;";
  const auto A = rewrite(Large);
  EXPECT_EQ(A.Status, "budget_exceeded") << A.Reason;
  EXPECT_EQ(A.Reason, "source_rewrite_work_budget_exceeded");
  EXPECT_TRUE(A.Text.empty());
  EXPECT_TRUE(A.CandidateHash.empty());
  EXPECT_TRUE(A.Records.empty());
  EXPECT_EQ(A.Accepted, 0U);
}

TEST(WebSourceBranch,
     ClaudeCode21296OriginalMembersRetainSelectionAndRefusals) {
  const auto *Path = std::getenv("NEVERD_CLAUDE_CODE_21296_ELF");
  if (!Path)
    GTEST_SKIP() << "Optional hash-pinned official original not supplied";
  const auto Snapshot = capture(Path, Limits{});
  ASSERT_EQ(Snapshot.Artifacts.size(), 1U);
  const auto &Input = Snapshot.Artifacts.front();
  ASSERT_EQ(Input.BlobHash,
            "24972e3bc859fab2b46ed4c1e51f7d6130f06d3bd550811a114640de3370d0de");
  const auto E = extractBun(Input);
  ASSERT_GT(E.Modules.size(), 1897U);
  for (const uint32_t Index : {1047U, 1897U}) {
    const auto &M = E.Modules[Index];
    ASSERT_EQ(M.Format, 1U);
    const auto Bytes = bunSourceBytes(E, M, MaxSourceRewriteBytes);
    const auto A = rewriteJavaScript(M.SourceArtifactID, Bytes, "module",
                                     "boolean_branches");
    EXPECT_EQ(A.Status, Index == 1047 ? "verified_local_rewrites"
                                      : "no_eligible_rewrites")
        << A.Reason;
    EXPECT_EQ(A.Accepted, Index == 1047 ? 1U : 0U);
    EXPECT_EQ(A.Refused, Index == 1047 ? 6U : 1007U);
    EXPECT_EQ(
        A.SourceHash,
        Index == 1047
            ? "faa820c7d6f239d9404d70278eb294ab926bc9c1ad4a32993b047d33baa62a91"
            : "1be18858dfe6863b48e9e0c25e25f9d04b22221935cb1a9e286cbd992d70f72"
              "2");
    EXPECT_EQ(
        A.CandidateHash,
        Index == 1047
            ? "e15ee77e5296c38f3d1065a46d9f7a7023340f7adaca86f3b6b8b6da2f20561d"
            : A.SourceHash);
    EXPECT_EQ(undoSourceRewrite(A, A.Text), Bytes);
  }
  EXPECT_THROW(bunSourceBytes(E, E.Modules[1613], MaxSourceRewriteBytes),
               Error);
}
} // namespace
