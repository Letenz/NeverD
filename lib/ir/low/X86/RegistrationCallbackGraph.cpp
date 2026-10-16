//===- RegistrationCallbackGraph.cpp - Callback execution states --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Interns immutable frames and proves closure of their execution graph.

#include "RegistrationCallbackGraph.h"

#include <bit>

namespace neverd::registration_state {

std::optional<size_t>
RegistrationCallbackGraph::add(size_t Block, size_t FirstOp,
                               const FrameState &Frame,
                               std::optional<size_t> Parent, Charge Work) {
  const size_t FrameWork = Frame.cellCount() + Frame.OtherRegisterBytes.size() +
                           Frame.Registers.size();
  if (!Work(FrameWork + 2 * std::bit_width(Buckets.size() + 1) + 10))
    return std::nullopt;
  auto &Bucket = Buckets[{Block, FirstOp, Frame.snapshotHash()}];
  std::optional<size_t> Found;
  for (size_t Candidate : Bucket) {
    const auto &Previous = Nodes[Candidate].Frame;
    if (!Work(FrameWork + Previous.cellCount() +
              Previous.OtherRegisterBytes.size() + 10))
      return std::nullopt;
    // Hash equality is only a lookup hint, never frame identity evidence.
    if (Previous == Frame) {
      Found = Candidate;
      break;
    }
  }
  if (!Found) {
    Found = Nodes.size();
    Bucket.push_back(*Found);
    Nodes.push_back({Block, FirstOp, Frame});
    Edges.emplace_back();
    Pending.push_back(*Found);
  }
  if (Parent) {
    if (*Parent >= Edges.size() ||
        !Work(2 * std::bit_width(Edges[*Parent].size() + 1) + 1))
      return std::nullopt;
    Edges[*Parent].insert(*Found);
  }
  return Found;
}

size_t RegistrationCallbackGraph::take() {
  const size_t Next = Pending.front();
  Pending.pop_front();
  return Next;
}

bool RegistrationCallbackGraph::closes(Charge Work) const {
  if (!Pending.empty() || !Work(Nodes.size() + 1))
    return false;
  std::vector<size_t> Predecessors(Nodes.size());
  for (const auto &Children : Edges) {
    if (!Work(Children.size() + 1))
      return false;
    for (size_t Child : Children)
      ++Predecessors[Child];
  }
  std::vector<size_t> Ready;
  for (size_t I = 0; I != Predecessors.size(); ++I)
    if (!Predecessors[I])
      Ready.push_back(I);
  size_t Completed = 0;
  while (!Ready.empty()) {
    const size_t Current = Ready.back();
    Ready.pop_back();
    ++Completed;
    for (size_t Child : Edges[Current]) {
      if (!Work(1))
        return false;
      if (!--Predecessors[Child])
        Ready.push_back(Child);
    }
  }
  return Completed == Nodes.size();
}

} // namespace neverd::registration_state
