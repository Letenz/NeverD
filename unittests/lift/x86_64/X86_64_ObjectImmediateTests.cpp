//===- X86_64_ObjectImmediateTests.cpp - Object immediates in HighC -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// An x86-64 object addresses its own sections only through relocations, so
// an immediate no relocation writes is a number even where an object of the
// image starts at that value, and an object whose bytes begin like a short
// string is still the whole object.  Each kernel is decompiled to HighC and
// run on the host against its own C.
//
//===----------------------------------------------------------------------===//

#include "HighCHostExecution.h"

class X86_64_ObjectImmediate : public HighCHostExecutionTest {};

// The trip count 128 equals the address the image gives the first node: it
// stays the count.
TEST_F(X86_64_ObjectImmediate, ALoopCountIsNoAddress) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
struct node {
  int v;
  const struct node *next;
};
static const struct node n4 = {0x5eed1234, 0};
static const struct node n3 = {40, &n4};
static const struct node n2 = {30, &n3};
static const struct node n1 = {20, &n2};
static const struct node n0 = {10, &n1};
int gchain(int a) {
  unsigned s = (unsigned)a | 1u;
  long acc = 0;
  for (int i = 0; i < 128; i++) {
    s = s * 1103515245u + 12345u;
    const struct node *q = &n0;
    int steps = (int)((s >> 4) & 7u);
    while (steps-- > 0 && q->next)
      q = q->next;
    acc += q->v;
    acc ^= acc >> 5;
  }
  return (int)acc;
}
)C",
                            "gchain",
                            {"gchain", "node", "n0", "n1", "n2", "n3", "n4"},
                            {-3, 0, 1, 77});
}

// The last node's bytes, 50 and a null pointer, also read as a
// one-character wide string, which is shorter than the node: the node stays
// a node whose null link ends the walk.
TEST_F(X86_64_ObjectImmediate, ANodeIsNoShorterString) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
struct node {
  int v;
  const struct node *next;
};
static const struct node n4 = {50, 0};
static const struct node n3 = {40, &n4};
static const struct node n2 = {30, &n3};
static const struct node n1 = {20, &n2};
static const struct node n0 = {10, &n1};
int gchain(int a) {
  unsigned s = (unsigned)a | 1u;
  long acc = 0;
  for (int i = 0; i < 128; i++) {
    s = s * 1103515245u + 12345u;
    const struct node *q = &n0;
    int steps = (int)((s >> 4) & 7u);
    while (steps-- > 0 && q->next)
      q = q->next;
    acc += q->v;
    acc ^= acc >> 5;
  }
  return (int)acc;
}
)C",
                            "gchain",
                            {"gchain", "node", "n0", "n1", "n2", "n3", "n4"},
                            {-3, 0, 1, 77});
}
