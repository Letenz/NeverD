//===- SourceRewriteTests.cpp - Local substitution receipt contracts ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Primitive values, outside syntax, bounded refusal and exact reversal.
///
//===----------------------------------------------------------------------===//

#include "Internal.h"
#include "gtest/gtest.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Bun.h"
#include "neverd/web/Error.h"
#include "neverd/web/SourceRewrite.h"

#include <algorithm>
#include <cstdlib>

namespace {
using namespace neverd::web;

SourceRewrite rewrite(std::string_view Text,
                      std::string_view Pass = "constant_folding",
                      std::string_view Type = "script") {
  return rewriteJavaScript(identity("rewrite-test", {Text}), Text, Type, Pass);
}

void expectRewrite(std::string_view Before, std::string_view After,
                   std::string_view Pass = "constant_folding") {
  const auto A = rewrite(Before, Pass);
  ASSERT_EQ(A.Status, "verified_local_rewrites") << A.Reason << ": " << Before;
  EXPECT_EQ(A.Text, After);
  EXPECT_EQ(A.SourceHash, sha256(Before));
  EXPECT_EQ(A.CandidateHash, sha256(After));
  EXPECT_EQ(undoSourceRewrite(A, A.Text), Before);
  const auto Again = rewrite(Before, Pass);
  EXPECT_EQ(A.ID, Again.ID);
  ASSERT_EQ(A.Records.size(), Again.Records.size());
  for (size_t I = 0; I < A.Records.size(); ++I)
    EXPECT_EQ(A.Records[I].ID, Again.Records[I].ID);
}

TEST(WebSourceRewrite, ExactStringsTemplatesAndUtf16PreserveSurrogates) {
  expectRewrite("const x = 'se' + 'cret';", "const x = (\"secret\");",
                "string_recovery");
  expectRewrite("const x = `id=${1+2}:${true}`;", "const x = (\"id=3:true\");",
                "string_recovery");
  expectRewrite(R"(const x = '\ud800' + '\0' + '\udfff';)",
                R"(const x = ("\ud800\u0000\udfff");)", "string_recovery");
  expectRewrite(R"(const x = '\u2028' + '"' + '\\';)",
                R"(const x = ("\u2028\u0022\u005c");)", "string_recovery");
  expectRewrite("const x = typeof null;", "const x = (\"object\");",
                "string_recovery");
}

TEST(WebSourceRewrite, NumbersBigIntsAndBooleansRetainSameValue) {
  for (const auto &[Input, Output] :
       {std::pair{"1+2*3", "7"},
        {"-4 % 2", "-0"},
        {"1e308 * 1e308", "1/0"},
        {"1 / -0", "-1/0"},
        {"0 / 0", "0/0"},
        {"0.1 + 0.2", "0.30000000000000004"},
        {"5e-324 / 2", "0"},
        {"~1n", "-2n"},
        {"2n ** 100n", "1267650600228229401496703205376n"},
        {"void 42", "void 0"},
        {"!0", "true"},
        {"1 === true", "false"}})
    expectRewrite("const x=" + std::string(Input) + ";",
                  "const x=(" + std::string(Output) + ");");
}

TEST(WebSourceRewrite, ExplicitPassesNeverRewriteOtherValueKinds) {
  const std::string Text = "const a='a'+'b';const b=2+3;";
  const auto Strings = rewrite(Text, "string_recovery");
  const auto Constants = rewrite(Text);
  EXPECT_NE(Strings.ID, Constants.ID);
  EXPECT_EQ(Strings.Text, "const a=(\"ab\");const b=2+3;");
  EXPECT_EQ(Constants.Text, "const a='a'+'b';const b=(5);");
  EXPECT_EQ(Strings.Accepted, 1u);
  EXPECT_EQ(Constants.Accepted, 1u);
  EXPECT_EQ(rewrite(Text, "guess").Reason, "unknown_source_rewrite_pass");
}

TEST(WebSourceRewrite, OutermostSelectionDoesNotHideEligibleChildrenOfRefusal) {
  const auto A = rewrite("const a=1+2*3;const b=unknown+(4+5);");
  ASSERT_EQ(A.Status, "verified_local_rewrites") << A.Reason;
  EXPECT_EQ(A.Text, "const a=(7);const b=unknown+((9));");
  EXPECT_EQ(A.Accepted, 2u);
  EXPECT_EQ(A.Refused, 1u);
  EXPECT_EQ(std::count_if(A.Records.begin(), A.Records.end(),
                          [](const auto &R) { return R.Status == "covered"; }),
            1);
  const auto &First = A.Records.front();
  EXPECT_EQ(First.Before, "1+2*3");
  EXPECT_EQ(A.Text.substr(First.Start, First.End - First.Start), First.After);
}

TEST(WebSourceRewrite, CommentsLicensesHashbangAndSourceDirectivesRemainExact) {
  const std::string Text =
      "#!/usr/bin/env node\n/*license*/\nconst a=1 /*KEEP*/ + 2;\n"
      "const b=3+4;// tail\n//# sourceMappingURL=private.map\n";
  const auto A = rewrite(Text);
  ASSERT_EQ(A.Status, "verified_local_rewrites") << A.Reason;
  EXPECT_EQ(A.Text,
            "#!/usr/bin/env node\n/*license*/\nconst a=1 /*KEEP*/ + 2;\n"
            "const b=(7);// tail\n//# sourceMappingURL=private.map\n");
  ASSERT_EQ(A.Records.size(), 2u);
  EXPECT_EQ(A.Records[0].Reason, "comment_preservation");
  EXPECT_EQ(undoSourceRewrite(A, A.Text), Text);
}

TEST(WebSourceRewrite, DirectiveProloguesAndTaggedTemplatesDoNotChange) {
  expectRewrite("'use '+ 'strict';function f(a,a){}",
                "(\"use strict\");function f(a,a){}", "string_recovery");
  const auto Tagged = rewrite("tag`x${1+2}`;const y=3+4;");
  ASSERT_EQ(Tagged.Status, "verified_local_rewrites") << Tagged.Reason;
  EXPECT_EQ(Tagged.Text, "tag`x${1+2}`;const y=(7);");
  EXPECT_EQ(Tagged.Accepted, 1u);
  EXPECT_EQ(Tagged.Refused, 2u);
  const auto Canonical = rewrite("const a=-0;const b=1/0;const c=void 0;");
  EXPECT_EQ(Canonical.Status, "no_eligible_rewrites");
  EXPECT_EQ(Canonical.Accepted, 0u);
}

TEST(WebSourceRewrite, ASIChangesRefuseAtomicallyEvenWhenCandidateParses) {
  for (const auto *Text : {"foo()\n1+2;", "foo\n1+2;", "const a=foo\n1+2;"}) {
    const auto A = rewrite(Text);
    EXPECT_EQ(A.Status, "unavailable") << Text;
    EXPECT_EQ(A.Reason, "source_rewrite_outer_tree_mismatch") << Text;
    EXPECT_TRUE(A.Text.empty());
    EXPECT_TRUE(A.CandidateHash.empty());
    EXPECT_TRUE(A.Records.empty());
  }
  expectRewrite("foo();\n1+2;", "foo();\n(3);");
}

TEST(WebSourceRewrite, CallsCoercionExceptionsAndDynamicScopesRemainUnknown) {
  for (const auto *Text :
       {"const x = obj + 1;", "const x = call() + 1;",
        "const x = ({get x(){return 1;}}).x + 1;", "const x = 1n / 0n;",
        "const x = +1n;", "const x = 1n + 1;", "const x = void call();"}) {
    const auto A = rewrite(Text);
    EXPECT_EQ(A.Status, "no_eligible_rewrites") << A.Reason << Text;
    EXPECT_EQ(A.Text, Text);
    EXPECT_EQ(A.Accepted, 0u);
    EXPECT_EQ(undoSourceRewrite(A, A.Text), Text);
  }
  for (const auto *Text :
       {"eval(code);const x=1+2;", "with(obj){const x=1+2;}"}) {
    const auto A = rewrite(Text);
    EXPECT_EQ(A.Reason, "source_rewrite_dynamic_scope");
    EXPECT_TRUE(A.Text.empty());
    EXPECT_TRUE(A.Records.empty());
  }
}

TEST(WebSourceRewrite, InverseRejectsChangedCandidateReceiptRangesAndBytes) {
  const auto A = rewrite("const x=1+2;const y=3+4;");
  ASSERT_EQ(A.Status, "verified_local_rewrites") << A.Reason;
  EXPECT_THROW(undoSourceRewrite(A, A.Text + " "), Error);
  auto Bad = A;
  Bad.Records.front().SourceStart++;
  EXPECT_THROW(undoSourceRewrite(Bad, A.Text), Error);
  Bad = A;
  Bad.Records.front().Start++;
  EXPECT_THROW(undoSourceRewrite(Bad, A.Text), Error);
  Bad = A;
  Bad.Records.front().Before[0] = '9';
  EXPECT_THROW(undoSourceRewrite(Bad, A.Text), Error);
  Bad = A;
  Bad.Records.front().After[0] = '[';
  EXPECT_THROW(undoSourceRewrite(Bad, A.Text), Error);
  Bad = A;
  Bad.Records.front().ID = "other";
  EXPECT_THROW(undoSourceRewrite(Bad, A.Text), Error);
  Bad = A;
  Bad.Accepted++;
  EXPECT_THROW(undoSourceRewrite(Bad, A.Text), Error);
  Bad = A;
  Bad.Pass = "string_recovery";
  EXPECT_THROW(undoSourceRewrite(Bad, A.Text), Error);
}

TEST(WebSourceRewrite, BudgetsAndIncompleteLexerPublishNoCandidate) {
  const auto Big = rewrite(std::string(MaxSourceRewriteBytes + 1, ' '));
  EXPECT_EQ(Big.Status, "budget_exceeded");
  EXPECT_TRUE(Big.Text.empty());
  std::string Many;
  for (uint64_t I = 0; I <= MaxSourceRewriteRecords; ++I)
    Many += "1+2;";
  const auto Records = rewrite(Many);
  EXPECT_EQ(Records.Reason, "source_rewrite_record_budget_exceeded");
  EXPECT_TRUE(Records.Records.empty());
  EXPECT_TRUE(Records.Text.empty());
  std::string Comments;
  for (uint64_t I = 0; I <= MaxJavaScriptLexemes; ++I)
    Comments += "/**/";
  Comments += "1 /*KEEP*/ + 2;";
  const auto Lex = rewrite(Comments);
  EXPECT_EQ(Lex.Reason, "source_rewrite_lexemes_unavailable");
  EXPECT_TRUE(Lex.Text.empty());
  EXPECT_TRUE(Lex.Records.empty());
  // Escaping raw UTF-8 can grow the derived file beyond the admitted source
  // profile even though each value and the original remain within budgets.
  std::string Expanded;
  for (unsigned I = 0; I < 10; ++I) {
    Expanded += "const x" + std::to_string(I) + "='";
    for (unsigned J = 0; J < 20000; ++J)
      Expanded += "\xc2\xa0";
    Expanded += "'+'';";
  }
  const auto Output = rewrite(Expanded, "string_recovery");
  EXPECT_EQ(Output.Status, "budget_exceeded") << Output.Reason;
  EXPECT_TRUE(Output.Text.empty());
  EXPECT_TRUE(Output.Records.empty());
}

TEST(WebSourceRewrite, ClaudeCode21296Module1897ReversesBothSelectedPasses) {
  const auto *Path = std::getenv("NEVERD_CLAUDE_CODE_21296_ELF");
  if (!Path)
    GTEST_SKIP() << "Optional hash-pinned official original not supplied";
  const auto Snapshot = capture(Path, Limits{});
  ASSERT_EQ(Snapshot.Artifacts.size(), 1u);
  const auto &Original = Snapshot.Artifacts.front();
  ASSERT_EQ(Original.BlobHash,
            "24972e3bc859fab2b46ed4c1e51f7d6130f06d3bd550811a114640de3370d0de");
  const auto E = extractBun(Original);
  ASSERT_GT(E.Modules.size(), 1897u);
  const auto &M = E.Modules[1897];
  const auto Bytes = bunSourceBytes(E, M, MaxSourceRewriteBytes);
  for (const auto *Pass : {"string_recovery", "constant_folding"}) {
    const auto A = rewriteJavaScript(
        M.SourceArtifactID, Bytes, M.Format == 1 ? "module" : "commonjs", Pass);
    ASSERT_TRUE(A.Status == "verified_local_rewrites" ||
                A.Status == "no_eligible_rewrites")
        << Pass << ": " << A.Reason;
    EXPECT_EQ(A.Accepted,
              std::string_view(Pass) == "string_recovery" ? 5u : 201u);
    EXPECT_EQ(A.Refused, 1496u);
    EXPECT_EQ(undoSourceRewrite(A, A.Text), Bytes);
    RecordProperty(std::string(Pass) + "_accepted", int(A.Accepted));
    RecordProperty(std::string(Pass) + "_refused", int(A.Refused));
  }
}

} // namespace
