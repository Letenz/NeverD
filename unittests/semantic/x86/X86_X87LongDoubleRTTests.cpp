//===- X86_X87LongDoubleRTTests.cpp - 80-bit long double roundtrip -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// `long double` on x86/x86-64 is the 80-bit x87 extended-precision type: clang
// emits `fldt`/`fstpt` (m80 load/store) and computes on the x87 stack at 64-bit
// mantissa precision.  Earlier x87 tests dodged this by using values that are
// exact in 64-bit (10.0/5.0 = 2.0), so a double-precision model happened to
// match.  These kernels accumulate non-representable constants over many
// iterations and return the low mantissa bytes, where a 64-bit vs 80-bit model
// diverges in the last bits -- exercising the x87 register as genuine f80.
//
// The result is read back from the low 8 bytes (the 64-bit mantissa) of the
// stored long double, never the undefined padding bytes, so original and lifted
// are compared on well-defined state only.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64X87LongDoubleRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64X87LongDoubleRT, Verify) { roundTripX64(GetParam()); }
class X86X87LongDoubleRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86X87LongDoubleRT, Verify) { roundTripX86(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeLongDoubleTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Multiply-and-add accumulation with constants that are not exact in any
    // binary float -- 80-bit and 64-bit mantissas diverge after a few rounds.
    {p+"_acc",
     t+" "+p+"_acc("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u;\n"
     "  long double x=(long double)u, s=0;\n"
     "  for(int i=0;i<50;i++){ x=x*1.0000001L+3.14159265358979L; s=s+x/7.0L; }\n"
     "  unsigned long long m; __builtin_memcpy(&m,&s,8);\n"
     "  return ("+t+")(unsigned)(m ^ (m>>32)); }\n",
     {0x57ULL}, "X87LongDouble", 2},

    // Division-heavy chain: 80-bit division rounds differently from 64-bit.
    {p+"_divchain",
     t+" "+p+"_divchain("+t+" a){\n"
     "  unsigned u=(unsigned)a|3u;\n"
     "  long double x=(long double)u;\n"
     "  for(int i=0;i<48;i++){ x = (x+1.0L)/1.3000000001L; x = x*2.7L - 0.333333333333L; }\n"
     "  unsigned long long m; __builtin_memcpy(&m,&x,8);\n"
     "  return ("+t+")(unsigned)(m ^ (m>>32)); }\n",
     {0xa3ULL}, "X87LongDouble", 2},

    // sqrt chain via the hardware x87 FSQRT (inline asm forces the instruction
    // instead of a `sqrtl` libcall): the 80-bit FSQRT result diverges from a
    // 64-bit one.  x is round-tripped through memory each step so the kernel
    // exercises f80 precision, not deep x87-stack register allocation.
    {p+"_sqrtchain",
     t+" "+p+"_sqrtchain("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u;\n"
     "  long double x=(long double)u + 0.5L; unsigned h=0;\n"
     "  for(int i=0;i<40;i++){ long double v=x*1.0000003L+2.0L;\n"
     "    __asm__(\"fsqrt\":\"+t\"(v)); x=v+0.1L;\n"
     "    unsigned long long m; __builtin_memcpy(&m,&x,8); h=h*131u+(unsigned)m; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x39ULL}, "X87LongDouble", 2},

    // Mixed-width: load a double (fldl, widened to f80) and an m80 constant,
    // combine, store the m80 result -- exercises both the widen and the native
    // 80-bit store path in one kernel.
    {p+"_mixwidth",
     t+" "+p+"_mixwidth("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u;\n"
     "  double d=(double)u * 1.1;\n"
     "  long double x=(long double)d, s=0;\n"
     "  for(int i=0;i<48;i++){ x=x*1.0000001L + (long double)d*0.5L; s=s+x - (long double)i*0.7L; }\n"
     "  unsigned long long m; __builtin_memcpy(&m,&s,8);\n"
     "  return ("+t+")(unsigned)(m ^ (m>>32)); }\n",
     {0x6eULL}, "X87LongDouble", 2},

    // Pure 80-bit polynomial (Horner) with non-exact coefficients.
    {p+"_horner",
     t+" "+p+"_horner("+t+" a){\n"
     "  long double x=(long double)((unsigned)a|1u) * 0.0001L;\n"
     "  long double r=0;\n"
     "  long double c[5]={1.1L,2.2000001L,3.3L,4.4444L,5.5L};\n"
     "  for(int k=0;k<200;k++){ r=0; for(int i=0;i<5;i++) r=r*x+c[i]; x=x+0.00001L; }\n"
     "  unsigned long long m; __builtin_memcpy(&m,&r,8);\n"
     "  return ("+t+")(unsigned)(m ^ (m>>32)); }\n",
     {0x21ULL}, "X87LongDouble", 2},

    // Comparison/select driven by 80-bit values: the branch a 64-bit model
    // takes can differ at the boundary where the two precisions disagree.
    {p+"_cmpsel",
     t+" "+p+"_cmpsel("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u; long double x=(long double)u, acc=0;\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i<60;i++){ x=x*1.0000007L+0.123456789L;\n"
     "    long double y=x/3.0L;\n"
     "    if(y> acc) { acc=y; h=h*131u+1u; } else { acc=acc*0.999L; h=h*131u+7u; } }\n"
     "  unsigned long long m; __builtin_memcpy(&m,&acc,8);\n"
     "  return ("+t+")(unsigned)(h ^ (unsigned)m ^ (unsigned)(m>>32)); }\n",
     {0x84ULL}, "X87LongDouble", 2},

    // A long double passed by value fills three i386 stack slots or two
    // x86-64 ones, and its sign and exponent sit in the last, partly used
    // slot. The callee must read all ten bytes and the caller store them.
    {p+"_param",
     "static long double "+p+"_hp(long double) __attribute__((noinline));\n"
     +t+" "+p+"_param("+t+" a){\n"
     "  long double x=(long double)((unsigned)a|1u) * -1.0000001L;\n"
     "  long double r="+p+"_hp(x);\n"
     "  unsigned long long m; unsigned short e;\n"
     "  __builtin_memcpy(&m,&r,8); __builtin_memcpy(&e,(char *)&r+8,2);\n"
     "  return ("+t+")(unsigned)(m ^ (m>>32) ^ e); }\n"
     "static long double "+p+"_hp(long double x){ return -x * 0.75L + x / 3.0L; }\n",
     {0x5bULL}, "X87LongDouble", 2},

    // An integer before the long double (i386: slot 0, then slots 1-3;
    // x86-64: EDI, then the stack), with an integer load and FABS on it.
    {p+"_mixparam",
     "static long double "+p+"_hm(int,long double) __attribute__((noinline));\n"
     +t+" "+p+"_mixparam("+t+" a){\n"
     "  long double x=(long double)((unsigned)a|1u) * -2.5000001L;\n"
     "  long double r="+p+"_hm((int)a - 7, x);\n"
     "  unsigned long long m; unsigned short e;\n"
     "  __builtin_memcpy(&m,&r,8); __builtin_memcpy(&e,(char *)&r+8,2);\n"
     "  return ("+t+")(unsigned)(m ^ (m>>32) ^ e); }\n"
     "static long double "+p+"_hm(int k,long double b){\n"
     "  return (long double)k * b - __builtin_fabsl(b); }\n",
     {0x3cULL}, "X87LongDouble", 2},

    // A callee passing a long double on: its only store before the call is
    // the ten-byte `fstp tbyte` of the argument.
    {p+"_fwdparam",
     "static long double "+p+"_hf(long double) __attribute__((noinline));\n"
     "static long double "+p+"_hg(long double) __attribute__((noinline));\n"
     +t+" "+p+"_fwdparam("+t+" a){\n"
     "  long double x=(long double)((unsigned)a|1u) * 1.0000001L;\n"
     "  long double r="+p+"_hg(x);\n"
     "  unsigned long long m; unsigned short e;\n"
     "  __builtin_memcpy(&m,&r,8); __builtin_memcpy(&e,(char *)&r+8,2);\n"
     "  return ("+t+")(unsigned)(m ^ (m>>32) ^ e); }\n"
     "static long double "+p+"_hg(long double x){ return "+p+"_hf(x + x) + 1.0L; }\n"
     "static long double "+p+"_hf(long double x){ return x / 3.0L; }\n",
     {0x47ULL}, "X87LongDouble", 2},

    // Two long doubles: the second starts where the first one's padding ends.
    {p+"_twoparam",
     "static long double "+p+"_h2(long double,long double)"
     " __attribute__((noinline));\n"
     +t+" "+p+"_twoparam("+t+" a){\n"
     "  long double x=(long double)((unsigned)a|1u) * 1.0000001L;\n"
     "  long double r="+p+"_h2(x, -x / 7.0L);\n"
     "  unsigned long long m; unsigned short e;\n"
     "  __builtin_memcpy(&m,&r,8); __builtin_memcpy(&e,(char *)&r+8,2);\n"
     "  return ("+t+")(unsigned)(m ^ (m>>32) ^ e); }\n"
     "static long double "+p+"_h2(long double a,long double b){\n"
     "  return a * b + a - b; }\n",
     {0x2dULL}, "X87LongDouble", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64LD =
    makeLongDoubleTC("x64ld", "long");
static const std::vector<RoundTripTC> kX86LD = makeLongDoubleTC("x86ld", "int");

INSTANTIATE_TEST_SUITE_P(X87LongDouble, X64X87LongDoubleRT,
                         ::testing::ValuesIn(kX64LD), rtTCName);
INSTANTIATE_TEST_SUITE_P(X87LongDouble, X86X87LongDoubleRT,
                         ::testing::ValuesIn(kX86LD), rtTCName);
