//===- X86_64_FrameAliasTests.cpp - Frame addresses in HighC --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// A variable that holds one address in the frame prints as that address; one
// whose value changes does not.  Each kernel is decompiled to HighC and run on
// the host against its own C.
//
//===----------------------------------------------------------------------===//

#include "HighCHostExecution.h"

class X86_64_FrameAlias : public HighCHostExecutionTest {};

// The search for a sparse table's level steps a row pointer through the
// frame, and the address past it is taken in the same loop.  That address
// changes with the row, though the row starts at a fixed place, so the
// reads after the loop go through it rather than the first row past it.
TEST_F(X86_64_FrameAlias, AnAddressDerivedFromAnAdvancingOneIsNoFixedSlot) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
static const unsigned char rq[16] = {37, 12, 58, 4,  29, 61, 7,  44,
                                     18, 53, 2,  40, 25, 9,  49, 31};
int rmqsparse(int a) {
  unsigned s = (unsigned)a, out = 0;
  for (int it = 0; it < 128; it++) {
    s = s * 1103515245u + 12345u;
    unsigned acc = s;
    unsigned sp[5][16];
    for (int i = 0; i < 16; i++)
      sp[0][i] = ((unsigned)rq[i] ^ ((s >> (i & 7)) & 3u)) & 63u;
    for (int k = 1; k < 5; k++) {
      int len = 1 << k;
      for (int i = 0; i + len <= 16; i++) {
        unsigned x = sp[k - 1][i], y = sp[k - 1][i + (len >> 1)];
        sp[k][i] = x < y ? x : y;
      }
    }
    for (int q = 0; q < 8; q++) {
      unsigned l = (s >> (q & 7)) & 7u, w = ((s >> q) & 7u) + 1u,
               r = l + w - 1u;
      if (r > 15u)
        r = 15u;
      unsigned len = r - l + 1u;
      int k = 0;
      while ((1u << (k + 1)) <= len)
        k++;
      unsigned x = sp[k][l], y = sp[k][r - (1u << k) + 1u];
      unsigned mn = x < y ? x : y;
      acc = acc * 131u + mn + (unsigned)k + len;
    }
    out = out * 1311u + acc;
  }
  return (int)out;
}
)C",
                            "rmqsparse", {"rmqsparse", "rq"},
                            {0x2E, 0, 1, -1, 12345});
}
