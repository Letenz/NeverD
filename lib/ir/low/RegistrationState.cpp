//===- RegistrationState.cpp - x86 EH reaching states ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/RegistrationState.h"

#include "RegistrationFrame.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/LowIR.h"

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <utility>

namespace neverd {
namespace {

using registration_state::FrameState;
using registration_state::FrameTransfer;
using registration_state::FrameValue;

struct Domain {
  std::set<int32_t> Levels;
  FrameState Frame;
  bool Reached = false;
  bool Unknown = false;
  bool Parent = false;
  bool Callback = false;
  bool Uninstalled = false;
  bool Installed = false;
  bool CanDispatch = false;
};

struct BlockFacts {
  bool Invalid = false;
  bool InstalledAtExit = false;
};

bool overlaps(int32_t Offset, uint16_t Width, int32_t Slot,
              uint16_t SlotWidth) {
  return int64_t(Offset) < int64_t(Slot) + SlotWidth &&
         int64_t(Slot) < int64_t(Offset) + Width;
}

} // namespace

RegistrationStateAnalysis analyzeRegistrationStates(const LowFunc &Function) {
  RegistrationStateAnalysis Result;
  if (!Function.ExceptionMetadata || !Function.ExceptionMetadata->Registration)
    return Result;
  const ExceptionFunction &EH = *Function.ExceptionMetadata;
  const RegistrationChainInfo &Chain = *EH.Registration;
  const bool KnownSEH =
      (EH.Personality == ExceptionPersonality::ExceptHandler3 &&
       EH.Encoding == ExceptionEncoding::X86ScopeTableEH3) ||
      (EH.Personality == ExceptionPersonality::ExceptHandler4 &&
       EH.Encoding == ExceptionEncoding::X86ScopeTableEH4);
  const bool KnownCxx =
      EH.Encoding == ExceptionEncoding::X86CxxFuncInfo &&
      (EH.Personality == ExceptionPersonality::CxxFrameHandlerX86 ||
       EH.Personality == ExceptionPersonality::CxxFrameHandler3);
  if ((!KnownSEH && !KnownCxx) || (KnownCxx != EH.Cxx.has_value())) {
    Result.Diagnostics.push_back(
        "registration language-handler semantics are not proven");
    return Result;
  }
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

  std::vector<BlockFacts> Facts(Function.Blocks.size());
  std::map<va_t, RegistrationTryLevelStore> Stores;
  const size_t StateCount = EH.Cxx ? EH.Cxx->MaxState : Chain.Scopes.size();
  if (StateCount > limits::kMaxRegistrationEHRecords) {
    Result.Diagnostics.push_back(
        "registration-state count exceeds the work budget");
    return Result;
  }
  std::set<int32_t> AllLevels{*Chain.SeededTryLevel};
  for (size_t I = 0; I < StateCount; ++I)
    AllLevels.insert(static_cast<int32_t>(I));
  auto ValidLevel = [&](int32_t Level) { return AllLevels.count(Level) != 0; };
  // Byte observations are not transfers until both their instruction and the
  // lifted address/value have been authenticated.
  for (const RegistrationTryLevelStore &Store : Chain.TryLevelStores) {
    auto Owner = Entries.upper_bound(Store.StoreVA);
    if (Owner == Entries.begin())
      continue;
    const size_t I = std::prev(Owner)->second;
    const LowBlock &Block = Function.Blocks[I];
    if (Store.StoreVA >= Block.EndAddr)
      continue;
    auto Boundary = std::find_if(
        Block.InstructionBoundaries.begin(), Block.InstructionBoundaries.end(),
        [&](const LowInstructionBoundary &Instruction) {
          return Instruction.Address == Store.StoreVA &&
                 Store.EndVA > Store.StoreVA &&
                 Instruction.Size == Store.EndVA - Store.StoreVA;
        });
    if (!ValidLevel(Store.Level) || Store.EndVA != Block.EndAddr ||
        Boundary == Block.InstructionBoundaries.end() ||
        !Stores.emplace(Store.StoreVA, Store).second)
      Facts[I].Invalid = true;
  }
  bool HasInstallBoundary = false;
  for (const LowBlock &Block : Function.Blocks)
    for (const LowInstructionBoundary &Instruction :
         Block.InstructionBoundaries)
      HasInstallBoundary |=
          Instruction.Address == Chain.ChainInstallVA &&
          Instruction.Size == 7 &&
          Instruction.Address + Instruction.Size == Block.EndAddr;
  if (!HasInstallBoundary) {
    Result.Diagnostics.push_back(
        "registration installation is not an exact decoded boundary");
    return Result;
  }

  std::vector<Domain> Incoming(Function.Blocks.size());
  bool ProvenInstallation = false;
  bool CompleteChainOperations = true;
  std::map<std::pair<va_t, int>, RegistrationChainAccess> ChainAccesses;
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
  std::map<va_t, std::pair<int, LowInstructionBoundary>> Boundaries;
  std::set<std::pair<va_t, int>> ChainOccurrences;
  for (const LowBlock &Block : Function.Blocks) {
    for (const LowInstructionBoundary &Boundary : Block.InstructionBoundaries) {
      if (!Charge(1))
        break;
      if (Boundary.Size == 0 || Boundary.Address < Block.StartAddr ||
          Boundary.Address >= Block.EndAddr ||
          Boundary.Size > Block.EndAddr - Boundary.Address ||
          !Boundaries
               .emplace(Boundary.Address, std::make_pair(Block.Id, Boundary))
               .second)
        CompleteChainOperations = false;
    }
    for (const LowOp &Op : Block.Ops) {
      if (!Charge(1))
        break;
      if (Op.MemoryAddressSpace == NdMemoryAddressSpace::X86FS &&
          (Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) &&
          (Op.Seq < 0 || !ChainOccurrences.emplace(Op.Addr, Op.Seq).second))
        CompleteChainOperations = false;
    }
    if (Exhausted)
      break;
  }
  auto RecordChainAccess = [&](const LowOp &Op, const LowBlock &Block,
                               RegistrationChainAccess::Kind Kind) {
    if (!Charge(1))
      return;
    auto Boundary = Boundaries.find(Op.Addr);
    if (Op.Seq < 0 || Boundary == Boundaries.end() ||
        Boundary->second.first != Block.Id) {
      CompleteChainOperations = false;
      return;
    }
    const RegistrationChainAccess Access{
        Op.Addr, Boundary->second.second.Address + Boundary->second.second.Size,
        Op.Seq, Kind};
    auto [It, Inserted] =
        ChainAccesses.emplace(std::make_pair(Op.Addr, Op.Seq), Access);
    if (!Inserted && (It->second.AccessKind != Kind ||
                      It->second.EndAddress != Access.EndAddress))
      CompleteChainOperations = false;
  };
  auto Merge = [&](size_t Target, const Domain &Source) {
    if (Exhausted || !Charge(Source.Levels.size() + Source.Frame.Cells.size() +
                             Incoming[Target].Frame.Cells.size() + 9))
      return;
    Domain &Dest = Incoming[Target];
    bool Changed = !Dest.Reached;
    if (!Dest.Reached) {
      Dest = Source;
      Dest.Reached = true;
    } else {
      const size_t Before = Dest.Levels.size();
      Dest.Levels.insert(Source.Levels.begin(), Source.Levels.end());
      Changed |= Before != Dest.Levels.size();
      Changed |= Dest.Frame.merge(Source.Frame);
      auto MergeFlag = [&](bool &Value, bool Other) {
        Changed |= Other && !Value;
        Value |= Other;
      };
      MergeFlag(Dest.Unknown, Source.Unknown);
      MergeFlag(Dest.Parent, Source.Parent);
      MergeFlag(Dest.Callback, Source.Callback);
      MergeFlag(Dest.Uninstalled, Source.Uninstalled);
      MergeFlag(Dest.Installed, Source.Installed);
      MergeFlag(Dest.CanDispatch, Source.CanDispatch);
    }
    if (Changed && !Queued[Target]) {
      Work.push_back(Target);
      Queued[Target] = true;
    }
  };
  auto Dispatch = [&](va_t Address, int32_t Level, const Domain &Source,
                      bool Unknown = false, bool Callback = false,
                      bool SearchFilter = false) {
    auto It = Entries.find(Address);
    if (It == Entries.end())
      return;
    Domain Root;
    Root.Levels = {Level};
    Root.Frame.Cells = Source.Frame.Cells;
    Root.Frame.Cells[*Chain.TryLevelOffset] =
        FrameValue::constant(uint32_t(Level));
    Root.Frame.Registers[x86reg::RBP / x86reg::GeneralRegStride] =
        FrameValue::frame(0);
    Root.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride].MayBeFrame =
        true;
    Root.Unknown = Unknown || Source.Unknown;
    Root.Parent = !Callback;
    Root.Callback = Callback;
    Root.Installed = true;
    Root.CanDispatch = !SearchFilter;
    Merge(It->second, Root);
  };

