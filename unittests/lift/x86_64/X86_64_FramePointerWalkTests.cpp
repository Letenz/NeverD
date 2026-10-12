//===- X86_64_FramePointerWalkTests.cpp - Pointers into the frame ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// A variable that first holds a frame address names that slot only while no
// other value replaces it.  Each kernel is decompiled to HighC and run on the
// host against its own C.
//
//===----------------------------------------------------------------------===//

#include "HighCHostExecution.h"

class X86_64_FramePointerWalk : public HighCHostExecutionTest {};

// `lo` starts at the buffer and the loop moves it on: each read through it
// reads the element it points at now, not the buffer's first.
TEST_F(X86_64_FramePointerWalk, AnAdvancedPointerIsNoFrameAddress) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
int ptrwalk(int a) {
  unsigned buf[40];
  for (int i = 0; i < 40; i++)
    buf[i] = (unsigned)a + (unsigned)i * 7u;
  unsigned *lo = buf, *hi = buf + 39, h = 0;
  for (int n = 0; n < 40 && lo < hi; n++) {
    long d = hi - lo;
    h += (unsigned)d * 131u + *lo + *hi;
    if (*lo < *hi)
      lo++;
    else
      hi--;
    h ^= (unsigned)(hi - lo);
  }
  return (int)h;
}
)C",
                            "ptrwalk", {"ptrwalk"}, {9, -3, 0, 1, 77});
}
