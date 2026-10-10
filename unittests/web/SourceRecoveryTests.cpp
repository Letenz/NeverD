#include "gtest/gtest.h"

#include "neverd/web/SourceRecovery.h"

#include <algorithm>

using namespace neverd::web;

TEST(WebSourceRecovery, KeepsCommentsLiteralsTemplatesAndASIWhileFormatting) {
  const std::string Input = R"JS(// license must remain
import{x}from'./dependency.js';function f(a){if(a){return /[{};]/g.test(`x${a};`)}return
{x:1}}const o={return(){return '};not syntax'}},v=()=>({a:1});for(let i=0;i<3;i++){f(i)}
)JS";
  const auto R = recoverReadableJavaScript("source", Input, "module");
  EXPECT_EQ(R.ParseStatus, "parsed");
  ASSERT_EQ(R.Status, "verified_same_parser_tree");
  EXPECT_NE(R.Text.find("// license must remain"), std::string::npos);
  EXPECT_NE(R.Text.find("/[{};]/g"), std::string::npos);
  EXPECT_NE(R.Text.find("`x${a};`"), std::string::npos);
  EXPECT_GT(std::count(R.Text.begin(), R.Text.end(), '\n'),
            std::count(Input.begin(), Input.end(), '\n'));
  // All original bytes survive, in order, even original whitespace.
  size_t At = 0;
  for (char C : R.Text)
    if (At < Input.size() && Input[At] == C)
      ++At;
  EXPECT_EQ(At, Input.size());
}

TEST(WebSourceRecovery, RejectsInvalidAndOverBudgetInputWithoutFakeSource) {
  auto R = recoverReadableJavaScript("source", "const = ;", "module");
  EXPECT_EQ(R.Status, "invalid_syntax");
  EXPECT_TRUE(R.Text.empty());
  ASSERT_FALSE(R.Diagnostics.empty());
  EXPECT_EQ(R.Diagnostics.front().Code, "syntax_error");
  EXPECT_GE(R.Diagnostics.front().ByteOffset, 0);
  R = recoverReadableJavaScript(
      "source", std::string(32 * 1024 * 1024 + 1, ' '), "module");
  EXPECT_EQ(R.Status, "source_byte_budget_exceeded");
  EXPECT_TRUE(R.Text.empty());
}

TEST(WebSourceRecovery, AsyncRestFormattingRetainsTheSameSyntaxTree) {
  const std::string Source = "const f=async(...args)=>{try{return await "
                             "run(...args)}finally{done()}};";
  const auto R = recoverReadableJavaScript("source", Source, "module");
  ASSERT_EQ(R.Status, "verified_same_parser_tree");
  EXPECT_TRUE(R.Diagnostics.empty());
  EXPECT_NE(R.Text.find("async(...args)"), std::string::npos);
}

TEST(WebSourceRecovery, LargeRecoveryDoesNotRaiseInteractiveParserLimits) {
  const std::string Source =
      "const x='" + std::string(2 * 1024 * 1024, 'a') + "';";
  EXPECT_ANY_THROW(inspectJavaScript("source", Source, "module"));
  const auto R = recoverReadableJavaScript("source", Source, "module");
  ASSERT_EQ(R.Status, "verified_same_parser_tree");
  EXPECT_TRUE(R.Text.starts_with(Source));
}
