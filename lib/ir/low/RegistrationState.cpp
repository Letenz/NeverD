//===- RegistrationState.cpp - x86 EH reaching states ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/RegistrationState.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86Regs.h"

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <utility>

namespace neverd {
namespace {

struct Domain {
  std::set<int32_t> Levels;
  bool Unknown = false;
  bool Parent = false;
  bool Callback = false;
  bool Uninstalled = false;
};

struct FrameExpression {
  std::optional<int32_t> Offset;
  std::optional<uint32_t> Constant;
  bool MayBeFrame = false;
};

struct SlotWrite {
  va_t Address = 0;
  std::optional<int32_t> Level;
};

// Authenticate scanner observations against the lifted store, and discover
// direct non-literal/partial/indexed writes which the immediate scanner cannot
// represent. Instruction-local temporaries cannot carry evidence to a later
// instruction with a reused temporary number.
std::vector<SlotWrite> collectSlotWrites(const LowBlock &Block,
                                         int32_t SlotOffset, va_t InstallVA,
                                         bool &InstallProven) {
  std::vector<SlotWrite> Writes;
  std::map<uint64_t, FrameExpression> Temps;
  va_t Instruction = InvalidVA;
  auto Read = [&](const NdVar &Value) -> FrameExpression {
    if (Value.isConst())
      return {{}, static_cast<uint32_t>(Value.Offset), false};
    if (Value.isReg() && Value.Offset == x86reg::RBP && Value.Size == 4)
      return {0, {}, true};
    if (Value.isTemp()) {
      auto It = Temps.find(Value.Offset);
      if (It != Temps.end())
        return It->second;
    }
    return {};
  };
  for (const LowOp &Op : Block.Ops) {
    if (Instruction != Op.Addr) {
      Temps.clear();
      Instruction = Op.Addr;
    }
    if (Op.Addr == InstallVA && Op.Opcode == NdOp::STORE &&
        Op.MemoryAddressSpace == NdMemoryAddressSpace::X86FS &&
        Op.NumInputs == 2 && Read(Op.Inputs[0]).Constant == uint32_t{0} &&
        Op.Inputs[1].isReg() && Op.Inputs[1].Offset == x86reg::RSP &&
        Op.Inputs[1].Size == 4)
      InstallProven = true;
    if (Op.Opcode == NdOp::STORE && Op.NumInputs == 2 &&
        Op.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
      const FrameExpression Address = Read(Op.Inputs[0]);
      if (Address.MayBeFrame) {
        const uint16_t Width = Op.Inputs[1].Size;
        if (!Address.Offset ||
            (int64_t(*Address.Offset) < int64_t(SlotOffset) + 4 &&
             int64_t(SlotOffset) < int64_t(*Address.Offset) + Width)) {
          SlotWrite Write{Op.Addr, {}};
          if (Address.Offset == SlotOffset && Width == 4)
            if (auto Value = Read(Op.Inputs[1]).Constant)
              Write.Level = static_cast<int32_t>(*Value);
          Writes.push_back(Write);
        }
      }
    }
    if (!Op.Output.isTemp())
      continue;
    FrameExpression Value;
    for (unsigned I = 0; I < Op.NumInputs; ++I)
      Value.MayBeFrame |= Read(Op.Inputs[I]).MayBeFrame;
    if ((Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT) &&
        Op.NumInputs == 1)
      Value = Read(Op.Inputs[0]);
    else if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
             Op.NumInputs == 2) {
      const FrameExpression Left = Read(Op.Inputs[0]);
      const FrameExpression Right = Read(Op.Inputs[1]);
      if (Left.Offset && Right.Constant)
        Value.Offset = static_cast<int32_t>(
            uint32_t(*Left.Offset) +
            (Op.Opcode == NdOp::INT_ADD ? *Right.Constant : -*Right.Constant));
      else if (Op.Opcode == NdOp::INT_ADD && Left.Constant && Right.Offset)
        Value.Offset =
            static_cast<int32_t>(*Left.Constant + uint32_t(*Right.Offset));
      else if (Left.Constant && Right.Constant)
        Value.Constant = Op.Opcode == NdOp::INT_ADD
                             ? *Left.Constant + *Right.Constant
                             : *Left.Constant - *Right.Constant;
    }
    Temps[Op.Output.Offset] = Value;
  }
  return Writes;
}

} // namespace

