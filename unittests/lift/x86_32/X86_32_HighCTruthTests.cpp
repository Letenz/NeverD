//===- X86_32_HighCTruthTests.cpp - Truth tests in HighC ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// A truth test reads the bytes the code tests, which may be fewer than the
// variable holding them.  Each kernel is decompiled to HighC and run on the
// host against its own C.
//
//===----------------------------------------------------------------------===//

#include "HighCHostExecution.h"

class X86_32_HighCTruth : public HighCHostExecutionTest {};

// `sete %ah; setne %al` writes two bytes of EAX and keeps its upper bytes,
// and `test %al, %al` tests only AL: the test is of the low byte, whatever
// the bytes above it hold.
TEST_F(X86_32_HighCTruth, ATestOfTheLowByteIgnoresTheBytesAbove) {
  HostFlags = {"-m32", "-msse2"};
  expectHighCRunsLikeSource({"-target", "i386-linux-gnu", "-march=pentium4"},
                            R"C(
static const unsigned char nq[4] = {2, 5, 1, 4};
int nqueens(int a) {
  unsigned s = (unsigned)a, out = 0;
  for (int it = 0; it < 128; it++) {
    s = s * 1103515245u + 12345u;
    unsigned acc = s;
    int N = 6;
    int br = (int)(((unsigned)nq[s & 3u] + (s >> 3)) % 6u);
    int bc = (int)((s >> 5) % 6u);
    int pos[6];
    for (int i = 0; i < 6; i++)
      pos[i] = -1;
    unsigned solutions = 0u;
    int r = 0;
    while (r >= 0) {
      int c = pos[r] + 1;
      int placed = 0;
      while (c < N) {
        int ok = 1;
        if (r == br && c == bc)
          ok = 0;
        for (int k = 0; ok && k < r; k++) {
          int ck = pos[k];
          if (ck == c || (r - k) == (c - ck) || (r - k) == (ck - c))
            ok = 0;
        }
        if (ok) {
          pos[r] = c;
          placed = 1;
          break;
        }
        c++;
      }
      if (placed) {
        if (r == N - 1)
          solutions++;
        else {
          r++;
          pos[r] = -1;
        }
      } else {
        pos[r] = -1;
        r--;
      }
    }
    acc = acc * 131u + solutions * 131u + (unsigned)(br * 6 + bc);
    out = out * 1311u + acc;
  }
  return (int)out;
}
)C",
                            "nqueens", {"nqueens", "nq"}, {0x90, 0, 1, -3, 77});
}
