//===- SourceVariableFlow.cpp - Bounded source-local lifetime proofs ------===//
#include "neverd/ir/SourceVariableFlow.h"

#include "neverd/ir/Limits.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <numeric>

using namespace neverd;

namespace {
bool contains(const std::vector<unsigned> &Values, unsigned Value) {
  return std::find(Values.begin(), Values.end(), Value) != Values.end();
}
struct Graph {
  const SourceVariableFlow &Flow;
  std::vector<std::vector<size_t>> Predecessors;
  std::vector<bool> Reachable;
  std::string Error;
  explicit Graph(const SourceVariableFlow &Flow) : Flow(Flow) {
    if (!Flow.Refusal.empty()) {
      Error = Flow.Refusal;
      return;
    }
    if (Flow.Nodes.empty() ||
        Flow.Nodes.size() > ::neverd::limits::ir::kSourceVariableFlowMaxNodes ||
        Flow.Entry >= Flow.Nodes.size()) {
      Error = "The source control-flow graph is unavailable or exceeds the "
              "edit budget";
      return;
    }
    Predecessors.resize(Flow.Nodes.size());
    Reachable.resize(Flow.Nodes.size());
    size_t Edges = 0;
    for (size_t N = 0; N < Flow.Nodes.size(); ++N) {
      for (size_t Next : Flow.Nodes[N].Successors) {
        if (Next >= Flow.Nodes.size() ||
            ++Edges > 4 * ::neverd::limits::ir::kSourceVariableFlowMaxNodes) {
          Error = "The source control-flow graph is incomplete";
          return;
        }
        Predecessors[Next].push_back(N);
      }
      for (const auto *Values : {&Flow.Nodes[N].Reads, &Flow.Nodes[N].Writes})
        for (unsigned V : *Values)
          if (V >= Flow.Variables.size()) {
            Error = "An edit references an unknown source value";
            return;
          }
    }
    std::vector<size_t> Work{Flow.Entry};
    while (!Work.empty()) {
      const size_t N = Work.back();
      Work.pop_back();
      if (Reachable[N])
        continue;
      Reachable[N] = true;
      for (size_t Next : Flow.Nodes[N].Successors)
        Work.push_back(Next);
    }
  }
};
std::string eligible(const SourceVariableFlow &Flow, unsigned V) {
  if (V >= Flow.Variables.size())
    return "The selected source value no longer exists";
  const auto &Variable = Flow.Variables[V];
  if (!Variable.Eligible || !Variable.Bytes)
    return Variable.Refusal.empty()
               ? "This value has escaping, partial or unsupported storage"
               : Variable.Refusal;
  return {};
}
unsigned mask(const std::vector<unsigned> &Values, unsigned A, unsigned B) {
  return (contains(Values, A) ? 1u : 0u) | (contains(Values, B) ? 2u : 0u);
}
} // namespace

