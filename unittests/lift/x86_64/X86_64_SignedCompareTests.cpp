//===- X86_64_SignedCompareTests.cpp - Signed comparisons in HighC --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// A comparison reads its operands as C types the text it prints, not as the
// IR types them.  Each kernel is decompiled to HighC and run on the host
// against its own C.
//
//===----------------------------------------------------------------------===//

#include "HighCHostExecution.h"

class X86_64_SignedCompare : public HighCHostExecutionTest {
protected:
  void expectRunsLikeSource(const std::string &Kernel, const std::string &Entry,
                            std::initializer_list<const char *> Names) {
    expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, Kernel, Entry,
                              Names, {-3, 0, 1, 77});
  }
};

// `(int)(s >> 2) - 0x40000000` prints as an OR with 0xC0000000, which C
// types as an unsigned int, so the signed comparison with the running
// minimum converts it back.
TEST_F(X86_64_SignedCompare, ArithmeticWithAnUnsignedLiteralComparesSigned) {
  expectRunsLikeSource(R"C(
int minmax(int a) {
  int mn = 2000000000, mx = -2000000000, mi = 0, acc = 0;
  unsigned s = (unsigned)a | 1u;
  for (int j = 0; j < 200; j++) {
    int v = (int)(s >> 2) - 0x40000000;
    if (v < mn)
      mn = v;
    if (v > mx) {
      mx = v;
      mi = j;
    }
    acc = acc * 131 + (mn >> 8) + (mx >> 8) + mi;
    s = s * 1103515245u + 12345u;
  }
  return acc ^ mn ^ mx ^ mi;
}
)C",
                       "minmax", {"minmax"});
}
