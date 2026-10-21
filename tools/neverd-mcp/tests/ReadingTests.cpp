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
                              "i64 s10; u32 s8; } stack_storage;"),
          "a frame is declared on one line, its slots named sN");
  require(contains(View.Text, "    i64 t1_2, t1_3;\n    u8 t4_5;\n"),
          "declarations of one type merge");
  require(contains(View.Text, "    s10 = arg0;\n"),
          "frame members drop stack_storage.");
  require(
      contains(View.Text, "    t1_2 = g_A010;\n    g_A010 = (i32)t1_2 + 1;\n"),
      "a typed global access is named by its address");
  require(contains(View.Text, "return (i32)(uptr)&s8;"),
          "uintptr_t is abbreviated");
  require(View.Legend == "iN/uN: intN_t/uintN_t, uptr: uintptr_t; "
                         "sN/gapN: stack_storage members var_mN/gapN, sN the "
                         "slot at -0xN; globals: i32 g_A010",
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

MCP_TEST(AnAddressReadWithTwoTypesIsDeclaredWithTheCommonerOne) {
  const auto View =
      readingView("void f(void) {\n    t = *(int32_t "
                  "*)(uintptr_t)(&g_A000_bytes[4]);\n    u = *(uint32_t "
                  "*)(uintptr_t)(&g_A000_bytes[4]);\n    *(uint32_t "
                  "*)(uintptr_t)(&g_A000_bytes[4]) = 2;\n}\n");
  require(contains(View.Text, "    t = *(i32 *)&g_A004;\n    u = g_A004;\n"
                              "    g_A004 = 2;\n"),
          "the other type keeps its cast: " + View.Text);
  require(contains(View.Legend, "globals: u32 g_A004"),
          "the name has the commoner type: " + View.Legend);
}

MCP_TEST(AnAddressTakenAsAnIntegerIsNamed) {
  const auto View = readingView(
      "void f(void) {\n    p = (uintptr_t)&g_A000_bytes[8];\n    q = "
      "(uintptr_t)(&g_A000_bytes[8]);\n    x = *(int64_t "
      "*)(uintptr_t)(&g_A000_bytes[16]);\n    r = (uintptr_t)&g_A000_bytes[16];"
      "\n    s = &g_A000_bytes[8] + 1;\n}\n");
  require(contains(View.Text, "    p = (uptr)&g_A008;\n    q = (uptr)&g_A008;\n"
                              "    x = g_A010;\n    r = (uptr)&g_A010;\n"),
          "an address taken as an integer is named: " + View.Text);
  require(contains(View.Text, "    s = &g_A000_bytes[8] + 1;\n"),
          "pointer arithmetic on the array keeps its spelling");
  require(contains(View.Legend, "globals: u8 g_A008[]; i64 g_A010"),
          "an address only taken is a byte array: " + View.Legend);
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

MCP_TEST(SlotsKeepTheirNamesWhereAShortNameIsTaken) {
  const auto View =
      readingView("void f(int64_t s8) {\n    stack_storage.var_m8 = s8;\n}\n");
  require(contains(View.Text, "    var_m8 = s8;\n"),
          "a slot keeps var_mN where sN names something else: " + View.Text);
  require(contains(View.Legend, "var_mN/gapN: stack_storage members"),
          "the legend says so: " + View.Legend);
}

MCP_TEST(TemporariesAreNumberedAcrossTheWholeFunction) {
  const std::string First = "int32_t f(int32_t t1) {\n    int32_t t9_4;\n"
                            "    int32_t t3_2;\n    t9_4 = t1;\n";
  const std::string Second = "    t3_2 = t9_4 + 1;\n    return t3_2;\n}\n";
  const std::string Whole = First + Second;
  // t1 is taken, so the first temporary is t2 on every page.
  const auto Head = readingView(First, Whole);
  require(contains(Head.Text, "    i32 t2, t3;\n    t2 = t1;\n"),
          "the first page: " + Head.Text);
  const auto Tail = readingView(Second, Whole);
  require(Tail.Text == "    t3 = t2 + 1;\n    return t3;\n}\n",
          "the second page numbers them as the first does: " + Tail.Text);
  require(contains(Tail.Legend, "tN: the temporaries tB_I, numbered in order"),
          "the legend says so: " + Tail.Legend);
  require(contains(readingView(Second).Text, "t3_2 = t9_4 + 1;"),
          "a page alone keeps the engine's names");
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

MCP_TEST(ARunOfDeclarationsMergesByTypeAndKeepsPointers) {
  const auto View = readingView("void f(void) {\n    char* a;\n    int64_t "
                                "b;\n    char* c;\n    int64_t d;\n    "
                                "servent** e;\n    a = 0;\n    char* g;\n}\n");
  require(contains(View.Text, "    char *a, *c;\n    i64 b, d;\n    servent** "
                              "e;\n    a = 0;\n    char* g;\n"),
          "each pointer name keeps its star: " + View.Text);
}