  Domain Initial;
  // After push EBP, the canonical direct prologue establishes EBP at this
  // incoming SP minus four bytes. Do not assume later reads still name it.
  Initial.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
      FrameValue::frame(4);
  Initial.Parent = Initial.Uninstalled = Initial.CanDispatch = true;
  Merge(Entry->second, Initial);
  for (va_t RootVA : Function.OrdinaryModuleAnalysisRoots) {
    if (RootVA == Function.Entry)
      continue;
    auto It = Entries.find(RootVA);
    if (It == Entries.end())
      continue;
    Domain Root;
    Root.Parent = Root.CanDispatch = Root.Unknown = true;
    Root.Installed = Root.Uninstalled = true;
    for (FrameValue &Register : Root.Frame.Registers)
      Register.MayBeFrame = true;
    Merge(It->second, Root);
  }

  while (!Work.empty() && !Exhausted) {
    const size_t I = Work.front();
    Work.pop_front();
    Queued[I] = false;
    const LowBlock &Block = Function.Blocks[I];
    const Domain Before = Incoming[I];
    Domain After = Before;
    FrameTransfer Transfer(After.Frame, *Chain.RegistrationOffset);
    std::set<va_t> ProvenStores;
    auto Invalidate = [&] {
      Facts[I].Invalid = true;
      After.Unknown = true;
      After.Levels.clear();
    };
    for (const LowOp &Op : Block.Ops) {
      // Ambiguous frame loads inspect every tracked cell. Charge that work,
      // not just the number of lifted operations, before entering the scan.
      if (!Charge(1 + (Op.Opcode == NdOp::LOAD ? After.Frame.Cells.size() : 0)))
        break;
      Transfer.beginInstruction(Op.Addr);
      const FrameValue Value = Transfer.evaluate(Op, After.Installed);
      if (Op.Opcode == NdOp::LOAD && Op.NumInputs == 1 &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::X86FS) {
        const FrameValue Address = Transfer.read(Op.Inputs[0]);
        if (!Address.Constant ||
            overlaps(int32_t(*Address.Constant), Op.Output.Size, 0, 4)) {
          if (Address.Constant == uint32_t{0} && Op.Output.Size == 4 &&
              After.Installed != After.Uninstalled)
            RecordChainAccess(
                Op, Block,
                After.Installed
                    ? RegistrationChainAccess::Kind::ReadInstalledHead
                    : RegistrationChainAccess::Kind::ReadPreviousHead);
          else
            CompleteChainOperations = false;
        }
      }
      if (Op.Opcode == NdOp::STORE && Op.NumInputs == 2) {
        const FrameValue Address = Transfer.read(Op.Inputs[0]);
        const FrameValue Stored = Transfer.read(Op.Inputs[1]);
        const uint16_t Width = Op.Inputs[1].Size;
        if (Op.MemoryAddressSpace == NdMemoryAddressSpace::X86FS &&
            (!Address.Constant ||
             overlaps(int32_t(*Address.Constant), Width, 0, 4))) {
          auto Boundary =
              std::find_if(Block.InstructionBoundaries.begin(),
                           Block.InstructionBoundaries.end(),
                           [&](const LowInstructionBoundary &Instruction) {
                             return Instruction.Address == Op.Addr;
                           });
          const bool AtEnd =
              Boundary != Block.InstructionBoundaries.end() &&
              Boundary->Address + Boundary->Size == Block.EndAddr;
          if (Address.Constant == uint32_t{0} && Width == 4 && AtEnd &&
              Op.Addr == Chain.ChainInstallVA &&
              Stored.Offset == Chain.RegistrationOffset && After.Uninstalled &&
              !After.Installed) {
            After.Levels = {*Chain.SeededTryLevel};
            After.Unknown = false;
            After.Uninstalled = false;
            After.Installed = true;
            ProvenInstallation = true;
            RecordChainAccess(Op, Block,
                              RegistrationChainAccess::Kind::Install);
          } else if (Address.Constant == uint32_t{0} && Width == 4 && AtEnd &&
                     Stored.PreviousChain && After.Installed &&
                     !After.Uninstalled) {
            After.Levels.clear();
            After.Unknown = false;
            After.Uninstalled = true;
            After.Installed = false;
            RecordChainAccess(Op, Block, RegistrationChainAccess::Kind::Remove);
          } else {
            CompleteChainOperations = false;
            Invalidate();
            After.Installed = After.Uninstalled = true;
          }
        } else if (Op.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
          if (After.Installed && Address.MayBeFrame &&
              (!Address.Offset ||
               overlaps(*Address.Offset, Width, *Chain.TryLevelOffset, 4))) {
            auto Observation = Stores.find(Op.Addr);
            if (Address.Offset == Chain.TryLevelOffset && Width == 4 &&
                Stored.Constant && ValidLevel(int32_t(*Stored.Constant)) &&
                Observation != Stores.end() &&
                Observation->second.Level == int32_t(*Stored.Constant) &&
                !After.Uninstalled && ProvenStores.insert(Op.Addr).second) {
              After.Levels = {int32_t(*Stored.Constant)};
              After.Unknown = false;
            } else
              Invalidate();
          }
          if (After.Installed && Address.Offset &&
              overlaps(
                  *Address.Offset, Width, *Chain.RegistrationOffset,
                  uint16_t(*Chain.TryLevelOffset - *Chain.RegistrationOffset)))
            Invalidate();
          if (Address.Offset) {
            if (!Charge(After.Frame.Cells.size() + 1))
              break;
            After.Frame.store(*Address.Offset, Width, Stored);
          } else if (Stored.MayBeFrame) {
            // A frame pointer escaped to storage whose future aliases cannot
            // be bounded by this frame-value domain.
            Invalidate();
          }
        }
      }
      Transfer.write(Op, Value);
    }
    for (auto It = Stores.lower_bound(Block.StartAddr);
         It != Stores.end() && It->first < Block.EndAddr; ++It)
      if (!ProvenStores.count(It->first))
        Invalidate();
    if (Facts[I].Invalid)
      After.Unknown = true;
    Facts[I].InstalledAtExit = After.Installed;
    for (int Successor : Block.Succs) {
      auto It = Index.find(Successor);
      if (It == Index.end()) {
        Result.Diagnostics.push_back(
            "registration-state CFG has a missing edge");
        return Result;
      }
      Merge(It->second, After);
    }

    // A filter is a searching callback, while a finally is an unwind callback
    // at its enclosing state. Lexical callback exclusion must not erase an
    // outer exception dispatch when that finally itself faults.
    if (!Before.CanDispatch || !Before.Installed)
      continue;
    const std::set<int32_t> &DispatchLevels =
        Before.Unknown || Facts[I].Invalid ? AllLevels : Before.Levels;
    for (int32_t Level : DispatchLevels) {
      if (!Charge(1))
        break;
      int32_t Walk = Level;
      for (size_t Step = 0; Step < Chain.Scopes.size(); ++Step) {
        if (!Charge(1) || Walk < 0 ||
            static_cast<size_t>(Walk) >= Chain.Scopes.size())
          break;
        const RegistrationScopeRecord &Scope = Chain.Scopes[Walk];
        Dispatch(Scope.FilterVA, Level, Before, Facts[I].Invalid, true, true);
        Dispatch(Scope.HandlerVA, Scope.EnclosingLevel, Before,
                 Facts[I].Invalid, Scope.IsFinally);
        Walk = Scope.EnclosingLevel;
      }
      if (!EH.Cxx || Level < 0 ||
          static_cast<uint32_t>(Level) >= EH.Cxx->MaxState)
        continue;
      const CxxExceptionInfo &Cxx = *EH.Cxx;
      Walk = Level;
      for (size_t Step = 0; Step < Cxx.UnwindMap.size(); ++Step) {
        if (!Charge(1) || Walk < 0 ||
            static_cast<size_t>(Walk) >= Cxx.UnwindMap.size())
          break;
        const CxxUnwindAction &Action = Cxx.UnwindMap[Walk];
        Dispatch(Action.ActionVA, Action.ToState, Before, true, true);
        Walk = Action.ToState;
      }
      for (const CxxTryBlock &Try : Cxx.TryBlocks) {
        if (!Charge(1))
          break;
        if (Level >= Try.TryLow && Level <= Try.TryHigh)
          for (const CxxCatchHandler &Catch : Try.Handlers)
            Dispatch(Catch.HandlerVA, Try.TryHigh + 1, Before,
                     Facts[I].Invalid);
      }
    }
  }
  if (Exhausted) {
    Result.Diagnostics.push_back(
        "registration-state propagation budget exhausted");
    return Result;
  }
  if (!ProvenInstallation) {
    Result.Diagnostics.push_back(
        "registration installation was not proven by the lifted code");
    return Result;
  }

