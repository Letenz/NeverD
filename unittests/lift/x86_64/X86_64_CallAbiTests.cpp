//===- X86_64_CallAbiTests.cpp - x86-64 call arguments in HighC -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// System V x86-64 passes the seventh and later integer arguments on the
// stack, and a callee can come from a stack slot.  Each kernel is decompiled
// to HighC and run on the host against its own C.
//
//===----------------------------------------------------------------------===//

#include "HighCHostExecution.h"

#include <fstream>
#include <iterator>
#include <string>

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

// `cmove` picks which of two stack slots the tail call loads its callee
// from, so the stores that fill both slots are read through that choice.
TEST_F(X86_64_CallAbi, ACalleePickedFromTwoStackSlotsIsTheStoredOne) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
static int pick_p(int) __attribute__((noinline));
static int pick_q(int) __attribute__((noinline));
typedef int (*pick_fn)(int);
int pick(int a) {
  volatile pick_fn s0 = pick_p, s1 = pick_q;
  pick_fn fp = (a & 1) ? s1 : s0;
  return fp(a * 3 + 1);
}
static int pick_p(int x) {
  unsigned h = (unsigned)x;
  for (unsigned i = 0; i < 6; i++)
    h = h * 131u + i;
  return (int)h;
}
static int pick_q(int x) {
  return (int)(((unsigned)x * 2654435761u) ^ ((unsigned)x >> 7));
}
)C",
                            "pick", {"pick", "pick_p", "pick_q"},
                            {-3, 0, 1, 2, 77});
}

// `long f(long a) { return (signed char)a; }` sign-extends the low byte of
// whatever register its caller passes.  Built apart from its callers, its C
// must do the same: a `signed char` parameter would let Clang take the
// caller's extension as done and return 0x1FE whole.
TEST_F(X86_64_CallAbi, AByteReadOfAParameterTakesTheWholeRegister) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "cross-target compilation requires Clang";
  const auto Source = tmpFile("sext8.c");
  const auto Object = tmpFile("sext8.o");
  std::ofstream(Source) << "long sext8(long a) { return (signed char)a; }\n";
  const auto Compiled =
      exec(NEVERD_TEST_CLANG, {"-target", "x86_64-linux-gnu", "-O2", "-c",
                               Source.string(), "-o", Object.string()});
  ASSERT_EQ(Compiled.exitCode, 0) << Compiled.err;
  const auto Decompiled = decompileToHighC(Object);
  ASSERT_EQ(Decompiled.exitCode, 0) << Decompiled.err;
  std::ifstream Input(tmpFile("decompiled_high.c"));
  const std::string C((std::istreambuf_iterator<char>(Input)),
                      std::istreambuf_iterator<char>());

  const auto HighC = tmpFile("sext8-highc.c");
  const auto HighCObject = tmpFile("sext8-highc.o");
  const auto Driver = tmpFile("sext8-driver.c");
  const auto Program = tmpFile("sext8.exe");
  std::ofstream(HighC) << C;
  std::ofstream(Driver) << "long sext8(long);\n"
                           "int main(void) {\n"
                           "  return sext8(0x1FE) == -2 && sext8(0x17F) == 127 "
                           "? 0 : 1;\n"
                           "}\n";
  const auto BuiltC = exec(NEVERD_TEST_CLANG, {"-O2", "-c", HighC.string(),
                                               "-o", HighCObject.string()});
  ASSERT_EQ(BuiltC.exitCode, 0) << BuiltC.err << "\n" << C;
  const auto Linked =
      exec(NEVERD_TEST_CLANG, {"-O2", Driver.string(), HighCObject.string(),
                               "-o", Program.string()});
  ASSERT_EQ(Linked.exitCode, 0) << Linked.err;
  EXPECT_EQ(exec(Program.string(), {}).exitCode, 0) << C;
}

// Ten doubles: eight in xmm0-xmm7 and two on the stack.  The callee's lift
// also lists the six integer argument registers, which it never reads; its
// definition leaves them out, and so must its prototype and its calls.
TEST_F(X86_64_CallAbi, AStackDoubleCalleeIsDeclaredAsItIsDefined) {
  expectHighCRunsLikeSource(
      {"-target", "x86_64-linux-gnu", "-ffp-contract=off"},
      R"C(
static double d10(double, double, double, double, double, double, double,
                  double, double, double) __attribute__((noinline));
int sumd10(int a) {
  double b = (double)(a & 0xffff);
  return (int)d10(b, b + 1, b + 2, b + 3, b + 4, b + 5, b + 6, b + 7, b + 8,
                  b + 9);
}
static double d10(double a, double b, double c, double d, double e, double f,
                  double g, double h, double i, double j) {
  return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 + i * 9 +
         j * 10;
}
)C",
      "sumd10", {"sumd10", "d10"}, {0, 1, 0x41, 0x1234});
}
