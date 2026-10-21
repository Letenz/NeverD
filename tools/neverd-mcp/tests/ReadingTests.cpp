//===- ReadingTests.cpp - The reading view of NeverD C --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Every abbreviation is listed and reversible; literals keep their text.
///
//===----------------------------------------------------------------------===//

#include "Reading.h"
#include "Testing.h"

using namespace neverd::binmcp;
using neverd::binmcp::test::require;

namespace {
constexpr const char *Function = R"(int32_t f(int64_t arg0) {
    _Alignas(16) struct frame_1000 {
        uint8_t gap0[8];
        int64_t var_m10;
        uint32_t var_m8;
    } stack_storage;
    int64_t t1_2;
    int64_t t1_3;
    uint8_t t4_5;
    stack_storage.var_m10 = arg0;
    t1_2 = *(int32_t *)(uintptr_t)(&g_A000_bytes[16]);
    *(int32_t *)(uintptr_t)(&g_A000_bytes[16]) = (int32_t)t1_2 + 1;
    puts("int64_t stack_storage.var_m10 /* not code */");
    return (int32_t)(uintptr_t)&stack_storage.var_m8;
}
)";

bool contains(const std::string &Text, const std::string &Part) {
  return Text.find(Part) != std::string::npos;
}
} // namespace

MCP_TEST(AbbreviationsAreListedAndReversible) {
  const auto View = readingView(Function);
  require(View.Text.substr(0, View.Text.find('\n')) == "i32 f(i64 arg0) {",
          "types are abbreviated in a signature");
  require(contains(View.Text, "_Alignas(16) struct frame_1000 { u8 gap0[8]; "
                              "i64 var_m10; u32 var_m8; } stack_storage;"),
          "a frame is declared on one line");
  require(contains(View.Text, "    i64 t1_2, t1_3;\n    u8 t4_5;\n"),
          "declarations of one type merge");
  require(contains(View.Text, "    var_m10 = arg0;\n"),
          "frame members drop stack_storage.");
  require(
      contains(View.Text, "    t1_2 = g_A010;\n    g_A010 = (i32)t1_2 + 1;\n"),
      "a typed global access is named by its address");
  require(contains(View.Text, "return (i32)(uptr)&var_m8;"),
          "uintptr_t is abbreviated");
  require(View.Legend == "iN/uN: intN_t/uintN_t, uptr: uintptr_t; "
                         "var_mN/gapN: stack_storage members; globals: i32 "
                         "g_A010",
          "the legend lists each abbreviation: " + View.Legend);
}

MCP_TEST(LiteralsAndCommentsKeepTheirText) {
  require(contains(readingView(Function).Text,
                   "puts(\"int64_t stack_storage.var_m10 /* not code */\");"),
          "a string literal keeps its text");
  require(readingView("/* int64_t stays */\nint64_t x;\n").Text ==
              "/* int64_t stays */\ni64 x;\n",
          "a comment keeps its text");
}

MCP_TEST(AnAddressReadWithTwoTypesKeepsItsCasts) {
  const auto View =
      readingView("void f(void) {\n    t = *(int32_t "
                  "*)(uintptr_t)(&g_A000_bytes[4]);\n    u = *(uint32_t "
                  "*)(uintptr_t)(&g_A000_bytes[4]);\n}\n");
  require(contains(View.Text, "*(i32 *)(uptr)(&g_A000_bytes[4])") &&
              contains(View.Text, "*(u32 *)(uptr)(&g_A000_bytes[4])"),
          "both typed reads keep their casts");
  require(!contains(View.Legend, "globals"), "no global is named");
}

MCP_TEST(NamesTheTextAlreadyUsesAreNotReused) {
  const auto View = readingView(
      "void f(int64_t i64) {\n    int32_t g_A004 = 0;\n    var_m8 = 1;\n"
      "    stack_storage.var_m8 = *(int32_t "
      "*)(uintptr_t)(&g_A000_bytes[4]);\n}\n");
  require(contains(View.Text, "void f(int64_t i64)"),
          "a type keeps its name where the short one is taken");
  require(contains(View.Text, "stack_storage.var_m8"),
          "a frame member keeps its prefix where its bare name is taken");
  require(contains(View.Text, "&g_A000_bytes[4]"),
          "an access keeps its spelling where its name is taken");
  require(!contains(View.Legend, "var_mN"), "no frame note");
}

MCP_TEST(TextWithoutAbbreviationsHasNoLegend) {
  const std::string Text = "int main(void) {\n    return 0;\n}\n";
  const auto View = readingView(Text);
  require(View.Text == Text && View.Legend.empty(),
          "the view is the text itself");
}

MCP_TEST(OneWordStatementsAreNoDeclarations) {
  const std::string Text = "void f(int32_t x) {\n    for (;;) {\n        "
                           "continue;\n        break;\n    }\n    x;\n    "
                           "return;\n}\n";
  require(readingView(Text).Text ==
              "void f(i32 x) {\n    for (;;) {\n        continue;\n        "
              "break;\n    }\n    x;\n    return;\n}\n",
          "a statement of one word is no type and name");
}
