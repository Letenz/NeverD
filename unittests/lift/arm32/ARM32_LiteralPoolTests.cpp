//===- ARM32_LiteralPoolTests.cpp - Literal pool words in HighC -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// An A32 literal pool keeps constants and relocated addresses side by side
// in the code section.  A constant word no relocation writes is a constant,
// however close a relocated word sits.  Each kernel is decompiled to HighC
// and run on the host against its own C.
//
//===----------------------------------------------------------------------===//

#include "HighCHostExecution.h"

class ARM32_LiteralPool : public HighCHostExecutionTest {
protected:
  ARM32_LiteralPool() {
    HostFlags = {"-m32"};
    KernelOptimization = "-O0";
  }
};

// -O0 code loads both the multiplier and g's address from the pool, the
// constant right after the word R_ARM_REL32 fills in.
TEST_F(ARM32_LiteralPool, AConstantBesideARelocatedWordIsAConstant) {
  expectHighCRunsLikeSource({"-target", "armv7-linux-gnueabi", "-marm"}, R"C(
unsigned g = 5;
int lit(int a) { return (int)((unsigned)a * 2654435761u + g); }
)C",
                            "lit", {"lit", "g"}, {-3, 0, 1, 77});
}
