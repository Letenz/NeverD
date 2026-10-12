//===- HighCDemandedBytesTests.cpp - Unknown bytes no result reads --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// A value built from known bytes and the bytes of a register the function
// never set traps in C only where a result depends on those bytes.  Each
// kernel is decompiled to HighC and run on the host against its own C.
//
//===----------------------------------------------------------------------===//

#include "HighCHostExecution.h"

class HighCDemandedBytes : public HighCHostExecutionTest {};

// CVTSI2SD writes the low double of an XMM register and keeps the upper half,
// which this function never set.  Only the low double reaches the result, so
// the C computes it with no unknown value.
TEST_F(HighCDemandedBytes, AConversionKeepsNoUnreadUpperHalf) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu", "-msse2"}, R"C(
int chain(int a) {
  long b = (long)a * 7 + 3;
  double da = (double)(long)a, db = (double)b;
  double r = da * db - da;
  long ri;
  __builtin_memcpy(&ri, &r, 8);
  return (int)(ri ^ (ri >> 32));
}
)C",
                            "chain", {"chain"}, {0, 1, -3, 77, 1000, -12345});
}

// Sign extending a word in place, `punpcklwd %xmm1, %xmm2; psrad $16` takes
// the low half of each lane from XMM2, which the function never set, and
// shifts it out again.  The register is no parameter, and the C, whose
// shift count is a variable holding 16, reads none of its bytes.
TEST_F(HighCDemandedBytes, AShiftDropsTheHalfAnUnsetRegisterGave) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu", "-msse2"}, R"C(
int sext_hi(int a) {
  unsigned r;
  __asm__("movd %1, %%xmm1\n\t"
          "punpcklwd %%xmm1, %%xmm2\n\t"
          "psrad $16, %%xmm2\n\t"
          "movd %%xmm2, %0"
          : "=r"(r)
          : "r"(a)
          : "xmm1", "xmm2");
  return (int)r;
}
)C",
                            "sext_hi", {"sext_hi"},
                            {0, 1, -3, 0x7FFF, 0x8000, -12345});
}
