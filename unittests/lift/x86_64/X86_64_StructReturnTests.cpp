//===- X86_64_StructReturnTests.cpp - Multi-register returns in HighC -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// System V x86-64 returns a small struct in up to two registers per class.
// The callee returns the record of every field, and the caller reads each
// one.  Each kernel is decompiled to HighC and run on the host against its
// own C.
//
//===----------------------------------------------------------------------===//

#include "HighCHostExecution.h"

class X86_64_StructReturn : public HighCHostExecutionTest {};

// Two doubles return in xmm0 and xmm1; the second is no zero.
TEST_F(X86_64_StructReturn, TwoDoublesReturnBoth) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
typedef struct { double a, b; } dd;
dd mkdd(int x) __attribute__((noinline));
int retdd(int x) {
  dd r = mkdd(x);
  return (int)r.a ^ ((int)r.b * 9);
}
dd mkdd(int x) {
  dd r;
  r.a = (double)(x >> 2) + 0.25;
  r.b = (double)(x >> 4) - 1.75;
  return r;
}
)C",
                            "retdd", {"retdd", "mkdd", "dd"}, {-3, 0, 1, 77});
}
