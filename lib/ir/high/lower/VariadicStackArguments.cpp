//===- VariadicStackArguments.cpp - Stack slots a call stores -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "VariadicStackArguments.h"

#include "HighEntryStackOffsets.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <set>

namespace neverd {
namespace detail {

namespace {

const MedBlock *blockOf(const MedFunc &Func, int Id) {
  for (const auto &Block : Func.Blocks)
    if (Block.Id == Id)
      return &Block;
  return nullptr;
}

/// Whether \p Block, or a block it reaches, reads \p View before writing it:
/// the reads the sub-register fixup keeps a view in step with writes to the
/// whole register for.
bool viewReadFrom(const MedFunc &Func, const MedBlock &Block,
                  const MedVar &View) {
  std::set<int> Seen{Block.Id};
  std::vector<const MedBlock *> Pending{&Block};
  while (!Pending.empty()) {
    const MedBlock *Current = Pending.back();
    Pending.pop_back();
    std::set<int> Written;
    for (const auto &Op : Current->Ops) {
      for (uint8_t I = 0; I < Op.NumInputs; ++I) {
        const MedVar &Input = Op.Inputs[I];
        if (Input.Kind == MedVar::Reg && Input.RegOff == View.RegOff &&
            Input.Size == View.Size && !Written.count(Input.SSAVer))
          return true;
      }
      if (Op.Output.Kind == MedVar::Reg && Op.Output.RegOff == View.RegOff &&
          Op.Output.Size == View.Size)
        Written.insert(Op.Output.SSAVer);
    }
    for (int Succ : Current->Succs)
      if (Seen.insert(Succ).second)
        if (const MedBlock *Next = blockOf(Func, Succ))
          Pending.push_back(Next);
  }
  return false;
}

} // namespace

const PhiNode *registerPhi(const MedFunc &Func, const MedBlock &Block,
                           uint64_t RegOff, const TargetRegInfo &TRI) {
  auto OnRegister = [&](const PhiNode &Phi) {
    return Phi.Output.Kind == MedVar::Reg && Phi.Output.RegOff == RegOff &&
           Phi.Output.Size > 0;
  };
  const PhiNode *Widest = nullptr;
  for (const auto &Phi : Block.Phis)
    if (OnRegister(Phi) && (!Widest || Phi.Output.Size > Widest->Output.Size))
      Widest = &Phi;
  for (const auto &Phi : Block.Phis)
    if (OnRegister(Phi) && (Phi.Output.Size == Widest->Output.Size ||
                            !TRI.writeZeroExtends(RegOff, Phi.Output.Size) ||
                            viewReadFrom(Func, Block, Phi.Output)))
      return &Phi;
  return Widest;
}

bool reachingRegisterAtBlockEntry(const MedFunc &Func, const MedBlock &Block,
                                  uint64_t RegOff, const TargetRegInfo &TRI,
                                  MedVar &Out) {
  auto blockById = [&](int Id) { return blockOf(Func, Id); };

  std::set<int> Visited;
  std::function<bool(const MedBlock &, MedVar &)> entryOf;
  std::function<bool(const MedBlock &, MedVar &)> exitOf;
  entryOf = [&](const MedBlock &Blk, MedVar &R) -> bool {
    if (!Visited.insert(Blk.Id).second)
      return false;
    if (const PhiNode *Phi = registerPhi(Func, Blk, RegOff, TRI)) {
      R = Phi->Output;
      return true;
    }
    if (Blk.Preds.empty()) {
      // Function entry: the identity COPY is the live-in. Do not invent every
      // unused incoming parameter; a later 1-arg call still has rdx live.
      for (const auto &Op : Blk.Ops) {
        if (Op.Opcode != NdOp::COPY)
          break;
        if (Op.Output.Kind == MedVar::Reg && Op.Output.RegOff == RegOff &&
            Op.Output.Size > 0 && Op.NumInputs >= 1 &&
            Op.Inputs[0].Kind == MedVar::Reg && Op.Inputs[0].RegOff == RegOff &&
            Op.Inputs[0].Id == Op.Output.Id && Op.Inputs[0].SSAVer == 0) {
          R = Op.Output;
          return true;
        }
      }
      return false;
    }
    for (int P : Blk.Preds)
      if (const MedBlock *PB = blockById(P))
        if (exitOf(*PB, R))
          return true;
    return false;
  };
  exitOf = [&](const MedBlock &Blk, MedVar &R) -> bool {
    for (auto It = Blk.Ops.rbegin(); It != Blk.Ops.rend(); ++It)
      if (It->Output.Kind == MedVar::Reg && It->Output.RegOff == RegOff &&
          It->Output.Size > 0) {
        R = It->Output;
        return true;
      }
    return entryOf(Blk, R);
  };

  return entryOf(Block, Out);
}

std::vector<StackSlotValue>
storedStackArguments(const MedFunc &Func, const MedBlock &Block, size_t CallIdx,
                     Arch TheArch, HighEntryStackOffsets &Offsets) {
  const auto &TRI = getTargetRegInfo(TheArch);
  const int64_t Slot = TRI.PointerSize;
  std::vector<StackSlotValue> Arguments;
  if (Slot <= 0 || CallIdx >= Block.Ops.size())
    return Arguments;
  // The stack pointer the call is made with: the last one its block defines,
  // or else the one reaching the block.
  MedVar CallStack;
  bool Known = false;
  for (size_t J = CallIdx; J-- > 0 && !Known;)
    if (const MedVar &Out = Block.Ops[J].Output;
        Out.Kind == MedVar::Reg && Out.RegOff == TRI.StackPointer && Out.Size) {
      CallStack = Out;
      Known = true;
    }
  if (!Known)
    Known = reachingRegisterAtBlockEntry(Func, Block, TRI.StackPointer, TRI,
                                         CallStack);
  const std::optional<int64_t> Base =
      Known ? Offsets.offset(CallStack) : std::nullopt;
  if (!Base)
    return Arguments;
  std::map<int64_t, StackSlotValue> Slots;
  for (size_t J = CallIdx; J-- > 0;) {
    const MedOp &Op = Block.Ops[J];
    if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
      break;
    if (Op.Opcode != NdOp::STORE || Op.NumInputs < 2)
      continue;
    const std::optional<int64_t> At = Offsets.offset(Op.Inputs[0]);
    if (!At || *At < *Base || (*At - *Base) % Slot != 0)
      continue;
    const int64_t Bytes = std::max<int64_t>(Op.Inputs[1].Size, Slot);
    for (int64_t Byte = 0; Byte < Bytes; Byte += Slot)
      Slots.try_emplace(
          *At - *Base + Byte,
          StackSlotValue{Op.Inputs[1], static_cast<uint16_t>(Byte)});
  }
  for (int64_t Offset = 0; Slots.count(Offset); Offset += Slot)
    Arguments.push_back(Slots.at(Offset));
  return Arguments;
}

} // namespace detail

void sizeVariadicOverflowFromStores(std::vector<MedFunc> &Funcs, Arch TheArch) {
  const auto &TRI = getTargetRegInfo(TheArch);
  std::map<va_t, MedFunc *> Callees;
  for (MedFunc &F : Funcs)
    if (F.takesVectorArgumentRegisters() && F.VariadicOverflowCount == 0)
      Callees[F.Entry] = &F;
  if (Callees.empty() || TRI.PointerSize <= 0)
    return;
  std::map<va_t, int> Widest;
  for (const MedFunc &Caller : Funcs) {
    std::optional<detail::HighEntryStackOffsets> Offsets;
    for (const MedBlock &Block : Caller.Blocks)
      for (size_t I = 0; I < Block.Ops.size(); ++I) {
        const MedOp &Call = Block.Ops[I];
        if (Call.Opcode != NdOp::CALL || !Call.NumInputs ||
            !Call.Inputs[0].isConst() ||
            !Callees.count(Call.Inputs[0].ConstVal))
          continue;
        if (!Offsets)
          Offsets.emplace(Caller, TheArch);
        const int Stored = static_cast<int>(
            detail::storedStackArguments(Caller, Block, I, TheArch, *Offsets)
                .size());
        int &Count = Widest[Call.Inputs[0].ConstVal];
        Count = std::max(Count, Stored);
      }
  }
  // One trailing stack parameter per overflow argument, as
  // finalizeVariadicCallees adds them: the va_arg walk reads memory, so only
  // their position after the register parameters and their width matter.
  for (const auto &[Entry, Stored] : Widest) {
    const int Count = std::min(Stored, limits::kMaxCallArgs);
    if (Count <= 0)
      continue;
    MedFunc &Callee = *Callees.at(Entry);
    int MaxId = 0;
    for (const MedVar &P : Callee.Params)
      MaxId = std::max(MaxId, P.Id);
    for (int K = 0; K < Count; ++K) {
      MedVar P;
      P.Kind = MedVar::Param;
      P.Id = ++MaxId;
      P.RegOff = kNoParamReg;
      P.Size = static_cast<uint16_t>(TRI.PointerSize);
      P.TheArch = TheArch;
      Callee.Params.push_back(P);
    }
    Callee.VariadicOverflowCount = Count;
  }
}

} // namespace neverd
