//===- X86_64_DynamicStackTests.cpp - Run-time stack in HighC -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// A VLA lowers to `sp - size` with a runtime size, whose result becomes the
// stack pointer; the C allocates that storage itself.  Each kernel is
// decompiled to HighC and run on the host against its own C.
//
//===----------------------------------------------------------------------===//

#include "HighCHostExecution.h"

class X86_64_DynamicStack : public HighCHostExecutionTest {};

// Two VLAs, the second in a branch, each sized at run time.  As raw stack
// arithmetic their elements would lie below the frame the C declares, in no
// object; the C reads and writes storage it allocated.
TEST_F(X86_64_DynamicStack, VariableLengthArraysAreStorageTheCAllocates) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
int dyncond(int a) {
  unsigned n = ((unsigned)a & 7u) + 2u;
  unsigned x[n];
  unsigned h = 0;
  for (unsigned i = 0; i < n; i++)
    x[i] = i * (unsigned)a + 1u;
  if ((unsigned)a & 8u) {
    unsigned m = (((unsigned)a >> 4) & 7u) + 2u;
    unsigned y[m];
    for (unsigned i = 0; i < m; i++)
      y[i] = (i ^ (unsigned)a) * 2654435761u;
    for (unsigned i = 0; i < m; i++)
      h = h * 131u + y[i];
  }
  for (unsigned i = 0; i < n; i++)
    h = h * 31u + x[i];
  return (int)h;
}
)C",
                            "dyncond", {"dyncond"},
                            {0x9C, 0, 1, -3, 77, 0x18, 0xF8});
}
