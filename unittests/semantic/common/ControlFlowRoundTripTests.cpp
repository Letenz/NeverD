//===- ControlFlowRoundTripTests.cpp - Control-flow roundtrip tests ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Tests control-flow patterns (if/else, loops, conditional execution) via
// C function roundtrips.  These verify that NeverD correctly lifts control
// flow and produces semantically equivalent recompiled code.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

// These cases run under the one TEST_P of each fixture, X64RoundTrip.LiftVerify
// (X64_RoundTripSemanticTests.cpp), AArch64RoundTrip.LiftVerify
// (AArch64_RoundTripSemanticTests.cpp), ARM32RoundTrip.LiftVerify
// (ARM32_RoundTripSemanticTests.cpp). gtest runs every TEST_P of a fixture on
// each of its instantiations, so another TEST_P here would run all of them
// again.

// clang-format off

static const std::vector<RoundTripTC> kX64ControlRT = {
  {"if_else_eq",
   "long if_else_eq(long a, long b) {\n"
   "  if (a == b) return 42;\n"
   "  return 0;\n"
   "}\n",
   {5, 5}, "ControlFlowRT"},

  {"if_else_neq",
   "long if_else_neq(long a, long b) {\n"
   "  if (a == b) return 42;\n"
   "  return 0;\n"
   "}\n",
   {5, 6}, "ControlFlowRT"},

  {"loop_sum",
   "long loop_sum(long n) {\n"
   "  long s = 0;\n"
   "  for (long i = 1; i <= n; ++i) s += i;\n"
   "  return s;\n"
   "}\n",
   {10}, "ControlFlowRT"},

  {"ternary",
   "long ternary(long a, long b) {\n"
   "  return (a < b) ? a : b;\n"
   "}\n",
   {3, 7}, "ControlFlowRT"},

  {"nested_if",
   "long nested_if(long a, long b) {\n"
   "  if (a > 0) {\n"
   "    if (b > 0) return a + b;\n"
   "    return a;\n"
   "  }\n"
   "  return 0;\n"
   "}\n",
   {10, 20}, "ControlFlowRT"},
};

static const std::vector<RoundTripTC> kA64ControlRT = {
  {"if_else_eq",
   "long if_else_eq(long a, long b) {\n"
   "  if (a == b) return 42;\n"
   "  return 0;\n"
   "}\n",
   {5, 5}, "ControlFlowRT"},

  {"loop_sum",
   "long loop_sum(long n) {\n"
   "  long s = 0;\n"
   "  for (long i = 1; i <= n; ++i) s += i;\n"
   "  return s;\n"
   "}\n",
   {10}, "ControlFlowRT"},

  {"ternary",
   "long ternary(long a, long b) {\n"
   "  return (a < b) ? a : b;\n"
   "}\n",
   {3, 7}, "ControlFlowRT"},
};

static const std::vector<RoundTripTC> kARM32ControlRT = {
  {"if_else_eq",
   "int if_else_eq(int a, int b) {\n"
   "  if (a == b) return 42;\n"
   "  return 0;\n"
   "}\n",
   {5, 5}, "ControlFlowRT"},

  {"loop_sum",
   "int loop_sum(int n) {\n"
   "  int s = 0;\n"
   "  for (int i = 1; i <= n; ++i) s += i;\n"
   "  return s;\n"
   "}\n",
   {10}, "ControlFlowRT"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(X64ControlFlowRT, X64RoundTrip,
                         ::testing::ValuesIn(kX64ControlRT), rtTCName);
INSTANTIATE_TEST_SUITE_P(A64ControlFlowRT, AArch64RoundTrip,
                         ::testing::ValuesIn(kA64ControlRT), rtTCName);
INSTANTIATE_TEST_SUITE_P(ARM32ControlFlowRT, ARM32RoundTrip,
                         ::testing::ValuesIn(kARM32ControlRT), rtTCName);
