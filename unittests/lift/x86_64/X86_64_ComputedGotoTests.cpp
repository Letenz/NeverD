//===- X86_64_ComputedGotoTests.cpp - -O0 computed gotos in HighC ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// At -O0 every `goto *table[i]` spills the target it loads to one frame slot
// and jumps to a shared block that reloads it, so each goto site selects its
// own case.  Each kernel is decompiled to HighC and run on the host against
// its own C.
//
//===----------------------------------------------------------------------===//

#include "HighCHostExecution.h"

class X86_64_ComputedGoto : public HighCHostExecutionTest {
protected:
  X86_64_ComputedGoto() { KernelOptimization = "-O0"; }
  void expectRunsLikeSource(const std::string &Kernel, const std::string &Entry,
                            std::initializer_list<const char *> Names) {
    expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, Kernel, Entry,
                              Names, {-3, 0, 1, 77});
  }
};

// Each label picks the next one its own way: a constant entry, or the
// accumulator under a different shift.  The dispatch switches on the selector
// the goto site that reached it chose.
TEST_F(X86_64_ComputedGoto, EachGotoSiteSelectsItsOwnCase) {
  expectRunsLikeSource(R"C(
int kconst(int a) {
  static const void *tab[4] = {&&K0, &&K1, &&K2, &&K3};
  unsigned acc = (unsigned)a;
  int n = 0;
  goto *tab[acc & 3];
K0:
  acc += 0x9E3779B9u;
  if (++n >= 40)
    goto fin;
  goto *tab[2];
K1:
  acc ^= acc >> 13;
  if (++n >= 40)
    goto fin;
  goto *tab[(acc >> 4) & 3];
K2:
  acc = acc * 2654435761u + 1u;
  if (++n >= 40)
    goto fin;
  goto *tab[0];
K3:
  acc -= acc >> 5;
  if (++n >= 40)
    goto fin;
  goto *tab[(acc >> 2) & 3];
fin:
  return (int)(acc ^ (unsigned)n);
}
)C",
                       "kconst", {"kconst"});
}
