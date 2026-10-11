//===- AArch64_HighCExecutionTests.cpp - AArch64 HighC run on the host ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Each kernel is decompiled to HighC and run on the host against its own C.
// The kernels keep to integer code, which the host compiles.
//
//===----------------------------------------------------------------------===//

#include "HighCHostExecution.h"

class AArch64_HighCExecution : public HighCHostExecutionTest {
protected:
  void expectRunsLikeSource(const std::string &Kernel, const std::string &Entry,
                            std::initializer_list<const char *> Names) {
    expectHighCRunsLikeSource({"-target", "aarch64-linux-gnu"}, Kernel, Entry,
                              Names, {0, 1, -1, 0x15, 0x27, 0x3F, 0x12345678});
  }
};

// A vector read at a run-time lane goes through the stack: `orr x8, x9, x8,
// lsl #2` over `mov x9, sp` selects the lane the load reads, so the `str q1,
// [sp]` before it is no write to an unread slot.
TEST_F(AArch64_HighCExecution,
       ALaneIndexedThroughTheFrameReadsTheStoredVector) {
  expectRunsLikeSource(R"C(
typedef int v4si __attribute__((vector_size(16)));
int lane(int a) {
  v4si v = {a, a + 1, a * 3, a ^ 5};
  return v[(a >> 4) & 3];
}
)C",
                       "lane", {"lane"});
}

// The writer's pointer arrives in x0, which it never writes, and its caller
// reads nothing it returns: it returns no value.
TEST_F(AArch64_HighCExecution, AWriterThroughItsPointerReturnsNoValue) {
  expectRunsLikeSource(R"C(
void upd4(long *s, int n) __attribute__((noinline));
int structptr(int x) {
  long s[4] = {x, x + 1, x + 2, x + 3};
  upd4(s, 12);
  return (int)(s[0] ^ s[1] ^ s[2] ^ s[3]);
}
void upd4(long *s, int n) {
  for (int i = 0; i < n; i++) {
    s[0] = s[0] * 3 + s[1];
    s[1] ^= s[0];
    s[2] += s[0];
    s[3] -= s[1];
  }
}
)C",
                       "structptr", {"structptr", "upd4"});
}

// FMAX and FMIN are IEEE 754-2019 maximum and minimum: a NaN operand gives a
// NaN, and -0 orders below +0, unlike fmaxf and fminf.
TEST_F(AArch64_HighCExecution, FMaxAndFMinPropagateANaN) {
  expectRunsLikeSource(R"C(
int extremes(int a) {
  float nan = __builtin_nanf("");
  float x = (a & 1) ? nan : (float)a;
  float y = (a & 2) ? -0.0f : (float)(a - 5);
  float z = (a & 4) ? 0.0f : (float)(7 - a);
  float m = __builtin_elementwise_maximum(x, y);
  float n = __builtin_elementwise_minimum(y, z);
  return (m != m) + 2 * __builtin_signbit(n) + 4 * (m == m ? (int)m : 0) +
         64 * (n == n ? (int)n : 0);
}
)C",
                       "extremes", {"extremes"});
}
