//===- FlowProgress.cpp - Bounded progress proofs for scalar SSA ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A defined iteration advances a nowrap counter in a finite domain.
/// Exhaustion produces poison consumed directly by the exit branch (UB),
/// which satisfies LLVM willreturn. No fixed choice for undef is assumed.
//===----------------------------------------------------------------------===//

#include "FlowProgress.h"

#include "LLVMFlowSemantics.h"

#include "neverd/diff/Control.h"
#include "neverd/diff/Limits.h"
#include "neverd/diff/Program.h"

#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Operator.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace neverd::diff {
llvm::StringRef flowProgressName(FlowProgressKind Kind) {
  switch (Kind) {
  case FlowProgressKind::Unproven:
    return "unproven";
  case FlowProgressKind::Acyclic:
    return "acyclic";
  case FlowProgressKind::CounterLoops:
    return "counter_loops";
  }
  return "invalid";
}
namespace {
std::optional<FlowLoopProgress>
counterUpdate(const DataFlowGraph &G, uint32_t Block, uint32_t Header,
              uint32_t Update, uint32_t Compare, llvm::StringRef Contract,
              llvm::function_ref<void(uint64_t)> Charge) {
  const auto &U = G.Nodes[Update];
  Charge(1 + U.Semantics.size());
  // A component's sole entry header executes on every continued iteration.
  // Its update dominates the latch; early exits can return without consuming
  // poison, but the exact latch comparison must consume it before a backedge.
  if ((U.Block != Block && U.Block != Header) ||
      (U.Opcode != "add" && U.Opcode != "sub") || U.Inputs.size() != 2)
    return {};
  const auto *Phi = &U.Inputs[0], *Step = &U.Inputs[1];
  if (U.Opcode == "add" && Phi->Type == FlowOperand::Kind::Constant)
    std::swap(Phi, Step);
  if (Phi->Type != FlowOperand::Kind::Node ||
      Step->Type != FlowOperand::Kind::Constant)
    return {};
  // Exact suffixes, not a search through arbitrary source string attributes.
  unsigned Width = 0, Flags = 0;
  llvm::StringRef Type(Step->Semantics), Suffix(U.Semantics);
  if (!Type.consume_front("i") || Type.getAsInteger(10, Width) || !Width ||
      Width > std::numeric_limits<uint64_t>::digits ||
      !Suffix.consume_front(Contract) ||
      !Suffix.consume_front(";result=" + Step->Semantics + ";flags=") ||
      Suffix.getAsInteger(10, Flags) || Suffix != std::to_string(Flags))
    return {};
  constexpr unsigned NSW = llvm::OverflowingBinaryOperator::NoSignedWrap;
  constexpr unsigned NUW = llvm::OverflowingBinaryOperator::NoUnsignedWrap;
  // Every nonzero fixed step exhausts its finite nowrap domain, even when
  // the exit value is skipped. No signed conversion or iteration count is
  // needed, including the i64 minimum signed step and maximum unsigned step.
  if (!Step->Value || (Flags & ~(NSW | NUW)) || !(Flags & (NSW | NUW)))
    return {};
  const auto Counter = Phi->Node;
  const auto &P = G.Nodes[Counter];
  Charge(1 + P.Semantics.size());
  if (P.Block != Header || !P.Phi || P.Opcode != "phi" || P.Inputs.size() < 2 ||
      P.Semantics != Contract.str() + ";result=" + Step->Semantics + ";flags=0")
    return {};
  unsigned SelfInputs = 0;
  for (const auto &Input : P.Inputs) {
    Charge(1);
    if (Input.Predecessor == NoIndex)
      return {};
    if (Input.Predecessor == Block) {
      if (Input.Type != FlowOperand::Kind::Node || Input.Node != Update)
        return {};
      ++SelfInputs;
    }
  }
  if (SelfInputs != 1)
    return {};
  return FlowLoopProgress{Block, Counter, Update, Compare};
}

std::optional<FlowLoopProgress>
counterLoop(const DataFlowGraph &G, uint32_t Block, uint32_t Header,
            llvm::StringRef Contract,
            llvm::function_ref<void(uint64_t)> Charge) {
  const auto &B = G.Blocks[Block];
  if (B.Successors.size() != 2 ||
      (B.Successors[0] == Header) == (B.Successors[1] == Header))
    return {};
  const auto &Branch = G.Nodes[B.Terminator];
  Charge(1 + Branch.Semantics.size());
  if (Branch.Opcode != "br" || Branch.Inputs.size() != 1 ||
      Branch.Inputs[0].Type != FlowOperand::Kind::Node ||
      Branch.Semantics != Contract.str() + ";result=i0;flags=0")
    return {};
  const auto Compare = Branch.Inputs[0].Node;
  const auto &Cmp = G.Nodes[Compare];
  Charge(1 + Cmp.Semantics.size());
  llvm::StringRef Suffix(Cmp.Semantics);
  unsigned Predicate = 0;
  if (Cmp.Block != Block || Cmp.Opcode != "icmp" || Cmp.Inputs.size() != 2 ||
      !Suffix.consume_front(Contract) ||
      !Suffix.consume_front(";result=i1;flags=0;predicate=") ||
      Suffix.getAsInteger(10, Predicate) ||
      Suffix != std::to_string(Predicate) ||
      Predicate < llvm::CmpInst::FIRST_ICMP_PREDICATE ||
      Predicate > llvm::CmpInst::LAST_ICMP_PREDICATE)
    return {};
  // Any direct scalar comparison propagates poison from either operand to
  // the branch. Progress does not depend on its predicate or exit polarity.
  for (const auto &Input : Cmp.Inputs) {
    Charge(1);
    if (Input.Type == FlowOperand::Kind::Node)
      if (auto L = counterUpdate(G, Block, Header, Input.Node, Compare,
                                 Contract, Charge))
        return L;
  }
  return {};
}

struct FlowComponents {
  std::vector<uint32_t> Id, Header, Latch, Backedges;
  std::vector<uint8_t> MultipleEntries;
};

/// Iterative Tarjan traversal: each block/edge is visited a bounded number of
/// times, including unreachable components. Compute the unique header and its
/// internal predecessor inventory before scanning any counter PHI. Otherwise
/// many rejected latches could repeatedly scan a large externally seeded PHI.
FlowComponents components(const DataFlowGraph &G,
                          llvm::function_ref<void(uint64_t)> Charge) {
  const auto Size = G.Blocks.size();
  Charge(10 * uint64_t(Size));
  FlowComponents C{std::vector<uint32_t>(Size, NoIndex),
                   std::vector<uint32_t>(Size, NoIndex),
                   std::vector<uint32_t>(Size, NoIndex),
                   std::vector<uint32_t>(Size), std::vector<uint8_t>(Size)};
  std::vector<uint32_t> Order(Size, NoIndex), Low(Size), Pending;
  std::vector<uint8_t> OnStack(Size);
  struct Frame {
    uint32_t Block;
    size_t Next;
  };
  std::vector<Frame> Stack;
  uint32_t Next = 0, Count = 0;
  const auto Visit = [&](uint32_t Block) {
    Charge(1);
    Order[Block] = Low[Block] = Next++;
    Pending.push_back(Block);
    OnStack[Block] = true;
    Stack.push_back({Block, 0});
  };
  for (uint32_t Root = 0; Root < Size; ++Root) {
    Charge(1);
    if (Order[Root] != NoIndex)
      continue;
    Visit(Root);
    while (!Stack.empty()) {
      Charge(1);
      auto &Top = Stack.back();
      const auto Block = Top.Block;
      if (Top.Next < G.Blocks[Block].Successors.size()) {
        const auto To = G.Blocks[Block].Successors[Top.Next++];
        if (Order[To] == NoIndex)
          Visit(To);
        else if (OnStack[To])
          Low[Block] = std::min(Low[Block], Order[To]);
        continue;
      }
      if (Low[Block] == Order[Block]) {
        uint32_t Member;
        do {
          Charge(1);
          Member = Pending.back();
          Pending.pop_back();
          OnStack[Member] = false;
          C.Id[Member] = Count;
        } while (Member != Block);
        ++Count;
      }
      Stack.pop_back();
      if (!Stack.empty()) {
        const auto Parent = Stack.back().Block;
        Low[Parent] = std::min(Low[Parent], Low[Block]);
      }
    }
  }
  const auto Entry = [&](uint32_t Block) {
    auto &Header = C.Header[C.Id[Block]];
    if (Header != NoIndex && Header != Block)
      C.MultipleEntries[C.Id[Block]] = true;
    Header = Block;
  };
  if (G.EntryBlock != NoIndex)
    Entry(G.EntryBlock);
  for (uint32_t From = 0; From < Size; ++From) {
    Charge(1);
    for (const auto To : G.Blocks[From].Successors) {
      Charge(1);
      if (C.Id[From] != C.Id[To])
        Entry(To);
    }
  }
  for (uint32_t From = 0; From < Size; ++From) {
    Charge(1);
    const auto Id = C.Id[From];
    for (const auto To : G.Blocks[From].Successors) {
      Charge(1);
      if (C.Id[To] == Id && To == C.Header[Id]) {
        C.Latch[Id] = From;
        ++C.Backedges[Id];
      }
    }
  }
  return C;
}
} // namespace

