//===- RegisterFlow.cpp - Bounded physical register dataflow ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded physical register dataflow.
//===----------------------------------------------------------------------===//

#include "neverd/ir/RegisterFlow.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/TargetRegInfo.h"

#include <deque>
#include <set>
#include <tuple>

namespace neverd {
namespace {
struct Node {
  va_t PC = 0;
  std::vector<const LowOp *> Ops;
  std::vector<size_t> Next, Previous;
  bool Root = false, Boundary = false;
};
uint64_t mask(unsigned Bytes) {
  return Bytes >= 8 ? UINT64_MAX : (uint64_t(1) << (Bytes * 8)) - 1;
}
uint64_t overlap(NdVar V, NdVar R) {
  uint64_t Mask = 0;
  if (!V.isReg())
    return 0;
  for (unsigned B = 0; B < R.Size; ++B)
    if (R.Offset + B >= V.Offset && R.Offset + B - V.Offset < V.Size)
      Mask |= uint64_t(1) << B;
  return Mask;
}
bool zeroExtends(const LowOp &O, const TargetRegInfo &TRI) {
  const auto V = O.Output;
  if (!V.isReg() || !TRI.writeZeroExtends(V.Offset, V.Size))
    return false;
  if (O.Opcode == NdOp::SUBBYTES && O.NumInputs == 2 && O.Inputs[0].isReg() &&
      O.Inputs[1].isConst() &&
      isSameRegisterLowSlice(V.Offset, V.Size, O.Inputs[0].Offset,
                             O.Inputs[0].Size, O.Inputs[1].Offset))
    return false;
  return true;
}
uint64_t writes(const Node &N, NdVar R, const TargetRegInfo &TRI) {
  uint64_t Mask = 0;
  for (auto *O : N.Ops) {
    auto V = O->Output;
    Mask |= overlap(V, R);
    if (zeroExtends(*O, TRI)) {
      auto [Off, Bytes] = TRI.findWideReg(V.Offset, V.Size);
      Mask |= overlap(NdVar::reg(Off, Bytes), R);
    }
  }
  return Mask;
}
using ByteKey = std::pair<VnodeSpace, uint64_t>;
using State = std::map<ByteKey, uint8_t>;
std::optional<uint64_t> read(const State &S, NdVar V) {
  if (!V.Size || V.Size > 8)
    return {};
  if (V.isConst())
    return V.Offset & mask(V.Size);
  if (!V.isReg() && !V.isTemp())
    return {};
  uint64_t Value = 0;
  for (unsigned B = 0; B < V.Size; ++B) {
    const auto It = S.find({V.Space, V.Offset + B});
    if (It == S.end())
      return {};
    Value |= uint64_t(It->second) << (B * 8);
  }
  return Value;
}
void forget(State &S, NdVar V) {
  if (!V.isReg() && !V.isTemp())
    return;
  for (unsigned B = 0; B < V.Size; ++B)
    S.erase({V.Space, V.Offset + B});
}
void transfer(State &S, const LowOp &O, const TargetRegInfo &TRI) {
  std::optional<uint64_t> Result;
  const auto A = O.NumInputs ? read(S, O.Inputs[0]) : std::nullopt;
  const auto B = O.NumInputs > 1 ? read(S, O.Inputs[1]) : std::nullopt;
  if (A) {
    switch (O.Opcode) {
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
      Result = *A;
      break;
    case NdOp::INT_SEXT:
      if (O.Inputs[0].Size && O.Inputs[0].Size <= 8) {
        auto V = *A;
        if (V & (uint64_t(1) << (O.Inputs[0].Size * 8 - 1)))
          V |= ~mask(O.Inputs[0].Size);
        Result = V;
      }
      break;
    case NdOp::INT_NOT:
    case NdOp::INT_NEGATE:
      Result = ~*A;
      break;
    case NdOp::INT_NEG2:
      Result = uint64_t(0) - *A;
      break;
    case NdOp::BOOL_NOT:
      Result = !*A;
      break;
    default:
      break;
    }
  }
  if (A && B) {
    switch (O.Opcode) {
    case NdOp::INT_ADD:
      Result = *A + *B;
      break;
    case NdOp::INT_SUB:
      Result = *A - *B;
      break;
    case NdOp::INT_MULT:
      Result = (*A) * (*B);
      break;
    case NdOp::INT_AND:
      Result = *A & *B;
      break;
    case NdOp::INT_OR:
      Result = *A | *B;
      break;
    case NdOp::INT_XOR:
      Result = *A ^ *B;
      break;
    case NdOp::BOOL_AND:
      Result = bool(*A) && bool(*B);
      break;
    case NdOp::BOOL_OR:
      Result = bool(*A) || bool(*B);
      break;
    case NdOp::BOOL_XOR:
      Result = bool(*A) != bool(*B);
      break;
    case NdOp::INT_EQUAL:
      Result = *A == *B;
      break;
    case NdOp::INT_NOTEQUAL:
      Result = *A != *B;
      break;
    case NdOp::INT_LESS:
      Result = *A < *B;
      break;
    case NdOp::INT_LESSEQUAL:
      Result = *A <= *B;
      break;
    case NdOp::INT_LEFT:
      if (*B < 64)
        Result = *A << *B;
      break;
    case NdOp::INT_RIGHT:
      if (*B < 64)
        Result = *A >> *B;
      break;
    case NdOp::SUBBYTES:
      if (*B < 8)
        Result = *A >> (*B * 8);
      break;
    case NdOp::CONCAT:
      if (O.Inputs[1].Size < 8)
        Result = (*A << (O.Inputs[1].Size * 8)) | *B;
      break;
    default:
      break;
    }
  }
  auto V = O.Output;
  forget(S, V);
  if (zeroExtends(O, TRI)) {
    const auto [Off, Bytes] = TRI.findWideReg(V.Offset, V.Size);
    for (unsigned N = 0; N < Bytes; ++N)
      if (Off + N < V.Offset || Off + N - V.Offset >= V.Size)
        S[{VnodeSpace::REG, Off + N}] = 0;
  }
  if (Result && (V.isReg() || V.isTemp()) && V.Size <= 8)
    for (unsigned N = 0; N < V.Size; ++N)
      S[{V.Space, V.Offset + N}] = static_cast<uint8_t>(*Result >> (N * 8));
}
} // namespace

RegisterFlowResult
queryRegisterFlow(const LowFunc &F, Arch A, va_t PC, NdVar R,
                  const std::string &Kind,
                  const std::map<va_t, RegisterValueAudit> &Audits) {
  RegisterFlowResult Out;
  if (!R.isReg() || !R.Size || R.Size > 64 ||
      (Kind != "definitions" && Kind != "uses" && Kind != "value") ||
      F.Blocks.size() >
          ::neverd::limits::ir::
              kRegisterFlowQueryRegisterFlowValueUsesDefinitionsFBlocksSizeLimit ||
      (F.DecodedInstructionCount && !F.hasCompleteInstructionLift())) {
    Out.Error = "Register flow needs a bounded complete native function and "
                "physical register";
    return Out;
  }
  const auto &TRI = getTargetRegInfo(A);
  std::vector<Node> Nodes;
  std::map<int, std::vector<size_t>> BlockNodes;
  std::map<int, const LowBlock *> Blocks;
  std::map<va_t, size_t> ByAddress;
  size_t Count = 0;
  for (const auto &B : F.Blocks) {
    Blocks[B.Id] = &B;
    for (auto &O : B.Ops) {
      if (++Count >
          ::neverd::limits::ir::kRegisterFlowQueryRegisterFlowCountLimit) {
        Out.Error = "Register-flow operation budget exceeded";
        return Out;
      }
      auto &IDs = BlockNodes[B.Id];
      if (IDs.empty() || Nodes[IDs.back()].PC != O.Addr) {
        if (!ByAddress.emplace(O.Addr, Nodes.size()).second) {
          Out.Error = "Ambiguous instruction occurrence";
          return Out;
        }
        IDs.push_back(Nodes.size());
        Nodes.push_back({O.Addr});
      }
      auto &N = Nodes[IDs.back()];
      N.Ops.push_back(&O);
      N.Boundary |= O.Opcode == NdOp::CALL || O.Opcode == NdOp::INDIR_CALL ||
                    O.Opcode == NdOp::INTRINSIC;
    }
  }
  if (Nodes.size() >
          ::neverd::limits::ir::kRegisterFlowQueryRegisterFlowNodesSizeLimit ||
      !ByAddress.count(PC)) {
    Out.Error = "Select a retained instruction within the register-flow budget";
    return Out;
  }
  auto First = [&](int Id) {
    std::vector<size_t> Result;
    std::set<int> Seen;
    std::vector<int> Work{Id};
    while (!Work.empty()) {
      auto Next = Work.back();
      Work.pop_back();
      if (!Seen.insert(Next).second || !Blocks.count(Next))
        continue;
      if (!BlockNodes[Next].empty())
        Result.push_back(BlockNodes[Next].front());
      else
        for (int S : Blocks.at(Next)->Succs)
          Work.push_back(S);
    }
    return Result;
  };
  for (const auto &B : F.Blocks) {
    const auto &IDs = BlockNodes[B.Id];
    const bool Root = B.StartAddr == F.Entry ||
                      F.ModuleAnalysisRoots.count(B.StartAddr) ||
                      F.OrdinaryModuleAnalysisRoots.count(B.StartAddr) ||
                      !B.ExceptionalPreds.empty();
    if (Root)
      for (auto N : First(B.Id))
        Nodes[N].Root = true;
    if (IDs.empty())
      continue;
    for (size_t I = 0; I < IDs.size(); ++I) {
      auto &N = Nodes[IDs[I]];
      if (I + 1 < IDs.size())
        N.Next.push_back(IDs[I + 1]);
      else
        for (int S : B.Succs) {
          if (!Blocks.count(S)) {
            Out.Error = "Incomplete register-flow CFG";
            return Out;
          }
          auto Targets = First(S);
          N.Next.insert(N.Next.end(), Targets.begin(), Targets.end());
        }
    }
  }
  for (size_t I = 0; I < Nodes.size(); ++I)
    for (auto N : Nodes[I].Next)
      Nodes[N].Previous.push_back(I);
  const auto Selected = ByAddress.at(PC);
  const uint64_t All = R.Size == 64 ? UINT64_MAX : (uint64_t(1) << R.Size) - 1;
  std::set<std::pair<va_t, std::string>> Published;
  auto Site = [&](va_t At, const std::string &K, const std::string &Why) {
    if (Published.emplace(At, K).second)
      Out.Sites.push_back({At, K, Why});
    Out.Unknown |= K == "boundary";
  };
  if (Kind != "value") {
    std::vector<std::pair<size_t, uint64_t>> Work;
    if (Kind == "uses" && Nodes[Selected].Boundary) {
      Site(PC, "boundary",
           "The selected call or opaque instruction has unproven implicit "
           "register effects");
      return Out;
    }
    if (Kind == "definitions") {
      if (Nodes[Selected].Root || Nodes[Selected].Previous.empty())
        Site(PC, "boundary", "Entry or exceptional register value is unknown");
      for (auto N : Nodes[Selected].Previous)
        Work.emplace_back(N, All);
    } else
      for (auto N : Nodes[Selected].Next)
        Work.emplace_back(N, All);
    std::set<std::pair<size_t, uint64_t>> Seen;
    while (!Work.empty()) {
      if (Seen.size() > ::neverd::limits::ir::
                            kRegisterFlowQueryRegisterFlowValueSeenSizeLimit ||
          Out.Sites.size() >
              ::neverd::limits::ir::
                  kRegisterFlowQueryRegisterFlowValueOutSitesSizeLimit) {
        Out.Error = "Register-flow traversal budget exceeded";
        Out.Sites.clear();
        return Out;
      }
      auto [I, Live] = Work.back();
      Work.pop_back();
      if (!Live || !Seen.emplace(I, Live).second)
        continue;
      const auto &N = Nodes[I];
      if (N.Boundary) {
        Site(N.PC, "boundary",
             "Call or opaque instruction has unproven implicit register "
             "effects");
        continue;
      }
      if (Kind == "uses") {
        for (auto *O : N.Ops) {
          for (unsigned J = 0; J < O->NumInputs; ++J)
            if (overlap(O->Inputs[J], R) & Live)
              Site(N.PC, "use", "Reads bytes of the selected register value");
          Node Single;
          Single.Ops = {O};
          Live &= ~writes(Single, R, TRI);
        }
      }
      const auto Written = writes(N, R, TRI) & Live;
      if (Written && Kind == "definitions")
        Site(N.PC, "definition", "Reaching physical register definition");
      Live &= ~Written;
      if (Live && Kind == "definitions" && (N.Root || N.Previous.empty()))
        Site(N.PC, "boundary",
             "Entry or exceptional register value is unknown");
      for (auto Next : Kind == "definitions" ? N.Previous : N.Next)
        Work.emplace_back(Next, Live);
    }
    return Out;
  }
  if (R.Size > 8) {
    Out.Unknown = true;
    return Out;
  }
  std::map<size_t, State> Inputs;
  std::deque<size_t> Work;
  std::set<size_t> Queued;
  auto Enqueue = [&](size_t N) {
    if (Queued.insert(N).second)
      Work.push_back(N);
  };
  for (size_t N = 0; N < Nodes.size(); ++N)
    if (Nodes[N].Root) {
      Inputs[N] = {};
      Enqueue(N);
    }
  size_t Budget = ::neverd::limits::ir::kRegisterFlowQueryRegisterFlowBudget,
         Retained = 0;
  while (!Work.empty()) {
    const size_t I = Work.front();
    Work.pop_front();
    Queued.erase(I);
    State S = Inputs.at(I);
    const auto &N = Nodes[I];
    std::erase_if(S, [&](const auto &V) {
      return V.first.first == VnodeSpace::TEMP &&
             !F.isFunctionTemporaryByte(V.first.second);
    });
    const auto Audit = Audits.find(N.PC);
    auto Arbitrary = [&](size_t After) {
      if (Audit != Audits.end() && Audit->second.Complete)
        for (const auto &[At, V] : Audit->second.Undefined)
          if (At == After)
            forget(S, V);
    };
    Arbitrary(0);
    size_t Operation = 0;
    for (auto *O : N.Ops) {
      size_t Cost = 1 + O->Output.Size;
      for (unsigned J = 0; J < O->NumInputs; ++J)
        Cost += O->Inputs[J].Size;
      if (Cost > Budget) {
        Out.Error = "Register value work budget exceeded";
        return Out;
      }
      Budget -= Cost;
      transfer(S, *O, TRI);
      Arbitrary(++Operation);
    }
    if (N.Boundary || Audit == Audits.end() || !Audit->second.Complete)
      S.clear();
    std::erase_if(S, [&](const auto &V) {
      return V.first.first == VnodeSpace::TEMP &&
             !F.isFunctionTemporaryByte(V.first.second);
    });
    if (S.size() >
        ::neverd::limits::ir::kRegisterFlowQueryRegisterFlowSSizeLimit) {
      Out.Error = "Register value fact budget exceeded";
      return Out;
    }
    for (auto Next : N.Next) {
      const auto [It, New] = Inputs.try_emplace(Next, S);
      if (New) {
        Retained += S.size();
        Enqueue(Next);
      } else {
        const auto Before = It->second.size();
        std::erase_if(It->second, [&](const auto &V) {
          auto Other = S.find(V.first);
          return Other == S.end() || Other->second != V.second;
        });
        if (Before != It->second.size())
          Enqueue(Next);
      }
      if (Retained >
          ::neverd::limits::ir::kRegisterFlowQueryRegisterFlowRetainedLimit) {
        Out.Error = "Register value state budget exceeded";
        return Out;
      }
    }
  }
  if (auto It = Inputs.find(Selected); It != Inputs.end())
    Out.Value = read(It->second, R);
  Out.Unknown = !Out.Value.has_value();
  return Out;
}
} // namespace neverd