  Result.Complete = Result.RegistrationLifetimeComplete = true;
  for (size_t I = 0; I < Function.Blocks.size(); ++I) {
    const LowBlock &Block = Function.Blocks[I];
    const Domain &State = Incoming[I];
    const bool CallbackOnly = State.Callback && !State.Parent;
    const bool Unknown =
        State.Reached && (State.Unknown || Facts[I].Invalid ||
                          (State.Callback && State.Parent) ||
                          (State.Uninstalled && State.Installed));
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
                             CallbackOnly,
                             State.CanDispatch && State.Installed});
    if (CallbackOnly && Unknown)
      Result.CallbackStatesComplete = false;
    if (!CallbackOnly && Unknown)
      Result.Complete = false;
    if (State.Parent && State.Reached &&
        (Unknown || (Block.Succs.empty() && Facts[I].InstalledAtExit)))
      Result.RegistrationLifetimeComplete = false;
  }
  if (!Result.Complete)
    Result.Diagnostics.push_back(
        "registration state has an unproven transition");
  Result.ChainOperationsComplete = CompleteChainOperations && Result.Complete &&
                                   Result.CallbackStatesComplete &&
                                   Result.RegistrationLifetimeComplete;
  if (Result.ChainOperationsComplete)
    for (const auto &[Identity, Access] : ChainAccesses)
      Result.ChainAccesses.push_back(Access);
  return Result;
}

} // namespace neverd
