#include "neverd/ir/med/MedConstantPropagation.h"

#include "neverd/ir/med/MedIR.h"

#include <deque>
#include <optional>
#include <tuple>

namespace neverd {
namespace {
using Key = std::tuple<MedVar::VarKind, int, int>;
Key key(const MedVar &Value) { return {Value.Kind, Value.Id, Value.SSAVer}; }

enum class Knowledge { Pending, Constant, Varying };
struct ConstantNode {
  MedVar Output;
  std::vector<MedVar> Inputs;
  std::vector<Key> Users;
  Knowledge State = Knowledge::Pending;
  MedVar Value;
  bool Queued = false;
  /// A width view of the single input, an extension or the bytes SUBBYTES
  /// takes from ViewOffset, rather than a copy or a PHI.
  NdOp View = NdOp::COPY;
  uint64_t ViewOffset = 0;
  /// The value passes through a width view on the way here.
  bool FromView = false;
};

/// The constant \p View makes of constant \p In, sized \p Size: the view
/// of a register that `xor edx, edx` zeroes and the next instruction reads
/// as RDX is still that constant, and a number like it.  A view of an
/// address is no address, and none is made.
std::optional<MedVar> viewOfConstant(NdOp View, uint64_t ViewOffset,
                                     const MedVar &In, uint16_t Size) {
  if (In.Provenance != ConstantAddressProvenance::Unknown &&
      In.Provenance != ConstantAddressProvenance::Scalar)
    return std::nullopt;
  uint64_t Bits = In.ConstVal;
  const unsigned InBits = In.Size * 8u;
  if (InBits < 64)
    Bits &= (uint64_t(1) << InBits) - 1;
  if (View == NdOp::SUBBYTES)
    Bits = ViewOffset < 8 ? Bits >> (ViewOffset * 8) : 0;
  else if (View == NdOp::INT_SEXT && InBits && InBits < 64 &&
           ((Bits >> (InBits - 1)) & 1))
    Bits |= ~uint64_t(0) << InBits;
  if (Size < 8)
    Bits &= (uint64_t(1) << (Size * 8u)) - 1;
  return MedVar::makeConst(Bits, Size, In.Provenance);
}
} // namespace

bool hasCompleteOrdinaryPhiInputs(const MedBlock &Block, const PhiNode &Phi,
                                  const std::set<int> &Incoming) {
  if (Phi.ExceptionalEntry || !Phi.Output.Size || Block.Preds.empty() ||
      Block.Preds.size() != Incoming.size() ||
      Phi.Args.size() != Block.Preds.size())
    return false;
  const std::set<int> Predecessors(Block.Preds.begin(), Block.Preds.end());
  if (Predecessors.size() != Block.Preds.size() || Predecessors != Incoming ||
      Phi.Args.size() != Predecessors.size())
    return false;
  std::set<int> Edges;
  for (const auto &[Pred, Value] : Phi.Args)
    if (Value.Size != Phi.Output.Size || !Predecessors.count(Pred) ||
        !Edges.insert(Pred).second)
      return false;
  return Edges == Predecessors;
}

bool propagateInvariantConstants(MedFunc &Func) {
  constexpr size_t MaxDefinitions = 65536;
  if (Func.Blocks.size() > MaxDefinitions)
    return false;
  size_t Budget = 1000000;
  size_t Definitions = 0;
  std::map<int, const MedBlock *> Blocks;
  std::map<int, std::set<int>> Incoming;
  for (const auto &Block : Func.Blocks) {
    Definitions += Block.Ops.size() + Block.Phis.size();
    if (Definitions > MaxDefinitions || Block.Id < 0 ||
        !Blocks.emplace(Block.Id, &Block).second ||
        !Block.ExceptionalPreds.empty() || !Block.ExceptionalSuccs.empty())
      return false;
    for (int Successor : Block.Succs) {
      if (!Budget--)
        return false;
      Incoming[Successor].insert(Block.Id);
    }
  }
  for (const auto &[Id, Predecessors] : Incoming)
    if (!Blocks.count(Id))
      return false;

  std::map<Key, ConstantNode> Nodes;
  auto Add = [&](const MedVar &Output, std::vector<MedVar> Inputs, bool Valid) {
    if (Output.isConst() || Output.Id < 0 || !Output.Size)
      return;
    auto [It, Inserted] = Nodes.try_emplace(key(Output));
    auto &Node = It->second;
    if (!Inserted) {
      Node.State = Knowledge::Varying;
      Node.Inputs.clear();
      return;
    }
    Node.Output = Output;
    Node.Inputs = std::move(Inputs);
    if (!Valid || Output.Size > 8 || Node.Inputs.empty())
      Node.State = Knowledge::Varying;
  };
  for (const auto &Block : Func.Blocks) {
    for (const auto &Phi : Block.Phis) {
      if (Phi.Args.size() > Budget)
        return false;
      Budget -= Phi.Args.size();
      std::vector<MedVar> Values;
      for (const auto &[Predecessor, Value] : Phi.Args) {
        Values.push_back(Value);
      }
      // The dispatcher's entry brings a value no predecessor supplies.
      Add(Phi.Output, std::move(Values),
          hasCompleteOrdinaryPhiInputs(Block, Phi, Incoming[Block.Id]));
    }
    for (const auto &Op : Block.Ops) {
      if (Op.NumInputs > Op.Inputs.size())
        return false;
      const bool Plain =
          Op.RegistrationRoot == MedOp::RegistrationRootKind::None &&
          !Op.Dead && !Op.SourceCallHint &&
          Op.MemoryOrdering == NdMemoryOrdering::None &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default;
      const bool Copy = Plain && Op.Opcode == NdOp::COPY && Op.NumInputs == 1;
      // A constant holds a word: a view into a wider value (a vector lane, a
      // register pair, a double-word dividend) has none.
      const bool Extend =
          Plain &&
          (Op.Opcode == NdOp::INT_ZEXT || Op.Opcode == NdOp::INT_SEXT) &&
          Op.NumInputs == 1 && Op.Inputs[0].Size &&
          Op.Inputs[0].Size < Op.Output.Size && Op.Output.Size <= 8;
      const bool Slice =
          Plain && Op.Opcode == NdOp::SUBBYTES && Op.NumInputs == 2 &&
          Op.Inputs[1].isConst() && Op.Inputs[0].Size <= 8 &&
          Op.Inputs[1].ConstVal + Op.Output.Size <= Op.Inputs[0].Size;
      const bool Tracked = Copy || Extend || Slice;
      Add(Op.Output,
          Tracked ? std::vector{Op.Inputs[0]} : std::vector<MedVar>{}, Tracked);
      if ((Extend || Slice) && !Op.Output.isConst() && Op.Output.Id >= 0)
        if (auto It = Nodes.find(key(Op.Output)); It != Nodes.end()) {
          It->second.View = Op.Opcode;
          It->second.ViewOffset = Slice ? Op.Inputs[1].ConstVal : 0;
        }
    }
  }

  std::deque<Key> Work;
  auto Enqueue = [&](const Key &Id) {
    auto &Node = Nodes.at(Id);
    if (!Node.Queued) {
      Node.Queued = true;
      Work.push_back(Id);
    }
  };
  for (auto &[Id, Node] : Nodes) {
    for (const auto &Input : Node.Inputs) {
      if (!Budget--)
        return false;
      if (!Input.isConst())
        if (auto It = Nodes.find(key(Input)); It != Nodes.end())
          It->second.Users.push_back(Id);
    }
    Enqueue(Id);
  }
  auto Drain = [&] {
    while (!Work.empty()) {
      if (!Budget--)
        return false;
      auto &Node = Nodes.at(Work.front());
      Work.pop_front();
      Node.Queued = false;
      if (Node.State == Knowledge::Varying)
        continue;
      Knowledge State = Knowledge::Pending;
      MedVar Value;
      for (const auto &Input : Node.Inputs) {
        if (!Budget--)
          return false;
        if (Node.View == NdOp::COPY && Input.Size != Node.Output.Size) {
          State = Knowledge::Varying;
          break;
        }
        const MedVar *Constant = &Input;
        if (!Input.isConst()) {
          const auto It = Nodes.find(key(Input));
          if (It == Nodes.end() || It->second.Output.Size != Input.Size ||
              It->second.State == Knowledge::Varying) {
            State = Knowledge::Varying;
            break;
          }
          if (It->second.State == Knowledge::Pending)
            continue;
          Constant = &It->second.Value;
        }
        const std::optional<MedVar> Viewed =
            Node.View == NdOp::COPY
                ? *Constant
                : viewOfConstant(Node.View, Node.ViewOffset, *Constant,
                                 Node.Output.Size);
        if (!Viewed || (State == Knowledge::Constant && Value != *Viewed)) {
          State = Knowledge::Varying;
          break;
        }
        State = Knowledge::Constant;
        Value = *Viewed;
      }
      if (Node.State == Knowledge::Constant && State == Knowledge::Constant &&
          Node.Value != Value)
        State = Knowledge::Varying;
      if (State == Node.State)
        continue;
      Node.State = State;
      Node.Value = Value;
      for (const auto &User : Node.Users)
        Enqueue(User);
    }
    return true;
  };
  if (!Drain())
    return false;
  // An unseeded cycle cannot contribute a defined value. Make every remaining
  // pending component opaque, then invalidate any provisional constants that
  // depended on it. No operand is changed before both fixed points converge.
  for (auto &[Id, Node] : Nodes)
    if (Node.State == Knowledge::Pending) {
      Node.State = Knowledge::Varying;
      for (const auto &User : Node.Users)
        Enqueue(User);
    }
  if (!Drain())
    return false;

  // Which constants pass through a width view.  Such a constant reaches the
  // arithmetic that reads it, but not:
  // - a call, which reads its argument registers as its convention or source
  //   signature binds them (an Objective-C message's `W2`);
  // - a copy or another view, which keeps the register chain a later reader
  //   folds;
  // - a shift or a multiplication: a shifted word reads as an int literal in
  //   C (`(uint32_t)(0 >> 32)`), and a scaled index keeps the selector of the
  //   jump table it addresses.
  for (bool Grew = true; Grew;) {
    Grew = false;
    for (auto &[Id, Node] : Nodes) {
      if (Node.FromView)
        continue;
      bool FromView = Node.View != NdOp::COPY;
      for (const auto &Input : Node.Inputs) {
        if (!Budget--)
          return false;
        if (!Input.isConst())
          if (auto It = Nodes.find(key(Input));
              It != Nodes.end() && It->second.FromView)
            FromView = true;
      }
      if (FromView) {
        Node.FromView = true;
        Grew = true;
      }
    }
  }

  bool Changed = false;
  auto Replace = [&](MedVar &Input, bool Numeric, bool KeepsViews) {
    if (Input.isConst())
      return;
    auto It = Nodes.find(key(Input));
    if (It == Nodes.end() || It->second.State != Knowledge::Constant ||
        It->second.Output.Size != Input.Size ||
        (KeepsViews && It->second.FromView))
      return;
    Input = It->second.Value;
    if (Numeric && Input.Provenance == ConstantAddressProvenance::Unknown)
      Input.Provenance = ConstantAddressProvenance::Scalar;
    Changed = true;
  };
  for (auto &Block : Func.Blocks) {
    for (auto &Phi : Block.Phis)
      for (auto &[Predecessor, Value] : Phi.Args)
        Replace(Value, false, false);
    for (auto &Op : Block.Ops) {
      const bool KeepsViews =
          Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
          Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT ||
          Op.Opcode == NdOp::INT_SEXT || Op.Opcode == NdOp::SUBBYTES ||
          Op.Opcode == NdOp::INT_LEFT || Op.Opcode == NdOp::INT_RIGHT ||
          Op.Opcode == NdOp::INT_ASHR || Op.Opcode == NdOp::INT_MULT;
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        Replace(Op.Inputs[I], isNumericConstantOperand(Op.Opcode, I),
                KeepsViews);
    }
  }
  return Changed;
}
} // namespace neverd
