//===- ARM32_ObjectImmediateTests.cpp - Object immediates in HighC --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// An ARM object addresses its own sections only through relocations, of a
// literal word or a MOVW/MOVT pair, so an immediate an instruction no
// relocation writes moves is a number even where an object of the image
// starts at that value.  Each kernel is decompiled to HighC and run on the
// host against its own C.
//
//===----------------------------------------------------------------------===//

#include "HighCHostExecution.h"

class ARM32_ObjectImmediate : public HighCHostExecutionTest {
protected:
  ARM32_ObjectImmediate() { HostFlags = {"-m32"}; }
};

// The trip count 200 equals the address the image gives the last row: it
// stays the count.
TEST_F(ARM32_ObjectImmediate, ALoopCountIsNoAddress) {
  expectHighCRunsLikeSource(
      {"-target", "armv7-linux-gnueabi"}, R"C(
static const int r0[5] = {2, 3, 5, 7, 11};
static const int r1[5] = {13, 17, 19, 23, 29};
static const int r2[5] = {31, 37, 41, 43, 47};
static const int r3[5] = {53, 59, 61, 67, 71};
static const int *const rows[4] = {r0, r1, r2, r3};
int arrind(int a) {
  unsigned s = (unsigned)a, h = 0;
  for (int i = 0; i < 200; i++) {
    s = s * 1103515245u + 12345u;
    const int *p = rows[(s >> 6) & 3u];
    unsigned acc = 0;
    for (int j = 0; j < 5; j++)
      acc = acc * 7u + (unsigned)*p++;
    h = h * 131u + acc;
    h ^= h >> 9;
  }
  return (int)h;
}
)C",
      "arrind", {"arrind", "r0", "r1", "r2", "r3", "rows"}, {-3, 0, 1, 77});
}