FlowProgress proveFlowProgress(const DataFlowGraph &G, llvm::StringRef Contract,
                               const DataFlowOptions &Options,
                               llvm::function_ref<void(uint64_t)> Charge,
                               bool MultiBlock) {
  if (auto Error = validateFiniteLLVMFlow(G, Contract, Charge))
    throw std::invalid_argument(*Error);
  FlowProgress P;
  P.SourceContract = P.ObservedContract = Contract.str();
  Charge(1 + Contract.size() * 2 + G.Blocks.size());
  std::vector<uint32_t> Cut(G.Blocks.size(), NoIndex);
  // The root and each witness occupy the same collection allowance as nodes.
  const uint64_t Retained = G.Nodes.size() + G.ParameterViews.size() + 1;
  if (Retained > Options.MaxNodes)
    throw DataFlowInterrupted(ComparisonState::ResourceLimited);
  std::optional<FlowComponents> C;
  if (MultiBlock)
    C = components(G, Charge);
  for (uint32_t I = 0; I < G.Blocks.size(); ++I) {
    Charge(1 + G.Blocks[I].Successors.size());
    auto Header = I;
    if (C) {
      const auto Id = C->Id[I];
      Header = C->Header[Id];
      if (Header == NoIndex || C->MultipleEntries[Id] ||
          C->Backedges[Id] != 1 || C->Latch[Id] != I)
        continue;
      bool InternalExit = false;
      for (const auto To : G.Blocks[I].Successors)
        InternalExit |= To != Header && C->Id[To] == Id;
      if (InternalExit)
        continue;
    }
    if (auto L = counterLoop(G, I, Header, Contract, Charge)) {
      if (C) {
        bool Reset = false;
        for (const auto &Input : G.Nodes[L->Counter].Inputs) {
          Charge(1);
          Reset |=
              Input.Predecessor != I && C->Id[Input.Predecessor] == C->Id[I];
        }
        if (Reset)
          continue;
        L->Header = Header;
      }
      if (P.Loops.size() == Options.MaxNodes - Retained)
        throw DataFlowInterrupted(ComparisonState::ResourceLimited);
      P.Loops.push_back(*L);
      Cut[I] = Header;
    }
  }
  Charge(G.Blocks.size());
  std::vector<uint32_t> Degree(G.Blocks.size()), Ready;
  for (uint32_t I = 0; I < G.Blocks.size(); ++I)
    for (auto To : G.Blocks[I].Successors) {
      Charge(1);
      if (To != Cut[I])
        ++Degree[To];
    }
  for (uint32_t I = 0; I < Degree.size(); ++I) {
    Charge(1);
    if (!Degree[I])
      Ready.push_back(I);
  }
  for (size_t I = 0; I < Ready.size(); ++I) {
    Charge(1);
    const auto From = Ready[I];
    for (auto To : G.Blocks[From].Successors) {
      Charge(1);
      if (To != Cut[From] && !--Degree[To])
        Ready.push_back(To);
    }
  }
  if (Ready.size() != G.Blocks.size()) {
    P.Loops.clear();
    return P;
  }
  P.Kind = P.Loops.empty() ? FlowProgressKind::Acyclic
                           : FlowProgressKind::CounterLoops;
  return P;
}