std::string neverd::canMapSourceVariables(const SourceVariableFlow &Flow,
                                          unsigned From, unsigned To) {
  if (auto Error = eligible(Flow, From); !Error.empty())
    return Error;
  if (auto Error = eligible(Flow, To); !Error.empty())
    return Error;
  if (From == To)
    return "Choose a different variable";
  if (Flow.Variables[From].EntryValue)
    return "Keep the entry parameter as the mapping destination";
  if (Flow.Variables[From].Bytes != Flow.Variables[To].Bytes ||
      Flow.Variables[From].TypeKey != Flow.Variables[To].TypeKey)
    return "The variables must have the same complete scalar type and width";
  Graph G(Flow);
  if (!G.Error.empty())
    return G.Error;
  bool Related = false;
  for (size_t N = 0; N < Flow.Nodes.size(); ++N)
    if (G.Reachable[N] && Flow.Nodes[N].Copy)
      Related |= *Flow.Nodes[N].Copy == std::pair{From, To} ||
                 *Flow.Nodes[N].Copy == std::pair{To, From};
  if (!Related)
    return "No complete value-copy relationship connects these variables";
  const unsigned Entry = (Flow.Variables[From].EntryValue ? 1u : 0u) |
                         (Flow.Variables[To].EntryValue ? 2u : 0u);
  std::vector<unsigned> Defined(Flow.Nodes.size(), 3), In(Flow.Nodes.size(), 3);
  bool Changed = true;
  size_t Work = 0;
  while (Changed) {
    Changed = false;
    for (size_t N = 0; N < Flow.Nodes.size(); ++N) {
      if (!G.Reachable[N])
        continue;
      if (++Work > ::neverd::limits::ir::kSourceVariableFlowMaxWork)
        return "The variable definition proof exceeded its budget";
      unsigned Before = N == Flow.Entry ? Entry : 3;
      for (size_t P : G.Predecessors[N])
        if (G.Reachable[P])
          Before &= Defined[P];
      In[N] = Before;
      unsigned After = Before | mask(Flow.Nodes[N].Writes, From, To);
      if (Defined[N] != After) {
        Defined[N] = After;
        Changed = true;
      }
    }
  }
  for (size_t N = 0; N < Flow.Nodes.size(); ++N)
    if (G.Reachable[N] && (mask(Flow.Nodes[N].Reads, From, To) & ~In[N]))
      return "A value can be read before a complete definition on a reachable "
             "path";
  std::vector<unsigned> LiveIn(Flow.Nodes.size()), LiveOut(Flow.Nodes.size());
  Changed = true;
  Work = 0;
  while (Changed) {
    Changed = false;
    for (size_t N = Flow.Nodes.size(); N-- > 0;) {
      if (!G.Reachable[N])
        continue;
      if (++Work > ::neverd::limits::ir::kSourceVariableFlowMaxWork)
        return "The variable liveness proof exceeded its budget";
      unsigned After = 0;
      for (size_t Next : Flow.Nodes[N].Successors)
        After |= LiveIn[Next];
      LiveOut[N] = After;
      const auto &Node = Flow.Nodes[N];
      unsigned Before =
          mask(Node.Reads, From, To) | (After & ~mask(Node.Writes, From, To));
      if (LiveIn[N] != Before) {
        LiveIn[N] = Before;
        Changed = true;
      }
    }
  }
  for (size_t N = 0; N < Flow.Nodes.size(); ++N) {
    if (!G.Reachable[N])
      continue;
    const auto &Node = Flow.Nodes[N];
    const unsigned Writes = mask(Node.Writes, From, To);
    if (Writes == 3)
      return "One operation defines both variables";
    const bool Copy = Node.Copy && (*Node.Copy == std::pair{From, To} ||
                                    *Node.Copy == std::pair{To, From});
    if (!Copy && (((Writes & 1) && (LiveOut[N] & 2)) ||
                  ((Writes & 2) && (LiveOut[N] & 1))))
      return "The variables have overlapping live ranges; mapping would "
             "overwrite a live value";
  }
  return {};
}

