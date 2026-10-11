//===- X86_64_ImageTableTests.cpp - Image tables in HighC -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// A table in the image prints as the object the output defines, and an index
// into it keeps the value the machine computed.  Each kernel is decompiled to
// HighC and run on the host against its own C.
//
//===----------------------------------------------------------------------===//

#include "HighCHostExecution.h"

class X86_64_ImageTable : public HighCHostExecutionTest {
protected:
  void expectRunsLikeSource(const std::string &Kernel, const std::string &Entry,
                            std::initializer_list<const char *> Names) {
    expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, Kernel, Entry,
                              Names, {-3, 0, 1, 77});
  }
};

// A call through a table of functions reads the table the output defines:
// the table's address added to an index is no offset into an object.
TEST_F(X86_64_ImageTable, ACallThroughAFunctionTableReadsTheTable) {
  expectRunsLikeSource(R"C(
typedef unsigned (*vf_t)(unsigned);
static unsigned h0(unsigned x) { return (x * 2654435761u) ^ (x >> 15); }
static unsigned h1(unsigned x) { return x + 0x85EBCA6Bu; }
static unsigned h2(unsigned x) { return (x << 13) | (x >> 19); }
struct obj { vf_t f; unsigned k; };
struct obj table[3] = {{h0, 3u}, {h1, 5u}, {h2, 7u}};
int dispatch(int a) {
  unsigned acc = (unsigned)a;
  for (int i = 0; i < 66; i++) {
    const struct obj *o = &table[(acc >> 2) % 3u];
    acc = o->f(acc) + o->k * (unsigned)i;
  }
  return (int)acc;
}
)C",
                       "dispatch", {"dispatch", "table", "h0", "h1", "h2"});
}
