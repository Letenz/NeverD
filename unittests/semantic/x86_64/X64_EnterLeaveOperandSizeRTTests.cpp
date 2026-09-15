//===- X64_EnterLeaveOperandSizeRTTests.cpp - ENTERW/LEAVEW width -*- C++ -*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Operand-size 16 ENTER/LEAVE in 64-bit mode (66H prefix) push/pop BP as
// 2-byte values while RSP stays 64-bit.  ENTERW writes only BP, so RBP's high
// bits survive.  The previous lifter always used pointer width, which turned
// ENTERW into ENTER and clobbered those bits.  The Unicorn fork had the same
// ENTER defect (EBP written at stack-address size).
//
// The distinctive RBP immediate makes Unicorn-vs-CPU and NeverD-vs-CPU
// mismatches observable: a 64-bit FrameTemp write yields a stack-page high
// half, not 0xAAAABBBBCCCC.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64EnterLeaveOperandSizeRT : public SemanticRoundTripFixture,
                                   public ::testing::WithParamInterface<RoundTripTC> {
};

TEST_P(X64EnterLeaveOperandSizeRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  {"enterw_preserves_rbp_high",
   "unsigned long f(void){\n"
   "  unsigned long d;\n"
   "  __asm__ volatile(\n"
   "    \"pushq %%rbp\\n\\t\"\n"
   "    \"movq %%rsp, %%rcx\\n\\t\"\n"
   "    \"movq $0xAAAABBBBCCCCDDDD, %%rbp\\n\\t\"\n"
   "    \"data16 enter $0, $1\\n\\t\"\n"
   "    \"movq %%rbp, %0\\n\\t\"\n"
   "    \"movq %%rcx, %%rsp\\n\\t\"\n"
   "    \"popq %%rbp\\n\\t\"\n"
   "    :\"=r\"(d)::\"rcx\",\"cc\",\"memory\");\n"
   "  return d >> 16;}\n",
   {}, "EnterLeaveOperandSize"},

  {"enterw_sz16_sp_delta",
   "unsigned long f(void){\n"
   "  unsigned long d;\n"
   "  __asm__ volatile(\n"
   "    \"data16 enter $16, $1\\n\\t\"\n"
   "    \"movq %%rsp, %0\\n\\t\"\n"
   "    \"subq %%rbp, %0\\n\\t\"\n"
   "    \"data16 leave\\n\\t\"\n"
   "    :\"=r\"(d)::\"cc\",\"memory\");\n"
   "  return d;}\n",
   {}, "EnterLeaveOperandSize"},

  {"enter_sz16_sp_delta",
   "unsigned long f(void){\n"
   "  unsigned long d;\n"
   "  __asm__ volatile(\n"
   "    \"enter $16, $1\\n\\t\"\n"
   "    \"movq %%rsp, %0\\n\\t\"\n"
   "    \"subq %%rbp, %0\\n\\t\"\n"
   "    \"leave\\n\\t\"\n"
   "    :\"=r\"(d)::\"cc\",\"memory\");\n"
   "  return d;}\n",
   {}, "EnterLeaveOperandSize"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(EnterLeaveOperandSize, X64EnterLeaveOperandSizeRT,
                         ::testing::ValuesIn(kX64), rtTCName);
