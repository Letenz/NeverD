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
#include <tuple>
#include <utility>

namespace neverd {
namespace {

using registration_state::FrameState;
using registration_state::FrameTransfer;
using registration_state::FrameValue;

struct Domain {
  std::set<int32_t> Levels;
  FrameState Frame;
  std::set<int32_t> InitializedFrameBytes;
  bool Reached = false;
  bool Unknown = false;
  bool Parent = false;
  bool Callback = false;
  bool OtherCallback = false;
  using CatchStack = std::vector<std::pair<uint32_t, uint32_t>>;
  std::set<CatchStack> CxxCatchStacks;
  bool Uninstalled = false;
  bool Installed = false;
  bool CanDispatch = false;
};

struct BlockFacts {
  bool Invalid = false;
  bool InstalledAtExit = false;
  bool CxxContinuationAtExit = false;
  bool NoReturnAtExit = false;
};

int32_t cxxMinimumTryLevel(const Domain &State, const CxxExceptionInfo &Cxx) {
  if (State.Parent || State.OtherCallback || State.CxxCatchStacks.empty())
    return 0;
  int32_t Minimum = INT32_MAX;
  for (const auto &Stack : State.CxxCatchStacks)
    Minimum = std::min(Minimum, Cxx.TryBlocks[Stack.back().first].TryHigh + 1);
  return Minimum;
}

bool overlaps(int32_t Offset, uint16_t Width, int32_t Slot,
              uint16_t SlotWidth) {
  return int64_t(Offset) < int64_t(Slot) + SlotWidth &&
         int64_t(Slot) < int64_t(Offset) + Width;
}

} // namespace

RegistrationStateAnalysis analyzeRegistrationStates(
    const LowFunc &Function, va_t SecurityCookieVA, va_t CookieCheckVA,
    const std::vector<RegistrationCalleeFrameContract> *Callees,
    const std::vector<RegistrationCleanupFrameContract> *Cleanups) {
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
  Result.CxxContinuationsComplete = !KnownCxx;
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
  if (EH.Cxx) {
    size_t Remaining = limits::kMaxRegistrationEHRecords - StateCount;
    if (EH.Cxx->TryBlocks.size() > Remaining) {
      Result.Diagnostics.push_back("C++ registration graph exceeds the budget");
      return Result;
    }
    Remaining -= EH.Cxx->TryBlocks.size();
    for (const CxxTryBlock &Try : EH.Cxx->TryBlocks) {
      if (Try.Handlers.size() > Remaining) {
        Result.Diagnostics.push_back(
            "C++ registration graph exceeds the budget");
        return Result;
      }
      Remaining -= Try.Handlers.size();
    }
    if (!EH.Cxx->hasValidStateGraph()) {
      Result.Diagnostics.push_back("C++ registration state graph is invalid");
      return Result;
    }
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
    const bool ValidImmediate =
        Store.Width == 4   ? ValidLevel(Store.Level)
        : Store.Width == 1 ? uint32_t(Store.Level) <= UINT8_MAX
        : Store.Width == 2 ? uint32_t(Store.Level) <= UINT16_MAX
                           : false;
    if (!ValidImmediate || Store.EndVA != Block.EndAddr ||
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
  const bool CheckCalls = Callees != nullptr;
  std::map<va_t, uint32_t> CalleeIndices;
  auto ValidObjects = [&](const auto &Ranges) {
    int32_t PreviousEnd = 0;
    for (const auto &Range : Ranges) {
      if (!Charge(1) || Range.Begin < PreviousEnd || Range.End <= Range.Begin ||
          Range.End > int64_t(limits::kMaxRegistrationEHStateWork))
        return false;
      PreviousEnd = Range.End;
    }
    return true;
  };
  auto ValidImageRanges = [&](const auto &Ranges) {
    va_t PreviousEnd = 0;
    for (const auto &Range : Ranges) {
      if (!Charge(1) || !Range.isValid() || Range.Begin < PreviousEnd ||
          Range.End > uint64_t(UINT32_MAX) + 1)
        return false;
      PreviousEnd = Range.End;
    }
    return true;
  };
  if (CheckCalls) {
    if (Callees->size() > 256) {
      Result.Diagnostics.push_back("registration callee count exceeds budget");
      return Result;
    }
    for (const auto &Callee : *Callees) {
      const bool Leaf =
          Callee.CalleeKind == RegistrationCalleeFrameContract::Kind::Leaf;
      const bool Throw = Callee.CalleeKind ==
                         RegistrationCalleeFrameContract::Kind::PrivateThrow;
      if (!Charge(1) || !Callee.Target || Callee.Target > UINT32_MAX ||
          Callee.StackPopBytes || (!Leaf && !Throw) ||
          Callee.DoesNotReturn != Throw ||
          (Leaf && (Callee.ThrownTypeVA || Callee.ThrownObjectSize)) ||
          (Throw &&
           (!Callee.ThrownTypeVA || Callee.ThrownTypeVA > UINT32_MAX ||
            !Callee.ThrownObjectSize ||
            Callee.ThrownObjectSize > limits::kMaxRegistrationEHStateWork ||
            !Callee.ECXReads.empty() || !Callee.ECXWrites.empty())) ||
          !ValidObjects(Callee.ECXReads) || !ValidObjects(Callee.ECXWrites) ||
          !ValidImageRanges(Callee.ImageReads) ||
          !ValidImageRanges(Callee.ImageWrites) ||
          !ValidImageRanges(Callee.CallerPCWrites) ||
          !CalleeIndices.emplace(Callee.Target, CalleeIndices.size()).second) {
        Result.Diagnostics.push_back("registration callee contract is invalid");
        return Result;
      }
    }
    Result.CalleeContracts = *Callees;
  }
  const bool CheckCleanups = KnownCxx && Cleanups != nullptr;
  std::map<uint32_t, uint32_t> CleanupIndices;
  if (CheckCleanups) {
    if (Cleanups->size() > limits::kMaxRegistrationEHRecords) {
      Result.Diagnostics.push_back("registration cleanup count exceeds budget");
      return Result;
    }
    for (const auto &Cleanup : *Cleanups) {
      const auto &Leaf = Cleanup.Leaf;
      if (!Charge(1) || Cleanup.ActionState >= EH.Cxx->UnwindMap.size() ||
          EH.Cxx->UnwindMap[Cleanup.ActionState].Kind !=
              CxxUnwindAction::ActionKind::Direct ||
          EH.Cxx->UnwindMap[Cleanup.ActionState].ObjectOffset ||
          Cleanup.RelayTarget !=
              EH.Cxx->UnwindMap[Cleanup.ActionState].ActionVA ||
          !Cleanup.RelayTarget || Cleanup.RelayTarget > UINT32_MAX ||
          !Leaf.Target || Leaf.Target > UINT32_MAX ||
          Leaf.CalleeKind != RegistrationCalleeFrameContract::Kind::Leaf ||
          Leaf.StackPopBytes || Leaf.DoesNotReturn || Leaf.ThrownTypeVA ||
          Leaf.ThrownObjectSize || !Leaf.CallerPCWrites.empty() ||
          !ValidObjects(Leaf.ECXReads) || !ValidObjects(Leaf.ECXWrites) ||
          !ValidImageRanges(Leaf.ImageReads) ||
          !ValidImageRanges(Leaf.ImageWrites) ||
          !CleanupIndices.emplace(Cleanup.ActionState, CleanupIndices.size())
               .second) {
        Result.Diagnostics.push_back(
            "registration cleanup contract is invalid");
        return Result;
      }
    }
    Result.CleanupContracts = *Cleanups;
  }
  auto ProjectFrameObject = [&](const Domain &State, int32_t Object,
                                std::optional<int32_t> SP, const auto &Extents,
                                auto &Projected, bool Reads) {
    if (!SP || State.Unknown)
      return false;
    for (const auto &Extent : Extents) {
      if (!Charge(1))
        return false;
      const int64_t Begin = int64_t(Object) + Extent.Begin;
      const int64_t End = int64_t(Object) + Extent.End;
      // Registration/SavedESP, saved EBP and the caller PC cannot be objects.
      if (Begin < *SP ||
          Begin < -int64_t(limits::kMaxRegistrationEHStateWork) || End > 0 ||
          (Begin < int64_t(*Chain.TryLevelOffset) + 4 &&
           int64_t(*Chain.RegistrationOffset) - (KnownCxx ? 4 : 0) < End) ||
          !Charge(size_t(End - Begin) + State.Frame.Cells.size()))
        return false;
      if (Reads) {
        for (int64_t Byte = Begin; Byte < End; ++Byte)
          if (!State.InitializedFrameBytes.count(int32_t(Byte)))
            return false;
        for (const auto &[Cell, Value] : State.Frame.Cells)
          if (Value.MayBeFrame && int64_t(Cell) < End &&
              Begin < int64_t(Cell) + 4)
            return false;
      }
      Projected.push_back({int32_t(Begin), int32_t(End)});
    }
    return true;
  };
  std::map<va_t, std::pair<int, LowInstructionBoundary>> Boundaries;
  std::set<std::pair<va_t, int>> ChainOccurrences;
  std::map<std::pair<va_t, int>, FrameValue> FrameValues;
  std::map<std::pair<va_t, int>, RegistrationCxxContinuation> CxxContinuations;
  std::set<std::pair<va_t, int>> InvalidCxxContinuations;
  bool CompleteCxxContinuations = true;
  std::map<std::pair<va_t, int>, std::optional<RegistrationIncomingFrameAccess>>
      IncomingAccesses;
  bool ConsistentIncomingAccesses = true;
  bool CompleteImageReads = true;
  bool CompleteCalls = CheckCalls;
  std::map<std::pair<va_t, int>, RegistrationCallFrameEffect> CallEffects;
  std::set<std::pair<va_t, int>> InvalidCalls;
  bool CompleteCleanups = CheckCleanups;
  using CleanupKey = std::tuple<int, int32_t, uint32_t>;
  std::map<CleanupKey, RegistrationCleanupFrameEffect> CleanupEffects;
  std::set<CleanupKey> InvalidCleanups;
  std::set<std::pair<va_t, va_t>> ImageReads;
  const bool EH4 = EH.Personality == ExceptionPersonality::ExceptHandler4;
  bool CompleteCookies =
      EH4 && SecurityCookieVA && SecurityCookieVA <= UINT32_MAX - 3 &&
      Chain.ScopeTableVA <= UINT32_MAX - 16 && Chain.EHCookieOffset < -2;
  auto CookieSlot = [&](int32_t Displacement) -> std::optional<int32_t> {
    const int64_t Offset =
        int64_t(*Chain.RegistrationOffset) + 16 + Displacement;
    if (Offset < INT32_MIN || Offset > INT32_MAX ||
        Offset + 4 > int64_t(*Chain.RegistrationOffset) - 8)
      return std::nullopt;
    return int32_t(Offset);
  };
  const auto EHCookieSlot = CookieSlot(Chain.EHCookieOffset);
  const auto GSCookieSlot = Chain.GSCookieOffset != -2
                                ? CookieSlot(Chain.GSCookieOffset)
                                : std::optional<int32_t>{};
  if (!EH4 || !GSCookieSlot || CookieCheckVA > UINT32_MAX)
    CookieCheckVA = 0;
  std::map<va_t, int> CookieCheckOccurrences;
  std::map<std::pair<va_t, int>, RegistrationCookieCheck> CookieChecks;
  bool ReadsGSCookie = false;
  CompleteCookies &= EHCookieSlot.has_value() &&
                     (Chain.GSCookieOffset == -2 || GSCookieSlot.has_value());
  auto CookiesReady = [&](const FrameState &Frame) {
    const auto SP =
        Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset;
    auto Matches = [&](std::optional<int32_t> Slot, int32_t XOROffset) {
      if (!Slot || !SP || *Slot < *SP)
        return false;
      const int64_t ExpectedFrame =
          int64_t(*Chain.RegistrationOffset) + 16 + XOROffset;
      const auto It = Frame.Cells.find(*Slot);
      return ExpectedFrame >= INT32_MIN && ExpectedFrame <= INT32_MAX &&
             It != Frame.Cells.end() && It->second.SecurityCookie &&
             It->second.CookieFrameOffset == int32_t(ExpectedFrame) &&
             It->second.CookieXOR == 0;
    };
    const auto Table = Frame.Cells.find(*Chain.RegistrationOffset + 8);
    return Table != Frame.Cells.end() && Table->second.SecurityCookie &&
           !Table->second.CookieFrameOffset &&
           Table->second.CookieXOR == Chain.ScopeTableVA &&
           Matches(EHCookieSlot, Chain.EHCookieXOROffset) &&
           (Chain.GSCookieOffset == -2 ||
            Matches(GSCookieSlot, Chain.GSCookieXOROffset));
  };
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
      const auto Memory = lowMemoryOperands(Op);
      if (CookieCheckVA && Op.Opcode == NdOp::CALL && Op.NumInputs == 1 &&
          Op.Inputs[0].isConst() && Op.Inputs[0].Offset == CookieCheckVA &&
          !CookieCheckOccurrences.emplace(Op.Addr, Op.Seq).second)
        CompleteCookies = false;
      if (Op.MemoryAddressSpace == NdMemoryAddressSpace::X86FS) {
        if (!Memory.Complete ||
            (Op.Opcode != NdOp::LOAD && Op.Opcode != NdOp::STORE) ||
            Op.Seq < 0 || !ChainOccurrences.emplace(Op.Addr, Op.Seq).second)
          CompleteChainOperations = false;
      }
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
    if (Exhausted ||
        !Charge(Source.Levels.size() + Source.Frame.Cells.size() +
                Source.Frame.OtherRegisterBytes.size() +
                Incoming[Target].Frame.Cells.size() +
                Incoming[Target].Frame.OtherRegisterBytes.size() +
                Source.CxxCatchStacks.size() +
                Incoming[Target].CxxCatchStacks.size() +
                Source.InitializedFrameBytes.size() +
                Incoming[Target].InitializedFrameBytes.size() + 10))
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
      for (auto It = Dest.InitializedFrameBytes.begin();
           It != Dest.InitializedFrameBytes.end();)
        if (!Source.InitializedFrameBytes.count(*It)) {
          It = Dest.InitializedFrameBytes.erase(It);
          Changed = true;
        } else
          ++It;
      auto MergeFlag = [&](bool &Value, bool Other) {
        Changed |= Other && !Value;
        Value |= Other;
      };
      MergeFlag(Dest.Unknown, Source.Unknown);
      MergeFlag(Dest.Parent, Source.Parent);
      MergeFlag(Dest.Callback, Source.Callback);
      MergeFlag(Dest.OtherCallback, Source.OtherCallback);
      for (const auto &Stack : Source.CxxCatchStacks) {
        if (!Charge(Stack.size() + 1))
          return;
        Changed |= Dest.CxxCatchStacks.insert(Stack).second;
      }
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
                      bool SearchFilter = false,
                      std::optional<std::pair<uint32_t, uint32_t>> CxxCatch =
                          std::nullopt) {
    auto It = Entries.find(Address);
    if (It == Entries.end()) {
      if (CxxCatch)
        CompleteCxxContinuations = false;
      return;
    }
    Domain Root;
    Root.Levels = {Level};
    for (auto &Register : Root.Frame.Registers)
      Register.MayBeFrame = true;
    Root.Frame.OtherRegistersMayBeFrame = true;
    Root.Frame.Cells = Source.Frame.Cells;
    Root.InitializedFrameBytes = Source.InitializedFrameBytes;
    Root.Frame.Cells[*Chain.TryLevelOffset] =
        FrameValue::constant(uint32_t(Level));
    Root.Frame.Registers[x86reg::RBP / x86reg::GeneralRegStride] =
        FrameValue::frame(0);
    Root.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride].MayBeFrame =
        true;
    Root.Unknown = Unknown || Source.Unknown;
    Root.Parent = !Callback;
    Root.Callback = Callback;
    Root.OtherCallback = Callback && !CxxCatch;
    if (CxxCatch) {
      if (Source.Parent && !Source.Callback)
        Root.CxxCatchStacks.insert({*CxxCatch});
      else if (!Source.Parent && Source.Callback && !Source.OtherCallback &&
               !Source.CxxCatchStacks.empty()) {
        for (auto Stack : Source.CxxCatchStacks) {
          if (!Charge(Stack.size() + 1))
            return;
          Stack.push_back(*CxxCatch);
          Root.CxxCatchStacks.insert(std::move(Stack));
        }
      } else {
        Root.CxxCatchStacks.insert({*CxxCatch});
        Root.Unknown = true;
      }
      // Unwinding and catch-object construction may change local objects.
      // Keep only frame taint for their old values. Runtime administration is
      // separate: the link word and SavedESP remain owned by the EH frame.
      for (auto &[Offset, Value] : Root.Frame.Cells)
        if (Offset != *Chain.RegistrationOffset &&
            int64_t(Offset) != int64_t(*Chain.RegistrationOffset) - 4 &&
            Offset != *Chain.TryLevelOffset)
          Value = registration_state::join(Value, {});
    }
    Root.Installed = true;
    Root.CanDispatch = !SearchFilter;
    Merge(It->second, Root);
  };

  Domain Initial;
  // No register-parameter convention has been proved for this PE32 entry.
  // Retain opaque incoming values (including saved caller EBP) until an
  // explicit source definition replaces them; native emission may not seed
  // them with zero and then expose the invented value.
  for (auto &Register : Initial.Frame.Registers)
    Register.MayBeFrame = true;
  Initial.Frame.OtherRegistersMayBeFrame = true;
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
    if (!Charge(Before.CxxCatchStacks.size() + 1))
      break;
    const int32_t MinimumTry = EH.Cxx ? cxxMinimumTryLevel(Before, *EH.Cxx) : 0;
    Domain After = Before;
    FrameTransfer Transfer(After.Frame, *Chain.RegistrationOffset,
                           EH4 ? SecurityCookieVA : 0);
    std::set<va_t> ProvenStores;
    std::optional<RegistrationCxxContinuation> CatchReturn;
    bool NoReturnAtExit = false;
    va_t NoReturnEnd = InvalidVA;
    auto Invalidate = [&] {
      Facts[I].Invalid = true;
      After.Unknown = true;
      After.Levels.clear();
    };
    for (const LowOp &Op : Block.Ops) {
      // Ambiguous frame loads inspect every tracked cell. Charge that work,
      // not just the number of lifted operations, before entering the scan.
      size_t RegisterBytes = Op.Output.Size;
      for (unsigned Input = 0; Input != Op.NumInputs; ++Input)
        RegisterBytes += Op.Inputs[Input].Size;
      if (!Charge(1 + RegisterBytes +
                  ((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE)
                       ? After.Frame.Cells.size()
                       : 0)))
        break;
      Transfer.beginInstruction(Op.Addr);
      const FrameValue Value = Transfer.evaluate(Op, After.Installed);
      if (KnownCxx && Op.Opcode == NdOp::RETURN &&
          !After.CxxCatchStacks.empty()) {
        const auto Identity = std::make_pair(Op.Addr, Op.Seq);
        const auto Boundary = Boundaries.find(Op.Addr);
        const FrameValue Target =
            Op.NumInputs == 1 ? Transfer.read(Op.Inputs[0]) : FrameValue{};
        const int64_t SavedSlot = int64_t(*Chain.RegistrationOffset) - 4;
        const auto SavedSP = SavedSlot >= INT32_MIN
                                 ? After.Frame.Cells.find(int32_t(SavedSlot))
                                 : After.Frame.Cells.end();
        const bool Valid =
            After.CxxCatchStacks.size() == 1 && !After.Parent &&
            !After.OtherCallback && !After.Unknown && !Facts[I].Invalid &&
            After.Installed && !After.Uninstalled && !After.Levels.empty() &&
            Op.Seq >= 0 && Op.NumInputs == 1 && Op.Inputs[0].Size == 4 &&
            &Op == &Block.Ops.back() && Block.Succs.empty() &&
            Target.Constant && !Target.MayBeFrame &&
            EH.CodeRange.contains(*Target.Constant) &&
            *Target.Constant != Function.Entry &&
            SavedSP != After.Frame.Cells.end() && SavedSP->second.Offset &&
            *SavedSP->second.Offset <= SavedSlot &&
            Boundary != Boundaries.end() &&
            Boundary->second.first == Block.Id &&
            Boundary->second.second.Control == LowInstructionControl::Return &&
            Boundary->second.second.Immediate.value_or(0) == 0 &&
            Op.Addr + Boundary->second.second.Size == Block.EndAddr;
        if (!Valid) {
          InvalidCxxContinuations.insert(Identity);
          CxxContinuations.erase(Identity);
          CompleteCxxContinuations = false;
        } else if (!InvalidCxxContinuations.count(Identity)) {
          const auto [TryIndex, CatchIndex] =
              After.CxxCatchStacks.begin()->back();
          CatchReturn = RegistrationCxxContinuation{TryIndex,
                                                    CatchIndex,
                                                    Op.Addr,
                                                    Block.EndAddr,
                                                    Op.Seq,
                                                    *Target.Constant,
                                                    *SavedSP->second.Offset};
          const auto [It, Inserted] =
              CxxContinuations.emplace(Identity, *CatchReturn);
          if (!Inserted && It->second != *CatchReturn) {
            InvalidCxxContinuations.insert(Identity);
            CxxContinuations.erase(Identity);
            CatchReturn.reset();
            CompleteCxxContinuations = false;
          }
        }
      }
      if (Op.Seq >= 0 && Op.Output.Size != 0 && Value.MayBeFrame) {
        if (!Charge(1))
          break;
        auto [It, New] =
            FrameValues.emplace(std::make_pair(Op.Addr, Op.Seq), Value);
        if (!New)
          It->second = registration_state::join(It->second, Value);
      }
      const auto Memory = lowMemoryOperands(Op);
      if (EH4 && Op.Opcode == NdOp::INTRINSIC)
        CompleteCookies = false;
      if (EH4 && Memory.Address &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
        const auto Address = Transfer.read(*Memory.Address);
        if (!Memory.Complete)
          CompleteCookies = false;
        if (After.Installed && (!Address.Offset && Address.MayBeFrame))
          CompleteCookies = false;
        if (After.Installed && Address.Offset) {
          auto Touches = [&](std::optional<int32_t> Slot) {
            return Slot &&
                   overlaps(*Address.Offset, Memory.AccessSize, *Slot, 4);
          };
          // Source cookie storage is owned only by its checked initialization
          // and the native runtime. Reads or writes after installation would
          // observe or change a synthetic cookie instead of the physical one.
          const bool CheckedGSRead = CookieCheckVA && Op.Opcode == NdOp::LOAD &&
                                     Address.Offset == GSCookieSlot &&
                                     Memory.AccessSize == 4;
          ReadsGSCookie |= CheckedGSRead;
          if (Touches(EHCookieSlot) ||
              (Touches(GSCookieSlot) && !CheckedGSRead) ||
              overlaps(*Address.Offset, Memory.AccessSize,
                       *Chain.RegistrationOffset + 8, 4))
            CompleteCookies = false;
        }
        if (Memory.StoredValue) {
          const uint64_t TableEnd =
              Chain.ScopeTableVA + 16 + uint64_t(Chain.Scopes.size()) * 12;
          if (!Address.Offset &&
              (!Address.Constant ||
               (uint64_t(*Address.Constant) < SecurityCookieVA + 4 &&
                SecurityCookieVA <
                    uint64_t(*Address.Constant) + Memory.AccessSize) ||
               (uint64_t(*Address.Constant) < TableEnd &&
                Chain.ScopeTableVA <
                    uint64_t(*Address.Constant) + Memory.AccessSize)))
            CompleteCookies = false;
        }
      }
      if (CookieCheckVA && Op.Opcode == NdOp::CALL && Op.NumInputs == 1 &&
          Op.Inputs[0].isConst() && Op.Inputs[0].Offset == CookieCheckVA) {
        const auto Boundary = Boundaries.find(Op.Addr);
        if (!Transfer.isCookieCheck(Op, CookieCheckVA) || Op.Seq < 0 ||
            Boundary == Boundaries.end() || Boundary->second.first != Block.Id)
          CompleteCookies = false;
        else
          CookieChecks.emplace(
              std::make_pair(Op.Addr, Op.Seq),
              RegistrationCookieCheck{
                  Op.Addr, Op.Addr + Boundary->second.second.Size, Op.Seq});
      }
      if (Op.Opcode == NdOp::INTRINSIC) {
        CompleteImageReads = false;
        if (CheckCalls) {
          CompleteCalls = false;
          After.InitializedFrameBytes.clear();
          for (auto &[Offset, Cell] : After.Frame.Cells)
            Cell = registration_state::join(Cell, {});
        }
      }
      if (Memory.Address && Op.Opcode != NdOp::STORE &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
        if (!Charge(1))
          break;
        const auto Address = Transfer.read(*Memory.Address);
        if (!Memory.Complete)
          CompleteImageReads = false;
        else if (Address.Offset) {
          // The established private frame cannot alias an image allocation.
        } else if (Address.Constant && !Address.MayBeFrame)
          ImageReads.emplace(*Address.Constant,
                             va_t(*Address.Constant) + Memory.AccessSize);
        else
          CompleteImageReads = false;
      }
      if (Memory.Complete &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
          (Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE)) {
        const auto Address = Transfer.read(*Memory.Address);
        const uint16_t Width =
            Op.Opcode == NdOp::LOAD ? Op.Output.Size : Memory.StoredValue->Size;
        std::optional<RegistrationIncomingFrameAccess> Access;
        if (Address.MayBeFrame && !Address.Offset)
          ConsistentIncomingAccesses = false;
        if (Address.Offset && int64_t(*Address.Offset) + Width > 4)
          Access =
              RegistrationIncomingFrameAccess{Op.Addr, Op.Seq, *Address.Offset,
                                              Width, Op.Opcode == NdOp::STORE};
        auto [It, Inserted] =
            IncomingAccesses.emplace(std::make_pair(Op.Addr, Op.Seq), Access);
        ConsistentIncomingAccesses &= Inserted || It->second == Access;
      }
      if (Op.Opcode == NdOp::ATOMIC_XCHG || Op.Opcode == NdOp::ATOMIC_ADD ||
          Op.Opcode == NdOp::ATOMIC_CMPXCHG) {
        // An RMW is a write even when its scalar result is dead. This domain
        // does not guess a conditional registration state or frame-cell value.
        if (!Memory.Complete ||
            Op.MemoryAddressSpace == NdMemoryAddressSpace::X86FS ||
            Transfer.read(*Memory.Address).MayBeFrame ||
            Transfer.read(*Memory.StoredValue).MayBeFrame)
          Invalidate();
      }
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
        if (Stored.MayBeFrame && !Charge((size_t(Width) + 3) / 4))
          break;
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
            if (EH4 && !CookiesReady(After.Frame))
              CompleteCookies = false;
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
            std::set<int32_t> WrittenLevels;
            bool Valid =
                Address.Offset == Chain.TryLevelOffset &&
                (Width == 1 || Width == 2 || Width == 4) && Stored.Constant &&
                Observation != Stores.end() &&
                Observation->second.Width == Width &&
                uint32_t(Observation->second.Level) == *Stored.Constant &&
                !After.Uninstalled;
            if (Valid && Width == 4) {
              Valid = ValidLevel(int32_t(*Stored.Constant));
              WrittenLevels.insert(int32_t(*Stored.Constant));
            } else if (Valid) {
              // A byte/word state write preserves the other bytes. Never
              // sign-extend its immediate or infer zero high bytes. Replay
              // every reaching whole level and retain distinct results.
              const uint32_t Mask = Width == 1 ? UINT8_MAX : UINT16_MAX;
              Valid = !After.Unknown && !After.Levels.empty() &&
                      *Stored.Constant <= Mask;
              for (int32_t Level : After.Levels) {
                if (!Charge(1)) {
                  Valid = false;
                  break;
                }
                const int32_t Written = int32_t((uint32_t(Level) & ~Mask) |
                                                (*Stored.Constant & Mask));
                Valid &= ValidLevel(Written);
                WrittenLevels.insert(Written);
              }
            }
            if (Valid && ProvenStores.insert(Op.Addr).second) {
              After.Levels = std::move(WrittenLevels);
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
            if (CheckCalls) {
              const auto SP =
                  After.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride]
                      .Offset;
              const int64_t End = int64_t(*Address.Offset) + Width;
              if (SP && *Address.Offset >= *SP && End <= 0) {
                if (!Charge(Width))
                  break;
                for (int64_t Byte = *Address.Offset; Byte < End; ++Byte)
                  After.InitializedFrameBytes.insert(int32_t(Byte));
              }
            }
            if (Address.Offset == Chain.TryLevelOffset &&
                ProvenStores.count(Op.Addr) && After.Levels.size() == 1)
              After.Frame.Cells[*Chain.TryLevelOffset] =
                  FrameValue::constant(uint32_t(*After.Levels.begin()));
          } else if (Stored.MayBeFrame) {
            // A frame pointer escaped to storage whose future aliases cannot
            // be bounded by this frame-value domain.
            Invalidate();
          }
          if (CheckCalls && !Address.Offset && !Address.Constant) {
            After.InitializedFrameBytes.clear();
            for (auto &[Offset, Cell] : After.Frame.Cells)
              Cell = registration_state::join(Cell, {});
          }
        }
      }
      std::optional<int32_t> CallSP;
      if (CheckCalls &&
          (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)) {
        const auto Identity = std::make_pair(Op.Addr, Op.Seq);
        const auto Boundary = Boundaries.find(Op.Addr);
        const auto Callee = Op.Opcode == NdOp::CALL && Op.NumInputs == 1 &&
                                    Op.Inputs[0].isConst() &&
                                    Op.Inputs[0].Size == 4
                                ? CalleeIndices.find(Op.Inputs[0].Offset)
                                : CalleeIndices.end();
        const auto SP =
            After.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride]
                .Offset;
        bool Valid =
            !After.Unknown && !Facts[I].Invalid && Op.Seq >= 0 &&
            Boundary != Boundaries.end() &&
            Boundary->second.first == Block.Id &&
            Boundary->second.second.Control == LowInstructionControl::Call &&
            Callee != CalleeIndices.end() && SP;
        RegistrationCallFrameEffect Effect;
        if (Valid) {
          const auto &Contract = Result.CalleeContracts[Callee->second];
          Effect.Address = Op.Addr;
          Effect.EndAddress =
              Boundary->second.second.Address + Boundary->second.second.Size;
          Effect.OpSeq = Op.Seq;
          Effect.Target = Contract.Target;
          Effect.CalleeIndex = Callee->second;
          Effect.StackPopBytes = Contract.StackPopBytes;
          Effect.DoesNotReturn = Contract.DoesNotReturn;
          const bool BorrowsObject =
              !Contract.ECXReads.empty() || !Contract.ECXWrites.empty();
          if (BorrowsObject)
            Effect.ECXFrameOffset =
                After.Frame.Registers[x86reg::RCX / x86reg::GeneralRegStride]
                    .Offset;
          Valid &= !BorrowsObject || Effect.ECXFrameOffset.has_value();
          if (Valid && BorrowsObject) {
            Valid &=
                ProjectFrameObject(After, *Effect.ECXFrameOffset, SP,
                                   Contract.ECXReads, Effect.FrameReads, true);
            Valid &= ProjectFrameObject(After, *Effect.ECXFrameOffset, SP,
                                        Contract.ECXWrites, Effect.FrameWrites,
                                        false);
          }
          if (Valid && !Charge(Contract.ImageReads.size() +
                               Effect.FrameWrites.size() + 1))
            Valid = false;
          if (Valid) {
            for (const auto &Read : Contract.ImageReads)
              ImageReads.emplace(Read.Begin, Read.End);
            for (const auto &Write : Effect.FrameWrites) {
              if (!Charge(After.Frame.Cells.size())) {
                Valid = false;
                break;
              }
              // Callee writes are may-effects. They invalidate value facts
              // without inventing definite initialization on untaken paths.
              for (auto It = After.Frame.Cells.begin();
                   It != After.Frame.Cells.end();)
                if (int64_t(It->first) < Write.End &&
                    Write.Begin < int64_t(It->first) + 4) {
                  if (It->second.MayBeFrame &&
                      (Write.Begin > It->first ||
                       int64_t(It->first) + 4 > Write.End)) {
                    It->second = {{}, {}, false, true};
                    ++It;
                  } else
                    It = After.Frame.Cells.erase(It);
                } else
                  ++It;
            }
          }
        }
        if (Valid && !InvalidCalls.count(Identity)) {
          const auto [It, Inserted] = CallEffects.emplace(Identity, Effect);
          Valid = Inserted || It->second == Effect;
        } else
          Valid = false;
        if (!Valid) {
          InvalidCalls.insert(Identity);
          CallEffects.erase(Identity);
          CompleteCalls = CompleteImageReads = false;
          After.InitializedFrameBytes.clear();
          // A callee without a checked borrow may change cells reached by an
          // escaped register or stack pointer. Keep taint but drop identities.
          for (auto &[Offset, Cell] : After.Frame.Cells)
            Cell = registration_state::join(Cell, {});
        } else {
          CallSP = *SP;
          NoReturnAtExit = Effect.DoesNotReturn;
          if (NoReturnAtExit)
            NoReturnEnd = Effect.EndAddress;
        }
      }
      Transfer.write(Op, Value, CookieCheckVA);
      if (CallSP)
        After.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
            FrameValue::frame(*CallSP);
      if (CheckCalls && Op.Output.isReg() &&
          Op.Output.Offset / x86reg::GeneralRegStride ==
              x86reg::RSP / x86reg::GeneralRegStride) {
        const auto SP =
            After.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride]
                .Offset;
        if (!SP)
          After.InitializedFrameBytes.clear();
        else {
          if (!Charge(After.InitializedFrameBytes.size()))
            break;
          After.InitializedFrameBytes.erase(
              After.InitializedFrameBytes.begin(),
              After.InitializedFrameBytes.lower_bound(*SP));
        }
      }
      if (NoReturnAtExit)
        break;
    }
    for (auto It = Stores.lower_bound(Block.StartAddr);
         It != Stores.end() && It->first < Block.EndAddr; ++It)
      if ((!NoReturnAtExit || It->first < NoReturnEnd) &&
          !ProvenStores.count(It->first))
        Invalidate();
    if (Facts[I].Invalid)
      After.Unknown = true;
    Facts[I].InstalledAtExit = After.Installed;
    Facts[I].CxxContinuationAtExit = CatchReturn.has_value();
    Facts[I].NoReturnAtExit = NoReturnAtExit;
    if (!NoReturnAtExit) {
      for (int Successor : Block.Succs) {
        auto It = Index.find(Successor);
        if (It == Index.end()) {
          Result.Diagnostics.push_back(
              "registration-state CFG has a missing edge");
          return Result;
        }
        Merge(It->second, After);
      }
    }
    if (CatchReturn) {
      const auto Resume = Entries.find(CatchReturn->TargetVA);
      if (Resume != Entries.end()) {
        Domain Continued = After;
        auto Stack = *Continued.CxxCatchStacks.begin();
        Stack.pop_back();
        Continued.Parent = Stack.empty();
        Continued.Callback = !Stack.empty();
        Continued.OtherCallback = false;
        Continued.CxxCatchStacks.clear();
        if (!Stack.empty())
          Continued.CxxCatchStacks.insert(std::move(Stack));
        Continued.Frame.Registers[x86reg::RBP / x86reg::GeneralRegStride] =
            FrameValue::frame(0);
        Continued.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
            FrameValue::frame(CatchReturn->SavedStackOffset);
        Merge(Resume->second, Continued);
      }
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
        if (CheckCleanups && Action.ActionVA) {
          const CleanupKey Identity{Block.Id, Level, uint32_t(Walk)};
          const auto Contract = CleanupIndices.find(uint32_t(Walk));
          auto SP =
              Before.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride]
                  .Offset;
          if (!Before.Parent && Before.Callback && !Before.OtherCallback &&
              !Before.CxxCatchStacks.empty())
            SP = Before.Frame.load(*Chain.RegistrationOffset - 4, 4).Offset;
          bool Valid = !Before.Unknown && !Facts[I].Invalid &&
                       Contract != CleanupIndices.end() && SP;
          RegistrationCleanupFrameEffect Effect;
          if (Valid) {
            const auto &C = Result.CleanupContracts[Contract->second];
            Effect.BlockId = Block.Id;
            Effect.Range = {Block.StartAddr, Block.EndAddr};
            Effect.DispatchLevel = Level;
            Effect.ActionState = uint32_t(Walk);
            Effect.CleanupIndex = Contract->second;
            Effect.StackOffset = *SP;
            Valid &=
                ProjectFrameObject(Before, C.ObjectFrameOffset, SP,
                                   C.Leaf.ECXReads, Effect.FrameReads, true);
            Valid &=
                ProjectFrameObject(Before, C.ObjectFrameOffset, SP,
                                   C.Leaf.ECXWrites, Effect.FrameWrites, false);
            if (Valid && Charge(C.Leaf.ImageReads.size()))
              for (const auto &Read : C.Leaf.ImageReads)
                ImageReads.emplace(Read.Begin, Read.End);
            else
              Valid = false;
          }
          if (Valid && !InvalidCleanups.count(Identity)) {
            auto [It, Inserted] = CleanupEffects.emplace(Identity, Effect);
            Valid = Inserted || It->second == Effect;
          } else
            Valid = false;
          if (!Valid) {
            InvalidCleanups.insert(Identity);
            CleanupEffects.erase(Identity);
            CompleteCleanups = false;
            CompleteImageReads = false;
          }
        }
        Dispatch(Action.ActionVA, Action.ToState, Before, true, true);
        Walk = Action.ToState;
      }
      for (uint32_t TryIndex = 0; TryIndex < Cxx.TryBlocks.size(); ++TryIndex) {
        const CxxTryBlock &Try = Cxx.TryBlocks[TryIndex];
        if (!Charge(1))
          break;
        if (Try.TryLow >= MinimumTry && Level >= Try.TryLow &&
            Level <= Try.TryHigh)
          for (uint32_t CatchIndex = 0; CatchIndex < Try.Handlers.size();
               ++CatchIndex)
            Dispatch(Try.Handlers[CatchIndex].HandlerVA, Try.TryHigh + 1,
                     Before, Facts[I].Invalid, true, false,
                     std::make_pair(TryIndex, CatchIndex));
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
  for (const auto &[Identity, Continuation] : CxxContinuations) {
    Result.CxxContinuations.push_back(Continuation);
    CompleteCxxContinuations &= Entries.count(Continuation.TargetVA) != 0;
  }
  Result.CxxContinuationsComplete = CompleteCxxContinuations;
  for (size_t I = 0; I < Function.Blocks.size(); ++I) {
    const LowBlock &Block = Function.Blocks[I];
    const Domain &State = Incoming[I];
    const bool CallbackOnly = State.Callback && !State.Parent;
    const bool Unknown =
        State.Reached && (State.Unknown || Facts[I].Invalid ||
                          (State.Callback && State.Parent) ||
                          (State.Uninstalled && State.Installed));
    const std::set<int32_t> &Levels = Unknown ? AllLevels : State.Levels;
    if (!Charge(Levels.size() + State.CxxCatchStacks.size() + 1)) {
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
                             State.CanDispatch && State.Installed,
                             EH.Cxx ? cxxMinimumTryLevel(State, *EH.Cxx) : 0});
    if (CallbackOnly && Unknown)
      Result.CallbackStatesComplete = false;
    if (!CallbackOnly && Unknown)
      Result.Complete = false;
    if (State.Parent && State.Reached &&
        (Unknown || (Block.Succs.empty() && Facts[I].InstalledAtExit &&
                     !Facts[I].NoReturnAtExit)))
      Result.RegistrationLifetimeComplete = false;
    if (!State.CxxCatchStacks.empty() && State.Reached && Block.Succs.empty() &&
        !Facts[I].CxxContinuationAtExit && !Facts[I].NoReturnAtExit)
      Result.CxxContinuationsComplete = false;
  }
  Result.RegistrationLifetimeComplete &= Result.CxxContinuationsComplete;
  if (!Result.CxxContinuationsComplete) {
    Result.Complete = Result.CallbackStatesComplete = false;
    Result.Diagnostics.push_back(
        "C++ catch continuation or restored stack is not proven");
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
  Result.IncomingFrameAccessesComplete = ConsistentIncomingAccesses;
  Result.CallFrameEffectsComplete = CompleteCalls && Result.Complete &&
                                    Result.CallbackStatesComplete &&
                                    Result.RegistrationLifetimeComplete;
  for (const auto &[Identity, Call] : CallEffects)
    Result.CallFrameEffects.push_back(Call);
  if (CheckCalls && !Result.CallFrameEffectsComplete)
    Result.Diagnostics.push_back(
        "registration call stack or initialized object borrow is not proven");
  Result.CleanupFrameEffectsComplete =
      CompleteCleanups && Result.Complete && Result.CallbackStatesComplete &&
      Result.RegistrationLifetimeComplete && !Exhausted;
  if (Result.CleanupFrameEffectsComplete)
    for (const auto &[Identity, Effect] : CleanupEffects)
      Result.CleanupFrameEffects.push_back(Effect);
  if (CheckCleanups && !Result.CleanupFrameEffectsComplete)
    Result.Diagnostics.push_back(
        "registration cleanup initialized object borrow is not proven");
  Result.ImageReadsComplete = Result.Complete &&
                              Result.CallbackStatesComplete &&
                              CompleteImageReads && !Exhausted;
  Result.SecurityCookiesComplete =
      CompleteCookies && Result.Complete && Result.CallbackStatesComplete &&
      Result.RegistrationLifetimeComplete && Result.ChainOperationsComplete &&
      CookieChecks.size() == CookieCheckOccurrences.size() &&
      (!ReadsGSCookie || !CookieChecks.empty());
  if (Result.SecurityCookiesComplete) {
    Result.SecurityCookieVA = SecurityCookieVA;
    Result.CookieCheckVA = CookieCheckVA;
    for (const auto &[Identity, Check] : CookieChecks)
      Result.CookieChecks.push_back(Check);
  }
  if (Result.ImageReadsComplete)
    for (const auto &[Begin, End] : ImageReads)
      Result.ImageReads.push_back({Begin, End});
  if (Result.Complete && Result.CallbackStatesComplete &&
      ConsistentIncomingAccesses)
    for (const auto &[Identity, Access] : IncomingAccesses)
      if (Access)
        Result.IncomingFrameAccesses.push_back(*Access);
  if (!ConsistentIncomingAccesses)
    Result.Diagnostics.push_back(
        "incoming caller frame projection is not exact");
  if (Result.Complete && Result.CallbackStatesComplete) {
    if (!Charge(FrameValues.size())) {
      Result.Complete = Result.ChainOperationsComplete =
          Result.ImageReadsComplete = Result.SecurityCookiesComplete =
              Result.CallFrameEffectsComplete =
                  Result.CleanupFrameEffectsComplete = false;
      Result.CallFrameEffects.clear();
      Result.CleanupFrameEffects.clear();
      Result.SecurityCookieVA = 0;
      Result.CookieCheckVA = 0;
      Result.CookieChecks.clear();
      Result.ChainAccesses.clear();
      Result.Diagnostics.push_back(
          "registration-frame output budget exhausted");
      return Result;
    }
    for (const auto &[Identity, Value] : FrameValues)
      Result.FrameValues.push_back(
          {Identity.first, Identity.second, Value.Offset});
  }
  return Result;
}

} // namespace neverd