SourceVariableSplit neverd::splitSourceVariable(const SourceVariableFlow &Flow,
                                                unsigned Variable) {
  SourceVariableSplit Result;
  auto Fail = [&](std::string Error) {
    Result.Refusal = std::move(Error);
    Result.Partitions = 0;
    return Result;
  };
  if (auto Error = eligible(Flow, Variable); !Error.empty())
    return Fail(Error);
  if (Flow.Variables[Variable].EntryValue)
    return Fail(
        "Entry parameters cannot be split into local storage lifetimes");
  Graph G(Flow);
  if (!G.Error.empty())
    return Fail(G.Error);
  std::vector<size_t> Definitions;
  Result.Definitions.resize(Flow.Nodes.size(), SourceVariableSplit::None);
  Result.Uses.resize(Flow.Nodes.size(), SourceVariableSplit::None);
  for (size_t N = 0; N < Flow.Nodes.size(); ++N)
    if (G.Reachable[N] && contains(Flow.Nodes[N].Writes, Variable)) {
      Result.Definitions[N] = Definitions.size();
      Definitions.push_back(N);
    }
  const unsigned Count = Definitions.size();
  if (Count < 2)
    return Fail("This value has only one complete definition");
  if (Count > ::neverd::limits::ir::
                  kSourceVariableFlowNeverdSplitSourceVariableCountLimit)
    return Fail(
        "The variable has too many definitions for an interactive split");
  const size_t Words = (Count + 1 + 63) / 64;
  if (Words * Flow.Nodes.size() >
      ::neverd::limits::ir::
          kSourceVariableFlowNeverdSplitSourceVariableWordsFlowNodesSizeLimit)
    return Fail("The reaching-definition proof exceeds its memory budget");
  using Bits = std::vector<uint64_t>;
  std::vector<Bits> Out(Flow.Nodes.size(), Bits(Words));
  std::deque<size_t> Pending{Flow.Entry};
  std::vector<bool> Queued(Flow.Nodes.size());
  Queued[Flow.Entry] = true;
  size_t Work = 0;
  auto Incoming = [&](size_t N) {
    Bits Before(Words);
    if (N == Flow.Entry)
      Before[Count / 64] |= uint64_t{1} << (Count % 64);
    for (size_t P : G.Predecessors[N])
      if (G.Reachable[P])
        for (size_t W = 0; W < Words; ++W)
          Before[W] |= Out[P][W];
    return Before;
  };
  while (!Pending.empty()) {
    const size_t N = Pending.front();
    Pending.pop_front();
    Queued[N] = false;
    if (++Work > ::neverd::limits::ir::kSourceVariableFlowMaxWork)
      return Fail("The reaching-definition proof exceeded its work budget");
    Bits Next = Incoming(N);
    if (Result.Definitions[N] != SourceVariableSplit::None) {
      std::fill(Next.begin(), Next.end(), 0);
      const unsigned D = Result.Definitions[N];
      Next[D / 64] |= uint64_t{1} << (D % 64);
    }
    if (Out[N] == Next)
      continue;
    Out[N] = std::move(Next);
    for (size_t S : Flow.Nodes[N].Successors)
      if (!Queued[S]) {
        Pending.push_back(S);
        Queued[S] = true;
      }
  }
  std::vector<unsigned> Parent(Count);
  std::iota(Parent.begin(), Parent.end(), 0);
  auto Root = [&](unsigned D) {
    while (Parent[D] != D) {
      Parent[D] = Parent[Parent[D]];
      D = Parent[D];
    }
    return D;
  };
  for (size_t N = 0; N < Flow.Nodes.size(); ++N) {
    if (!G.Reachable[N] || !contains(Flow.Nodes[N].Reads, Variable))
      continue;
    const Bits Before = Incoming(N);
    if (Before[Count / 64] & (uint64_t{1} << (Count % 64)))
      return Fail("A reachable use has no complete definition");
    unsigned First = SourceVariableSplit::None;
    for (unsigned D = 0; D < Count; ++D)
      if (Before[D / 64] & (uint64_t{1} << (D % 64))) {
        if (First == SourceVariableSplit::None)
          First = D;
        else
          Parent[Root(D)] = Root(First);
      }
    if (First == SourceVariableSplit::None)
      return Fail("A reachable use has no reaching definition");
    Result.Uses[N] = First;
  }
  std::vector<unsigned> Used;
  for (unsigned D : Result.Uses)
    if (D != SourceVariableSplit::None)
      Used.push_back(Root(D));
  std::sort(Used.begin(), Used.end());
  Used.erase(std::unique(Used.begin(), Used.end()), Used.end());
  if (Used.size() < 2)
    return Fail("The definitions meet in one used lifetime; no independent "
                "lifetime can be split");
  for (auto *Partitions : {&Result.Uses, &Result.Definitions})
    for (unsigned &D : *Partitions)
      if (D != SourceVariableSplit::None) {
        const auto It = std::lower_bound(Used.begin(), Used.end(), Root(D));
        D = It != Used.end() && *It == Root(D) ? It - Used.begin()
                                               : SourceVariableSplit::None;
      }
  Result.Partitions = Used.size();
  return Result;
}
