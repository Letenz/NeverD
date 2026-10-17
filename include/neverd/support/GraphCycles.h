//===- GraphCycles.h - Strongly connected nodes of a graph ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Strongly connected components of a directed graph given as successor
/// lists over nodes 0..N-1, computed in one linear pass. A node can reach
/// another node of its component, and only those, along a path that comes
/// back to it; it lies on a cycle when its component has another node or it
/// has an edge to itself.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_GRAPHCYCLES_H
#define NEVERD_SUPPORT_GRAPHCYCLES_H

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace neverd {

/// The component number of each node of the graph whose successors are
/// \p Succs. Iterative Tarjan, so a deep graph does not exhaust the stack.
inline std::vector<size_t>
stronglyConnectedComponents(const std::vector<std::vector<size_t>> &Succs) {
  const size_t N = Succs.size();
  constexpr size_t Unvisited = static_cast<size_t>(-1);
  std::vector<size_t> Index(N, Unvisited), Low(N, 0), Component(N, Unvisited);
  std::vector<bool> OnStack(N, false);
  std::vector<size_t> Stack;
  // Each frame is a node and the position of its next successor.
  std::vector<std::pair<size_t, size_t>> Frames;
  size_t Next = 0, Components = 0;
  for (size_t Root = 0; Root < N; ++Root) {
    if (Index[Root] != Unvisited)
      continue;
    Index[Root] = Low[Root] = Next++;
    Stack.push_back(Root);
    OnStack[Root] = true;
    Frames.push_back({Root, 0});
    while (!Frames.empty()) {
      const size_t Node = Frames.back().first;
      size_t &Pos = Frames.back().second;
      if (Pos < Succs[Node].size()) {
        const size_t Succ = Succs[Node][Pos++];
        if (Index[Succ] == Unvisited) {
          Index[Succ] = Low[Succ] = Next++;
          Stack.push_back(Succ);
          OnStack[Succ] = true;
          Frames.push_back({Succ, 0});
        } else if (OnStack[Succ]) {
          Low[Node] = std::min(Low[Node], Index[Succ]);
        }
        continue;
      }
      Frames.pop_back();
      if (!Frames.empty())
        Low[Frames.back().first] =
            std::min(Low[Frames.back().first], Low[Node]);
      if (Low[Node] != Index[Node])
        continue;
      size_t Member;
      do {
        Member = Stack.back();
        Stack.pop_back();
        OnStack[Member] = false;
        Component[Member] = Components;
      } while (Member != Node);
      ++Components;
    }
  }
  return Component;
}

/// Whether each node of the graph whose successors are \p Succs lies on a
/// cycle.
inline std::vector<bool>
nodesOnCycles(const std::vector<std::vector<size_t>> &Succs) {
  const std::vector<size_t> Component = stronglyConnectedComponents(Succs);
  std::vector<size_t> Size(Succs.size(), 0);
  for (size_t C : Component)
    ++Size[C];
  std::vector<bool> OnCycle(Succs.size(), false);
  for (size_t Node = 0; Node < Succs.size(); ++Node)
    OnCycle[Node] = Size[Component[Node]] > 1 ||
                    std::find(Succs[Node].begin(), Succs[Node].end(), Node) !=
                        Succs[Node].end();
  return OnCycle;
}

} // namespace neverd

#endif // NEVERD_SUPPORT_GRAPHCYCLES_H
