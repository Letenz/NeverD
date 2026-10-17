//===- GraphCyclesTests.cpp - Strongly connected nodes of a graph ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/support/GraphCycles.h"

#include <cstddef>
#include <vector>

using namespace neverd;

namespace {

TEST(GraphCycles, GroupsNodesThatReachEachOther) {
  // 0 -> 1 -> 2 -> 1, 2 -> 3, 4 -> 4, 5 alone.
  const std::vector<std::vector<size_t>> Succs = {{1}, {2}, {1, 3},
                                                  {},  {4}, {}};
  const std::vector<size_t> Component = stronglyConnectedComponents(Succs);
  ASSERT_EQ(Component.size(), Succs.size());
  EXPECT_EQ(Component[1], Component[2]);
  EXPECT_NE(Component[0], Component[1]);
  EXPECT_NE(Component[3], Component[1]);
  EXPECT_NE(Component[4], Component[5]);
  EXPECT_EQ(nodesOnCycles(Succs),
            (std::vector<bool>{false, true, true, false, true, false}));
}

TEST(GraphCycles, HandlesAChainDeeperThanTheCallStack) {
  // A million-node path closed into one cycle, then left open.
  const size_t N = 1000000;
  std::vector<std::vector<size_t>> Succs(N);
  for (size_t I = 0; I + 1 < N; ++I)
    Succs[I] = {I + 1};
  Succs[N - 1] = {0};
  const std::vector<bool> Closed = nodesOnCycles(Succs);
  EXPECT_TRUE(Closed.front());
  EXPECT_TRUE(Closed.back());
  Succs[N - 1].clear();
  const std::vector<bool> Open = nodesOnCycles(Succs);
  EXPECT_FALSE(Open.front());
  EXPECT_FALSE(Open.back());
}

} // namespace
