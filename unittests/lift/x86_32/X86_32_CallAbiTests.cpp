//===- X86_32_CallAbiTests.cpp - i386 call arguments in HighC -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Clang gives a directly called static i386 function a register convention
// (fastcc: ECX and EDX, then the stack), and a wrapper that only passes its
// arguments on reads them nowhere else.  Each kernel is built as the
// semantic grid builds i386 code, decompiled to HighC, and run on the host
// against its own C.
//
//===----------------------------------------------------------------------===//

#include "HighCHostExecution.h"

class X86_32_CallAbi : public HighCHostExecutionTest {
protected:
  void expectRunsLikeSource(const std::string &Kernel, const std::string &Entry,
                            std::initializer_list<const char *> Names) {
    expectHighCRunsLikeSource({"-target", "i386-linux-gnu", "-march=pentium4"},
                              Kernel, Entry, Names, {-3, 0, 1, 77});
  }
};

// A wrapper that only forwards ECX and EDX takes them as its parameters,
// and its call passes them on.
TEST_F(X86_32_CallAbi, ForwarderPassesItsRegisterArgumentsThrough) {
  expectRunsLikeSource(R"C(
static unsigned g2(unsigned, unsigned) __attribute__((noinline));
static unsigned f2(unsigned, unsigned) __attribute__((noinline));
int fwd2(int a) {
  unsigned b = (unsigned)a;
  return (int)f2(b, b * 2654435761u + 1u);
}
static unsigned f2(unsigned x, unsigned y) { return g2(x, y); }
static unsigned g2(unsigned x, unsigned y) {
  unsigned h = x ^ 0x9e3779b9u;
  for (unsigned i = 0; i < 8; i++)
    h = h * 31u + ((y >> (i & 7)) & 0xffu) + i;
  return h;
}
)C",
                       "fwd2", {"fwd2", "f2", "g2"});
}

// Two register arguments and eight on the stack, pushed afresh on every
// iteration of a loop, whose stack pointer is a PHI at the loop head.
TEST_F(X86_32_CallAbi, StackArgumentsPushedInALoopReachTheCallee) {
  expectRunsLikeSource(R"C(
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
                       "manyarg", {"manyarg", "ma"});
}
