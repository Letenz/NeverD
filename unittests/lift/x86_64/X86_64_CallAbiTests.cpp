//===- X86_64_CallAbiTests.cpp - x86-64 call arguments in HighC -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// System V x86-64 passes the seventh and later integer arguments on the
// stack.  Each kernel is decompiled to HighC and run on the host against its
// own C.
//
//===----------------------------------------------------------------------===//

#include "HighCHostExecution.h"

class X86_64_CallAbi : public HighCHostExecutionTest {};

// Six register arguments and four pushed afresh on every iteration of a
// loop, whose stack pointer is a PHI at the loop head.
TEST_F(X86_64_CallAbi, StackArgumentsPushedInALoopReachTheCallee) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
static unsigned ma(unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
                   unsigned, unsigned, unsigned, unsigned)
    __attribute__((noinline));
int manyarg(int a) {
  unsigned s = (unsigned)a, acc = 0x811C9DC5u;
  for (int k = 0; k < 40; k++) {
    unsigned r = ma(s, s ^ acc, s + (unsigned)k, acc, s * 3u, acc >> 2,
                    s ^ 0x9E3779B9u, acc * 7u, s + acc, (unsigned)k * 131u);
    acc = (acc ^ r) * 16777619u;
    s = s * 1664525u + 1013904223u;
  }
  return (int)acc;
}
static unsigned ma(unsigned a, unsigned b, unsigned c, unsigned d, unsigned e,
                   unsigned f, unsigned g, unsigned h, unsigned i,
                   unsigned j) {
  return ((a * 131u + b) ^ (c << 3)) + (d - e) + (f ^ g) + (h * 5u) - i +
         (j >> 1);
}
)C",
                            "manyarg", {"manyarg", "ma"}, {-3, 0, 1, 77});
}