RegistrationStateAnalysis analyzeRegistrationStates(const LowFunc &Function) {
  RegistrationStateAnalysis Result;
  if (!Function.ExceptionMetadata || !Function.ExceptionMetadata->Registration)
    return Result;
  const ExceptionFunction &EH = *Function.ExceptionMetadata;
  const RegistrationChainInfo &Chain = *EH.Registration;
  if (EH.ParseStatus != ExceptionParseStatus::Complete) {
    Result.Diagnostics.push_back("registration metadata is incomplete");
    return Result;
  }
  if (!Chain.RegistrationOffset || !Chain.TryLevelOffset ||
      !Chain.SeededTryLevel || !Chain.ChainInstallVA ||
      Chain.TryLevelStores.empty()) {
    Result.Diagnostics.push_back(
        "registration frame and state slot are not proven");
    return Result;
  }

  std::map<int, size_t> Index;
  std::map<va_t, size_t> Entries;
  for (size_t I = 0; I < Function.Blocks.size(); ++I) {
    const LowBlock &Block = Function.Blocks[I];
    if (!Index.emplace(Block.Id, I).second ||
        !Entries.emplace(Block.StartAddr, I).second ||
        Block.StartAddr >= Block.EndAddr) {
      Result.Diagnostics.push_back("invalid registration-state CFG identity");
      return Result;
    }
  }
  auto Entry = Entries.find(Function.Entry);
  if (Entry == Entries.end()) {
    Result.Diagnostics.push_back("registration-state CFG has no exact entry");
    return Result;
  }

  // Only decoded instruction boundaries can authorize a state transition.
  // A byte-scanner hit inside another instruction must not change the domain.
  std::vector<std::optional<int32_t>> Writes(Function.Blocks.size());
  std::vector<bool> InvalidWrite(Function.Blocks.size());
  std::vector<std::vector<SlotWrite>> LiftedWrites;
  std::optional<size_t> InstallBlock;
  for (size_t I = 0; I < Function.Blocks.size(); ++I) {
    const LowBlock &Block = Function.Blocks[I];
    bool InstallProven = false;
    LiftedWrites.push_back(collectSlotWrites(
        Block, *Chain.TryLevelOffset, Chain.ChainInstallVA, InstallProven));
    for (const LowInstructionBoundary &Instruction :
         Block.InstructionBoundaries)
      if (Instruction.Address == Chain.ChainInstallVA &&
          Instruction.Size == 7 &&
          Instruction.Address + Instruction.Size == Block.EndAddr &&
          InstallProven)
        InstallBlock = I;
  }
  if (!InstallBlock) {
    Result.Diagnostics.push_back(
        "registration installation is not an exact decoded boundary");
    return Result;
  }
  for (const RegistrationTryLevelStore &Store : Chain.TryLevelStores) {
    const bool ValidLevel =
        Store.Level == *Chain.SeededTryLevel ||
        (Store.Level >= 0 &&
         static_cast<size_t>(Store.Level) <
             (EH.Cxx ? EH.Cxx->MaxState : Chain.Scopes.size()));
    auto Owner = Entries.upper_bound(Store.StoreVA);
    if (Owner != Entries.begin()) {
      const size_t I = std::prev(Owner)->second;
      const LowBlock &Block = Function.Blocks[I];
      if (Store.StoreVA >= Block.EndAddr)
        continue;
      auto Boundary =
          std::find_if(Block.InstructionBoundaries.begin(),
                       Block.InstructionBoundaries.end(),
                       [&](const LowInstructionBoundary &Instruction) {
                         return Instruction.Address == Store.StoreVA &&
                                Instruction.Size == Store.EndVA - Store.StoreVA;
                       });
      if (!ValidLevel || Store.EndVA <= Store.StoreVA ||
          Store.EndVA != Block.EndAddr ||
          Boundary == Block.InstructionBoundaries.end() || Writes[I] ||
          LiftedWrites[I].size() != 1 ||
          LiftedWrites[I][0].Address != Store.StoreVA ||
          LiftedWrites[I][0].Level != Store.Level) {
        InvalidWrite[I] = true;
      } else {
        Writes[I] = Store.Level;
      }
    }
  }
  for (size_t I = 0; I < Function.Blocks.size(); ++I)
    if (!LiftedWrites[I].empty() && !Writes[I])
      InvalidWrite[I] = true;

  std::vector<Domain> Incoming(Function.Blocks.size());
  std::deque<size_t> Work;
  std::vector<bool> Queued(Function.Blocks.size());
  size_t WorkUsed = 0;
  bool Exhausted = false;
  auto Charge = [&](size_t Amount) {
    if (Amount > limits::kMaxRegistrationEHStateWork - WorkUsed) {
      Exhausted = true;
      return false;
    }
    WorkUsed += Amount;
    return true;
  };
  std::set<int32_t> AllLevels{*Chain.SeededTryLevel};
  const size_t StateCount = EH.Cxx ? EH.Cxx->MaxState : Chain.Scopes.size();
  if (StateCount > limits::kMaxRegistrationEHStateWork) {
    Result.Diagnostics.push_back(
        "registration-state count exceeds the work budget");
    return Result;
  }
  for (size_t I = 0; I < StateCount; ++I)
    AllLevels.insert(static_cast<int32_t>(I));
  auto Merge = [&](size_t Target, const Domain &Source) {
    if (Exhausted)
      return;
    if (!Charge(Source.Levels.size() + 1))
      return;
    Domain &Dest = Incoming[Target];
    const size_t Before = Dest.Levels.size();
    const bool WasUnknown = Dest.Unknown;
    const bool WasParent = Dest.Parent;
    const bool WasCallback = Dest.Callback;
    const bool WasUninstalled = Dest.Uninstalled;
    Dest.Levels.insert(Source.Levels.begin(), Source.Levels.end());
    Dest.Unknown |= Source.Unknown;
    Dest.Parent |= Source.Parent;
    Dest.Callback |= Source.Callback;
    Dest.Uninstalled |= Source.Uninstalled;
    if ((Before != Dest.Levels.size() || WasUnknown != Dest.Unknown ||
         WasParent != Dest.Parent || WasCallback != Dest.Callback ||
         WasUninstalled != Dest.Uninstalled) &&
        !Queued[Target]) {
      Work.push_back(Target);
      Queued[Target] = true;
    }
  };
  auto Dispatch = [&](va_t Address, int32_t Level, bool Unknown = false,
                      bool Callback = false) {
    auto It = Entries.find(Address);
    if (It != Entries.end())
      Merge(It->second, {{Level}, Unknown, !Callback, Callback});
  };

  Merge(Entry->second, {{}, false, true, false, true});
  while (!Work.empty() && !Exhausted) {
    size_t I = Work.front();
    Work.pop_front();
    Queued[I] = false;
    const Domain Before = Incoming[I];
    Domain After = Before;
    if (I == *InstallBlock) {
      // The prologue seed belongs to the live registration, not to the
      // instructions which construct it before FS:[0] is changed.
      After.Levels = {*Chain.SeededTryLevel};
      After.Uninstalled = false;
    } else if (InvalidWrite[I]) {
      After.Unknown = true;
      After.Levels.clear();
    } else if (Writes[I] && !Before.Uninstalled) {
      After.Levels = {*Writes[I]};
      After.Unknown = false;
    }
    for (int Successor : Function.Blocks[I].Succs) {
      auto It = Index.find(Successor);
      if (It == Index.end()) {
        Result.Diagnostics.push_back(
            "registration-state CFG has a missing edge");
        return Result;
      }
      Merge(It->second, After);
    }

    // Search filters see the searching frame's state. Once local unwind has
    // selected a scope, its handler/finally runs at the enclosing level.
    if (!Before.Parent)
      continue;
    const std::set<int32_t> &DispatchLevels =
        Before.Unknown ? AllLevels : Before.Levels;
    for (int32_t Level : DispatchLevels) {
      if (!Charge(1))
        break;
      int32_t Walk = Level;
      for (size_t Step = 0; Step < Chain.Scopes.size(); ++Step) {
        if (!Charge(1))
          break;
        if (Walk < 0 || static_cast<size_t>(Walk) >= Chain.Scopes.size())
          break;
        const RegistrationScopeRecord &Scope = Chain.Scopes[Walk];
        Dispatch(Scope.FilterVA, Level, Before.Unknown, true);
        Dispatch(Scope.HandlerVA, Scope.EnclosingLevel, Before.Unknown,
                 Scope.IsFinally);
        Walk = Scope.EnclosingLevel;
      }
      if (!EH.Cxx || Level < 0 ||
          static_cast<uint32_t>(Level) >= EH.Cxx->MaxState)
        continue;
      const CxxExceptionInfo &Cxx = *EH.Cxx;
      if (static_cast<size_t>(Level) >= Cxx.UnwindMap.size()) {
        Result.Diagnostics.push_back(
            "registration state exceeds the C++ unwind map");
        return Result;
      }
      const CxxUnwindAction &Action = Cxx.UnwindMap[Level];
      // A cleanup's nested-exception state depends on the runtime's active
      // unwind walk, not just on the caller's lexical state.
      Dispatch(Action.ActionVA, Level, true, true);
      for (const CxxTryBlock &Try : Cxx.TryBlocks) {
        if (!Charge(1))
          break;
        if (Level >= Try.TryLow && Level <= Try.TryHigh)
          for (const CxxCatchHandler &Catch : Try.Handlers)
            Dispatch(Catch.HandlerVA, Try.TryHigh + 1, Before.Unknown);
      }
    }
  }
  if (Exhausted) {
    Result.Diagnostics.push_back(
        "registration-state propagation budget exhausted");
    return Result;
  }

  Result.Complete = true;
  for (size_t I = 0; I < Function.Blocks.size(); ++I) {
    const LowBlock &Block = Function.Blocks[I];
    const Domain &State = Incoming[I];
    const bool CallbackOnly = State.Callback && !State.Parent;
    const bool Unknown = State.Unknown || (State.Callback && State.Parent) ||
                         (State.Uninstalled && !State.Levels.empty());
    const std::set<int32_t> &Levels = Unknown ? AllLevels : State.Levels;
    if (!Charge(Levels.size() + 1)) {
      Result.Complete = false;
      Result.Blocks.clear();
      Result.Diagnostics.push_back(
          "registration-state output budget exhausted");
      return Result;
    }
    Result.Blocks.push_back({Block.Id,
                             {Block.StartAddr, Block.EndAddr},
                             {Levels.begin(), Levels.end()},
                             Unknown,
                             CallbackOnly});
    if (CallbackOnly && Unknown)
      Result.CallbackStatesComplete = false;
    if (!CallbackOnly &&
        (Unknown || (InvalidWrite[I] && !State.Levels.empty()) ||
         (I == *InstallBlock && !State.Levels.empty())))
      Result.Complete = false;
  }
  if (!Result.Complete)
    Result.Diagnostics.push_back(
        "registration state has an unproven transition");
  return Result;
}

} // namespace neverd
