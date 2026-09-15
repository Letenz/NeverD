#include "neverd/ir/med/MedConstantPropagation.h"

#include "neverd/ir/med/MedIR.h"

#include <deque>
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
};
} // namespace

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
    const std::set<int> Predecessors(Block.Preds.begin(), Block.Preds.end());
    const bool Complete = !Predecessors.empty() &&
                          Predecessors.size() == Block.Preds.size() &&
                          Predecessors == Incoming[Block.Id];
    for (const auto &Phi : Block.Phis) {
      if (Phi.Args.size() > Budget)
        return false;
      Budget -= Phi.Args.size();
      std::vector<MedVar> Values;
      std::set<int> Edges;
      for (const auto &[Predecessor, Value] : Phi.Args) {
        Edges.insert(Predecessor);
        Values.push_back(Value);
      }
      Add(Phi.Output, std::move(Values),
          Complete && Edges.size() == Phi.Args.size() && Edges == Predecessors);
    }
    for (const auto &Op : Block.Ops) {
      if (Op.NumInputs > Op.Inputs.size())
        return false;
      const bool Copy = Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
                        !Op.Dead && !Op.SourceCallHint &&
                        Op.MemoryOrdering == NdMemoryOrdering::None &&
                        Op.MemoryAddressSpace == NdMemoryAddressSpace::Default;
      Add(Op.Output, Copy ? std::vector{Op.Inputs[0]} : std::vector<MedVar>{},
          Copy);
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
        if (Input.Size != Node.Output.Size) {
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
        if (State == Knowledge::Constant && Value != *Constant) {
          State = Knowledge::Varying;
          break;
        }
        State = Knowledge::Constant;
        Value = *Constant;
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

  bool Changed = false;
  auto Replace = [&](MedVar &Input, bool Numeric) {
    if (Input.isConst())
      return;
    auto It = Nodes.find(key(Input));
    if (It == Nodes.end() || It->second.State != Knowledge::Constant ||
        It->second.Output.Size != Input.Size)
      return;
    Input = It->second.Value;
    if (Numeric && Input.Provenance == ConstantAddressProvenance::Unknown)
      Input.Provenance = ConstantAddressProvenance::Scalar;
    Changed = true;
  };
  for (auto &Block : Func.Blocks) {
    for (auto &Phi : Block.Phis)
      for (auto &[Predecessor, Value] : Phi.Args)
        Replace(Value, false);
    for (auto &Op : Block.Ops)
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        Replace(Op.Inputs[I], isNumericConstantOperand(Op.Opcode, I));
  }
  return Changed;
}
} // namespace neverd