std::optional<std::string>
validateFlowProgress(const DataFlowGraph &G, const DataFlowOptions &Options,
                     llvm::function_ref<void(uint64_t)> Charge) {
  if (!hasProgressProof(G.Profile))
    return G.Progress
               ? std::optional<std::string>(
                     "progress evidence disagrees with the graph profile")
               : std::nullopt;
  if (!G.Progress)
    return "progress profile has no source contract evidence";
  const auto &P = *G.Progress;
  Charge(1 + P.SourceContract.size() + P.ObservedContract.size() +
         P.Loops.size());
  if (G.retainedItems() > Options.MaxNodes || P.SourceContract.empty() ||
      P.ObservedContract.empty() ||
      P.SourceContract.size() > limits::diff::kMaxFlowSemanticsBytes ||
      P.ObservedContract.size() > limits::diff::kMaxFlowSemanticsBytes ||
      (P.Kind == FlowProgressKind::Unproven &&
       P.SourceContract != P.ObservedContract))
    return "progress evidence exceeds its allowance or changes an unproved "
           "contract";
  try {
    const auto Derived =
        proveFlowProgress(G, P.ObservedContract, Options, Charge,
                          G.Profile == LLVMMultiBlockProgressDataFlowProfile);
    if (Derived.Kind != P.Kind || Derived.Loops != P.Loops)
      return "progress witnesses do not prove the represented control flow";
  } catch (const std::invalid_argument &Error) {
    return Error.what();
  }
  return {};
}
} // namespace neverd::diff
