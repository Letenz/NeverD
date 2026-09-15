//===- X64_CvtScalarUpperRTTests.cpp - CVTSI2SD/SS scalar typing -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Guardrail for the scalar integer->FP converts CVTSI2SD / CVTSI2SS.
//
// Hardware (and Unicorn) write only the low element (CVTSI2SD -> [63:0],
// CVTSI2SS -> [31:0]) and leave the rest of the XMM unchanged.  clang -O2
// really does consume that preserved high lane: `movapd dst,src; cvtsi2sd
// r32,dst; mulpd dst,src` treats xmm[127:64] as a live packed double (see
// x64o63_dotp / x64o63_det3).  The producer must CONCAT the old upper bytes
// onto a *narrow* convert temp so FLOAT_INT2FLOAT still infers float/double
// from the temp width.  Scalar *SS/*SD consumers extract the low element
// before inferFloatTy(); packed consumers (MULPD/MULPS) read the preserved
// lanes.  The dirty-upper scalar probes lock the extract-at-consumer side;
// the packed-mul probes lock the preserve-at-producer side.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64CvtScalarUpperRT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64CvtScalarUpperRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kCvt = {

  // ===== Narrow-typing lock: CVTSI2* -> scalar op, with a dirtied upper lane ==
  // movq+unpcklpd seeds BOTH 64-bit lanes with `a`; cvtsi2* then overwrites the
  // low element.  The following scalar op MUST treat the result as the narrow
  // FP width (float / double), not as a wide value — that is exactly what
  // breaks if the converter is changed to CONCAT the upper lane back in.
  {"cvtsi2ss_sqrt_dirtyup",  // (float)25 -> sqrt -> 5.0f = 0x40A00000
   "long f(long a){\n"
   "  unsigned out;\n"
   "  __asm__ volatile(\n"
   "    \"movq %1, %%xmm5\\n\\t\"\n"
   "    \"unpcklpd %%xmm5, %%xmm5\\n\\t\"\n"
   "    \"cvtsi2ss %k1, %%xmm5\\n\\t\"\n"
   "    \"sqrtss %%xmm5, %%xmm5\\n\\t\"\n"
   "    \"movd %%xmm5, %0\\n\\t\"\n"
   "    : \"=m\"(out) : \"r\"(a) : \"xmm5\");\n"
   "  return (long)(unsigned)out;\n}\n",
   {25ULL}, "CvtScalar"},

  {"cvtsi2sd_sqrt_dirtyup",  // (double)25 -> sqrt -> 5.0 = 0x4014000000000000
   "long f(long a){\n"
   "  unsigned long long out;\n"
   "  __asm__ volatile(\n"
   "    \"movq %1, %%xmm5\\n\\t\"\n"
   "    \"unpcklpd %%xmm5, %%xmm5\\n\\t\"\n"
   "    \"cvtsi2sd %1, %%xmm5\\n\\t\"\n"
   "    \"sqrtsd %%xmm5, %%xmm5\\n\\t\"\n"
   "    \"movq %%xmm5, %0\\n\\t\"\n"
   "    : \"=m\"(out) : \"r\"(a) : \"xmm5\");\n"
   "  return (long)out;\n}\n",
   {25ULL}, "CvtScalar"},

  {"cvtsi2ss_mul_dirtyup",   // (float)25 squared -> 625.0f = 0x441C4000
   "long f(long a){\n"
   "  unsigned out;\n"
   "  __asm__ volatile(\n"
   "    \"movq %1, %%xmm5\\n\\t\"\n"
   "    \"unpcklpd %%xmm5, %%xmm5\\n\\t\"\n"
   "    \"cvtsi2ss %k1, %%xmm5\\n\\t\"\n"
   "    \"mulss %%xmm5, %%xmm5\\n\\t\"\n"
   "    \"movd %%xmm5, %0\\n\\t\"\n"
   "    : \"=m\"(out) : \"r\"(a) : \"xmm5\");\n"
   "  return (long)(unsigned)out;\n}\n",
   {25ULL}, "CvtScalar"},

  {"cvtsi2sd_mul_dirtyup",   // (double)7 squared -> 49.0 = 0x4048800000000000
   "long f(long a){\n"
   "  unsigned long long out;\n"
   "  __asm__ volatile(\n"
   "    \"movq %1, %%xmm5\\n\\t\"\n"
   "    \"unpcklpd %%xmm5, %%xmm5\\n\\t\"\n"
   "    \"cvtsi2sd %1, %%xmm5\\n\\t\"\n"
   "    \"mulsd %%xmm5, %%xmm5\\n\\t\"\n"
   "    \"movq %%xmm5, %0\\n\\t\"\n"
   "    : \"=m\"(out) : \"r\"(a) : \"xmm5\");\n"
   "  return (long)out;\n}\n",
   {7ULL}, "CvtScalar"},

  // comiss after a convert: another scalar consumer (was broken by the wide
  // re-typing).  (float)a > 0.0f ? 1 : 0.
  {"cvtsi2ss_comiss_dirtyup",
   "long f(long a){\n"
   "  unsigned char b;\n"
   "  __asm__ volatile(\n"
   "    \"movq %1, %%xmm5\\n\\t\"\n"
   "    \"unpcklpd %%xmm5, %%xmm5\\n\\t\"\n"
   "    \"cvtsi2ss %k1, %%xmm5\\n\\t\"\n"
   "    \"xorps %%xmm6, %%xmm6\\n\\t\"\n"
   "    \"comiss %%xmm6, %%xmm5\\n\\t\"\n"
   "    \"seta %0\\n\\t\"\n"
   "    : \"=m\"(b) : \"r\"(a) : \"xmm5\", \"xmm6\", \"cc\");\n"
   "  return (long)b;\n}\n",
   {25ULL}, "CvtScalar"},

  // Packed consumer of the preserved high lane.  This is the clang -O2
  // `cvtsi2sd; mulpd` pattern: converting the low element must not zero
  // xmm[127:64].  a=3, b=2.0; after mulpd the high lane is 4.0.
  {"cvtsi2sd_mulpd_dirtyup",
   "long f(long a, long b){\n"
   "  unsigned long long out;\n"
   "  __asm__ volatile(\n"
   "    \"movq %2, %%xmm5\\n\\t\"\n"
   "    \"unpcklpd %%xmm5, %%xmm5\\n\\t\"\n"
   "    \"cvtsi2sd %k1, %%xmm5\\n\\t\"\n"
   "    \"mulpd %%xmm5, %%xmm5\\n\\t\"\n"
   "    \"movhlps %%xmm5, %%xmm5\\n\\t\"\n"
   "    \"movq %%xmm5, %0\\n\\t\"\n"
   "    : \"=m\"(out) : \"r\"(a), \"r\"(b) : \"xmm5\");\n"
   "  return (long)out;\n}\n",
   {3ULL, 0x4000000000000000ULL}, "CvtScalar"},

  // CVTSI2SS preserves [127:32].  Broadcast 2.0f, overwrite lane 0 with
  // (float)3, mulps, return lane 1 = 4.0f.
  {"cvtsi2ss_mulps_dirtyup",
   "long f(long a, long b){\n"
   "  unsigned out;\n"
   "  __asm__ volatile(\n"
   "    \"movd %2, %%xmm5\\n\\t\"\n"
   "    \"shufps $0, %%xmm5, %%xmm5\\n\\t\"\n"
   "    \"cvtsi2ss %k1, %%xmm5\\n\\t\"\n"
   "    \"mulps %%xmm5, %%xmm5\\n\\t\"\n"
   "    \"psrldq $4, %%xmm5\\n\\t\"\n"
   "    \"movd %%xmm5, %0\\n\\t\"\n"
   "    : \"=m\"(out) : \"r\"(a), \"r\"(b) : \"xmm5\");\n"
   "  return (long)(unsigned)out;\n}\n",
   {3ULL, 0x40000000ULL}, "CvtScalar"},

  // ===== Conversion value / round-to-nearest-even correctness ================
  // int64 just past the double 53-bit mantissa: 2^53+3 -> rounds to 2^53+4.
  {"cvtsi2sd_round_2p53",
   "long f(long a){\n"
   "  double d = (double)a;\n"
   "  long r; __builtin_memcpy(&r, &d, 8); return r;\n}\n",
   {0x20000000000003ULL}, "CvtScalar"},

  // int32 just past the float 24-bit mantissa: 2^24+3 -> rounds to 2^24+4.
  {"cvtsi2ss_round_2p24",
   "long f(long a){\n"
   "  float f = (float)(int)a;\n"
   "  unsigned r; __builtin_memcpy(&r, &f, 4); return (long)(unsigned)r;\n}\n",
   {0x1000003ULL}, "CvtScalar"},

  // 64-bit signed extremes through (float)/(double).
  {"cvtsi2sd_i64_max",
   "long f(long a){\n"
   "  double d = (double)a;\n"
   "  long r; __builtin_memcpy(&r, &d, 8); return r;\n}\n",
   {0x7FFFFFFFFFFFFFFFULL}, "CvtScalar"},

  {"cvtsi2ss_i64_neg",
   "long f(long a){\n"
   "  float f = (float)a;\n"
   "  unsigned r; __builtin_memcpy(&r, &f, 4); return (long)(unsigned)r;\n}\n",
   {(uint64_t)(int64_t)-1234567ULL}, "CvtScalar"},

  // Low-scalar value sanity (isolated instruction, no following op).
  {"cvtsi2sd_i32_low",
   "long f(long a){\n"
   "  unsigned long long out;\n"
   "  __asm__ volatile(\n"
   "    \"cvtsi2sd %1, %%xmm5\\n\\t\"\n"
   "    \"movq %%xmm5, %0\\n\\t\"\n"
   "    : \"=m\"(out) : \"r\"((int)a) : \"xmm5\");\n"
   "  return (long)out;\n}\n",
   {42ULL}, "CvtScalar"},

  {"cvtsi2ss_i32_low",
   "long f(long a){\n"
   "  unsigned out;\n"
   "  __asm__ volatile(\n"
   "    \"cvtsi2ss %1, %%xmm5\\n\\t\"\n"
   "    \"movd %%xmm5, %0\\n\\t\"\n"
   "    : \"=m\"(out) : \"r\"((int)a) : \"xmm5\");\n"
   "  return (long)(unsigned)out;\n}\n",
   {42ULL}, "CvtScalar"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(CvtScalar, X64CvtScalarUpperRT,
                         ::testing::ValuesIn(kCvt), rtTCName);
