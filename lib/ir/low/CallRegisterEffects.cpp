//===- CallRegisterEffects.cpp - Callee GPR write summaries -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/CallRegisterEffects.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <deque>

namespace neverd {

namespace {
constexpr uint64_t kX64GPRBytes = 16 * 8;
constexpr unsigned kX64StackPointerFamily = 4; // RSP

GPRFamilyMask familyBit(Arch A, uint64_t RegOff) {
  auto Family = gprFamilyOf(A, RegOff);
  if (!Family || *Family == kX64StackPointerFamily)
    return 0;
  return GPRFamilyMask(1) << *Family;
}

/// Record a read of \p Size bytes at \p RegOff.
void addRead(Arch A, GPRReadWidths &Reads, uint64_t RegOff, uint64_t Size) {
  auto Family = gprFamilyOf(A, RegOff);
  if (!Family || *Family == kX64StackPointerFamily)
    return;
  const uint64_t End = std::min<uint64_t>(RegOff % 8 + Size, 8);
  Reads[*Family] = std::max(Reads[*Family], static_cast<uint8_t>(End));
}

void joinReads(GPRReadWidths &Into, const GPRReadWidths &From) {
  for (size_t I = 0; I < Into.size(); ++I)
    Into[I] = std::max(Into[I], From[I]);
}

/// A value's relation to the stack pointer at function entry: unrelated, an
/// entry-relative byte offset in [Lo, Hi], or a stack address whose offset is
/// not known.
struct StackValue {
  enum Kind : uint8_t { Unrelated, Range, Unknown };
  Kind K = Unrelated;
  int64_t Lo = 0;
  int64_t Hi = 0;

  static StackValue at(int64_t Offset) { return {Range, Offset, Offset}; }
  static StackValue unknown() { return {Unknown, 0, 0}; }
  StackValue shifted(int64_t Delta) const {
    return {K, Lo + Delta, Hi + Delta};
  }
  bool operator==(const StackValue &) const = default;
};

StackValue joinStack(const StackValue &A, const StackValue &B) {
  if (A.K == StackValue::Unrelated)
    return B;
  if (B.K == StackValue::Unrelated)
    return A;
  if (A.K == StackValue::Unknown || B.K == StackValue::Unknown)
    return StackValue::unknown();
  return {StackValue::Range, std::min(A.Lo, B.Lo), std::max(A.Hi, B.Hi)};
}

/// Temporaries and whole x86-64 GPRs, keyed by (is temporary, offset).
using StackKey = std::pair<bool, uint64_t>;
using StackState = std::map<StackKey, StackValue>;

std::optional<StackKey> stackKeyOf(const NdVar &V) {
  if (V.isTemp())
    return StackKey{true, V.Offset};
  if (V.isReg() && V.Offset < kX64GPRBytes)
    return StackKey{false, V.Offset / 8 * 8};
  return std::nullopt;
}

/// Only a whole 8-byte value carries an address.
bool wholeWord(const NdVar &V) {
  return V.Size == 8 && (!V.isReg() || V.Offset % 8 == 0);
}

StackValue stackValueOf(const StackState &S, const NdVar &V) {
  auto Key = stackKeyOf(V);
  if (!Key || !wholeWord(V))
    return {};
  auto It = S.find(*Key);
  return It == S.end() ? StackValue{} : It->second;
}

/// Bound the incoming stack-argument slots \p F reads, per Win64: the body
/// reads slot K when it loads from the entry stack pointer plus
/// EntryStackBase + K * SlotBytes, directly or through a tail call made at
/// the entry stack pointer.  Anything that could read those slots out of
/// sight makes the bound unknown.
void summarizeIncomingStackReads(const BinaryImage &Img, const LowFunc &F,
                                 const std::map<int, size_t> &IndexOfId,
                                 const std::set<va_t> &BlockStarts,
                                 LocalRegisterEffect &Effect) {
  if (Img.Arch != Arch::X64 || Img.Format != BinaryFormat::COFF) {
    Effect.UnknownStackReads = true;
    return;
  }
  const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);
  const IntegerArgumentLayout Layout = TRI.integerArgumentLayout(true);
  const StackKey SP{false, TRI.StackPointer};
  const size_t NumArgRegs = Layout.Registers.size();
  // The home slot of argument register K lies just above the return address.
  auto HomeSlot = [&](size_t K) {
    return static_cast<int64_t>(TRI.PointerSize + K * Layout.SlotBytes);
  };
  // Spills of an incoming argument register to its own home slot in the
  // entry block, by register position: the step that spills it.
  std::vector<std::optional<size_t>> HomeSpill(NumArgRegs);
  // Home slots a load reads back.
  std::vector<bool> HomeLoaded(NumArgRegs);
  // The lowest incoming-area offset a pointer that escapes addresses.
  std::optional<int64_t> LowestEscape;
  // A pointer at or above the home area can reach the incoming arguments.
  auto Escapes = [&](const StackValue &V) {
    const bool Reaches = V.K == StackValue::Unknown ||
                         (V.K == StackValue::Range &&
                          V.Hi >= static_cast<int64_t>(TRI.PointerSize));
    if (Reaches && V.K == StackValue::Range)
      LowestEscape = LowestEscape ? std::min(*LowestEscape, V.Lo) : V.Lo;
    return Reaches;
  };
  // Argument registers the entry block writes before each of its operations;
  // a call clobbers them all.
  std::vector<uint8_t> EntryWrittenBefore;
  {
    uint8_t Written = 0;
    for (const LowOp &Op : F.Blocks.front().Ops) {
      EntryWrittenBefore.push_back(Written);
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
        Written = 0xFF;
      else if (Op.Output.isReg())
        for (size_t K = 0; K < NumArgRegs; ++K)
          if (Op.Output.Offset / 8 == Layout.Registers[K] / 8)
            Written |= uint8_t(1u << K);
    }
  }
  bool Unknown = false;
  int StackArgs = 0;
  auto Run = [&](const LowBlock &Block, StackState &S) {
    auto ControlOf = [&](size_t OpIndex) {
      for (const LowInstructionBoundary &Boundary : Block.InstructionBoundaries)
        if (OpIndex >= Boundary.FirstOp &&
            OpIndex - Boundary.FirstOp < Boundary.OpCount)
          return Boundary.Control;
      return LowInstructionControl::None;
    };
    auto Set = [&](const NdVar &V, StackValue Value) {
      auto Key = stackKeyOf(V);
      if (!Key)
        return;
      if (!wholeWord(V))
        Value = {};
      // The stack pointer always addresses the stack.
      if (*Key == SP && Value.K == StackValue::Unrelated)
        Value = StackValue::unknown();
      if (Value.K == StackValue::Unrelated)
        S.erase(*Key);
      else
        S[*Key] = Value;
    };
    auto ArgumentsEscape = [&]() {
      for (uint64_t Reg : Layout.Registers)
        if (Escapes(stackValueOf(S, NdVar::reg(Reg, 8))))
          return true;
      return false;
    };
    for (size_t I = 0; I < Block.Ops.size() && !Unknown; ++I) {
      const LowOp &Op = Block.Ops[I];
      const bool TailCall = ControlOf(I) == LowInstructionControl::TailCall;
      auto In = [&](unsigned K) {
        return K < Op.NumInputs ? stackValueOf(S, Op.Inputs[K]) : StackValue{};
      };
      auto Const = [&](unsigned K) -> std::optional<int64_t> {
        if (K < Op.NumInputs && Op.Inputs[K].isConst())
          return static_cast<int64_t>(Op.Inputs[K].Offset);
        return std::nullopt;
      };
      const bool DirectTarget = Op.NumInputs > 0 && Op.Inputs[0].isConst();
      switch (Op.Opcode) {
      case NdOp::COPY:
        Set(Op.Output, In(0));
        continue;
      case NdOp::INT_ADD: {
        const StackValue A = In(0), B = In(1);
        if (A.K == StackValue::Range && Const(1))
          Set(Op.Output, A.shifted(*Const(1)));
        else if (B.K == StackValue::Range && Const(0))
          Set(Op.Output, B.shifted(*Const(0)));
        else if (A.K != StackValue::Unrelated || B.K != StackValue::Unrelated)
          Set(Op.Output, StackValue::unknown());
        else
          Set(Op.Output, {});
        continue;
      }
      case NdOp::INT_SUB: {
        const StackValue A = In(0);
        if (A.K == StackValue::Range && Const(1))
          Set(Op.Output, A.shifted(-*Const(1)));
        else if (A.K != StackValue::Unrelated)
          Set(Op.Output, StackValue::unknown());
        else
          Set(Op.Output, {});
        continue;
      }
      case NdOp::LOAD:
      case NdOp::ATOMIC_XCHG:
      case NdOp::ATOMIC_ADD:
      case NdOp::ATOMIC_CMPXCHG:
        if (Op.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
          const StackValue A = In(0);
          if (A.K == StackValue::Unknown) {
            Unknown = true;
          } else if (A.K == StackValue::Range) {
            const int64_t Last =
                A.Hi + std::max<int64_t>(Op.Output.Size, 1) - 1;
            for (size_t K = 0; K < NumArgRegs; ++K)
              if (A.Lo < HomeSlot(K) + static_cast<int64_t>(Layout.SlotBytes) &&
                  HomeSlot(K) <= Last)
                HomeLoaded[K] = true;
            if (Last >= Layout.EntryStackBase)
              StackArgs = std::max<int64_t>(
                  StackArgs,
                  static_cast<int64_t>(Layout.Registers.size()) +
                      (Last - Layout.EntryStackBase) / Layout.SlotBytes + 1);
          }
        }
        // An atomic also stores its value operands.
        for (unsigned K = 1; K < Op.NumInputs; ++K)
          Unknown |= Escapes(In(K));
        Set(Op.Output, {});
        continue;
      case NdOp::STORE: {
        // Writing the slots reads none; storing a pointer to them lets
        // other code read them.
        Unknown |= Escapes(In(1));
        const StackValue A = In(0);
        if (&Block == &F.Blocks.front() && A.K == StackValue::Range &&
            A.Lo == A.Hi && Op.NumInputs >= 2)
          for (size_t K = 0; K < NumArgRegs; ++K)
            if (A.Lo == HomeSlot(K) &&
                Op.Inputs[1] == NdVar::reg(Layout.Registers[K], 8) &&
                !((EntryWrittenBefore[I] >> K) & 1))
              HomeSpill[K] = I;
        continue;
      }
      case NdOp::CALL:
      case NdOp::INDIR_CALL:
      case NdOp::BRANCH:
      case NdOp::COND_BR:
      case NdOp::INDIR_BR:
        break;
      default:
        // Arithmetic on a stack address (a flag, an alignment mask) yields
        // no address; an operation that may dereference a pointer into the
        // incoming slots is a read this summary cannot bound.
        if (Op.Opcode == NdOp::INTRINSIC)
          for (unsigned K = 0; K < Op.NumInputs; ++K)
            Unknown |= Escapes(In(K));
        Set(Op.Output, {});
        continue;
      }
      // Control transfer.
      const bool LeavesFunction =
          (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR)
              ? DirectTarget && !BlockStarts.count(Op.Inputs[0].Offset)
              : Op.Opcode != NdOp::INDIR_BR || Block.Succs.empty();
      if (!LeavesFunction)
        continue;
      Unknown |= ArgumentsEscape();
      if ((Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
          !TailCall) {
        // A returning callee reads its own slots, below this function's, and
        // leaves nothing in the registers it may clobber.
        for (uint64_t Family = 0; Family < kX64GPRBytes; Family += 8)
          if (!TRI.isCalleeSaveReg(Family) && StackKey{false, Family} != SP)
            S.erase(StackKey{false, Family});
        continue;
      }
      // A tail call hands this function's stack to its target.
      const bool EntryStack =
          stackValueOf(S, NdVar::reg(TRI.StackPointer, 8)) == StackValue::at(0);
      if (DirectTarget && Op.Opcode != NdOp::INDIR_CALL &&
          Op.Opcode != NdOp::INDIR_BR && EntryStack &&
          !Img.findImportAt(Op.Inputs[0].Offset))
        Effect.StackTailCallees.insert(Op.Inputs[0].Offset);
      else
        Unknown = true;
    }
  };

  std::vector<std::optional<StackState>> BlockEntry(F.Blocks.size());
  std::vector<unsigned> Visits(F.Blocks.size());
  BlockEntry[0] = StackState{{SP, StackValue::at(0)}};
  std::deque<size_t> Work{0};
  auto Drain = [&]() {
    while (!Work.empty() && !Unknown) {
      const size_t BI = Work.front();
      Work.pop_front();
      StackState S = *BlockEntry[BI];
      Run(F.Blocks[BI], S);
      for (int SuccId : F.Blocks[BI].Succs) {
        auto It = IndexOfId.find(SuccId);
        if (It == IndexOfId.end())
          continue;
        const size_t Succ = It->second;
        if (!BlockEntry[Succ]) {
          BlockEntry[Succ] = S;
          Work.push_back(Succ);
          continue;
        }
        StackState &Old = *BlockEntry[Succ];
        const bool Widen = ++Visits[Succ] > limits::kMaxStackOffsetJoinVisits;
        bool Changed = false;
        for (const auto &[Key, Value] : S) {
          StackValue &Slot = Old[Key];
          StackValue Next = joinStack(Slot, Value);
          if (Next == Slot)
            continue;
          Slot = Widen ? StackValue::unknown() : Next;
          Changed = true;
        }
        if (Changed)
          Work.push_back(Succ);
      }
    }
  };
  Drain();
  // A block no edge reaches (a handler the unwinder enters) runs with a stack
  // and frame pointer this summary does not know.
  const StackKey FP{false, TRI.FramePointer};
  for (size_t BI = 0; BI < F.Blocks.size() && !Unknown; ++BI) {
    if (BlockEntry[BI])
      continue;
    BlockEntry[BI] =
        StackState{{SP, StackValue::unknown()}, {FP, StackValue::unknown()}};
    Work.push_back(BI);
    Drain();
  }
  // A variadic prologue spills the first variadic argument register and
  // every later one to their home slots and hands a pointer to the first of
  // those slots on as the va_list.  A caller passes only the variadic
  // arguments it sets, so those spills read none.  Any other spill that no
  // load reads back, with no pointer to the incoming area escaping, reads
  // nothing either: only this function could observe its home slots.
  int VariadicFrom = -1;
  if (LowestEscape && *LowestEscape >= HomeSlot(1) &&
      (*LowestEscape - HomeSlot(0)) % Layout.SlotBytes == 0) {
    const size_t First =
        static_cast<size_t>((*LowestEscape - HomeSlot(0)) / Layout.SlotBytes);
    bool Spilled = First < NumArgRegs;
    for (size_t K = First; Spilled && K < NumArgRegs; ++K)
      Spilled = HomeSpill[K].has_value();
    if (Spilled)
      VariadicFrom = static_cast<int>(First);
  }
  for (size_t K = 0; K < NumArgRegs; ++K) {
    if (!HomeSpill[K])
      continue;
    const bool Reads = VariadicFrom >= 0 ? static_cast<int>(K) < VariadicFrom
                                         : Unknown || HomeLoaded[K];
    if (auto Family = gprFamilyOf(Img.Arch, Layout.Registers[K]);
        Family && !Reads)
      Effect.Blocks.front().Steps[*HomeSpill[K]].Reads[*Family] = 0;
  }
  Effect.VariadicFrom = VariadicFrom;
  Effect.UnknownStackReads |= Unknown;
  Effect.StackArgs = StackArgs;
}
} // namespace

std::optional<unsigned> gprFamilyOf(Arch A, uint64_t RegOff) {
  if (A != Arch::X64 || RegOff >= kX64GPRBytes)
    return std::nullopt;
  return static_cast<unsigned>(RegOff / 8);
}

LocalRegisterEffect
localRegisterEffect(const BinaryImage &Img, const LowFunc &F,
                    const libc::NoReturnTargetIndex *NoReturnTargets) {
  LocalRegisterEffect Effect;
  if (!F.hasCompleteLiftCoverage() ||
      !F.UnsafeIndirectBranchAddresses.empty() || F.Blocks.empty()) {
    Effect.Unknown = true;
    Effect.Incomplete = true;
    Effect.UnknownStackReads = true;
    return Effect;
  }
  std::map<int, size_t> IndexOfId;
  std::set<va_t> BlockStarts;
  for (size_t I = 0; I < F.Blocks.size(); ++I) {
    IndexOfId[F.Blocks[I].Id] = I;
    BlockStarts.insert(F.Blocks[I].StartAddr);
  }
  Effect.Blocks.resize(F.Blocks.size());
  const uint64_t FPReturn = getTargetRegInfo(Img.Arch).FPReturnReg;
  auto WritesFPReturn = [&](const NdVar &Out) {
    return FPReturn != 0 && Out.isReg() && Out.Size != 0 &&
           Out.Offset < FPReturn + 16 && FPReturn < Out.Offset + Out.Size;
  };
  for (size_t BI = 0; BI < F.Blocks.size(); ++BI) {
    const LowBlock &Block = F.Blocks[BI];
    RegisterBlock &Out = Effect.Blocks[BI];
    for (int Succ : Block.Succs)
      if (auto It = IndexOfId.find(Succ); It != IndexOfId.end())
        Out.Succs.push_back(It->second);
    auto ControlOf = [&](size_t OpIndex) {
      for (const LowInstructionBoundary &Boundary : Block.InstructionBoundaries)
        if (OpIndex >= Boundary.FirstOp &&
            OpIndex - Boundary.FirstOp < Boundary.OpCount)
          return std::pair{Boundary.Control, Boundary.ControlFlags};
      return std::pair{LowInstructionControl::None,
                       LowInstructionControlFlag::None};
    };
    for (size_t I = 0; I < Block.Ops.size(); ++I) {
      const LowOp &Op = Block.Ops[I];
      const auto [Control, Flags] = ControlOf(I);
      const bool TailCall = Control == LowInstructionControl::TailCall;
      const bool DirectTarget = Op.NumInputs > 0 && Op.Inputs[0].isConst();
      const bool Transfers = Op.Opcode == NdOp::CALL ||
                             Op.Opcode == NdOp::BRANCH ||
                             Op.Opcode == NdOp::COND_BR;
      const bool EntersNoReturnFunction =
          Transfers && DirectTarget &&
          libc::isNoReturnTarget(Img, Op.Inputs[0].Offset, NoReturnTargets);
      // The CFG flags a call that never returns; the LowIR contract keeps a
      // tail call unflagged, so a tail call into a no-return function is
      // recognized here.
      const bool NoReturnCall =
          Op.Opcode == NdOp::CALL &&
          (hasLowInstructionControlFlag(Flags,
                                        LowInstructionControlFlag::NoReturn) ||
           (TailCall && EntersNoReturnFunction));
      RegisterStep Step;
      for (uint8_t In = 0; In < Op.NumInputs; ++In)
        if (Op.Inputs[In].isReg())
          addRead(Img.Arch, Step.Reads, Op.Inputs[In].Offset,
                  Op.Inputs[In].Size);
      // The result of a call that never returns reaches no one.
      if (Op.Output.isReg() && !NoReturnCall) {
        const GPRFamilyMask Bit = familyBit(Img.Arch, Op.Output.Offset);
        Effect.Writes |= Bit;
        if (WritesFPReturn(Op.Output))
          Effect.Writes |= kFPReturnWriteBit;
        // A 32- or 64-bit write defines the whole register; a byte or word
        // write keeps the rest of the old value, which stays live exactly as
        // wide as a later read needs it.
        if (Op.Output.Size >= 4)
          Step.Kills |= Bit;
        else if (auto Family = gprFamilyOf(Img.Arch, Op.Output.Offset);
                 Family && Op.Output.Offset % 8 == 0)
          Step.LowWrites[*Family] = std::max<uint8_t>(
              Step.LowWrites[*Family], static_cast<uint8_t>(Op.Output.Size));
      }
      switch (Op.Opcode) {
      case NdOp::INDIR_CALL:
        // A rewritten indirect tail jump (`jmp [iat]` in an import thunk)
        // passes this function's registers on to its unknown target.
        Effect.Unknown = true;
        (TailCall ? Step.UnknownTailCall : Step.UnknownCall) = true;
        break;
      case NdOp::INDIR_BR:
        // A resolved jump table has successors; an indirect tail jump leaves
        // for code this summary cannot see.
        if (Block.Succs.empty()) {
          Effect.Unknown = true;
          Step.UnknownTailCall = true;
        }
        break;
      case NdOp::BRANCH:
      case NdOp::COND_BR:
        // A direct branch to another function's entry leaves this body the
        // way a tail call does; the CFG keeps no block for it.
        if (DirectTarget && !BlockStarts.count(Op.Inputs[0].Offset)) {
          // A jump into a function that never returns adds no write a caller
          // can observe, and an unconditional one ends this path.
          if (!EntersNoReturnFunction)
            Effect.Callees.insert(Op.Inputs[0].Offset);
          Step.Callee = Op.Inputs[0].Offset;
          Step.TailCallee = true;
          Step.Exits = EntersNoReturnFunction && Op.Opcode == NdOp::BRANCH;
        }
        break;
      case NdOp::CALL: {
        // A call that never returns cannot change a register its caller
        // reads, but it still receives this function's registers.
        Step.Exits = NoReturnCall;
        if (!DirectTarget || Img.findImportAt(Op.Inputs[0].Offset)) {
          if (!NoReturnCall)
            Effect.Unknown = true;
          (TailCall ? Step.UnknownTailCall : Step.UnknownCall) = true;
        } else {
          Step.Callee = Op.Inputs[0].Offset;
          Step.TailCallee = TailCall;
          if (!NoReturnCall)
            Effect.Callees.insert(Op.Inputs[0].Offset);
          else if (!EntersNoReturnFunction)
            Effect.NoReturnCallees.insert(Op.Inputs[0].Offset);
        }
        break;
      }
      default:
        break;
      }
      Effect.UnknownEntryReads |= Step.UnknownTailCall;
      Out.Steps.push_back(Step);
    }
  }
  summarizeIncomingStackReads(Img, F, IndexOfId, BlockStarts, Effect);
  return Effect;
}

namespace {
/// Bytes of each family live on entry to \p F, given the current callee
/// summaries.
GPRReadWidths entryLiveWidths(const LocalRegisterEffect &F,
                              const std::map<va_t, GPRFamilyMask> &MayWrite,
                              const std::map<va_t, GPRReadWidths> &EntryReads,
                              GPRFamilyMask VolatileFamilies,
                              GPRFamilyMask ArgumentFamilies,
                              const std::set<va_t> &DispatchThunks) {
  auto Clear = [](GPRReadWidths &Live, GPRFamilyMask Mask) {
    for (size_t I = 0; I < Live.size(); ++I)
      if ((Mask >> I) & 1)
        Live[I] = 0;
  };
  auto Transfer = [&](const RegisterBlock &Block, GPRReadWidths Live) {
    for (auto It = Block.Steps.rbegin(); It != Block.Steps.rend(); ++It) {
      const RegisterStep &Step = *It;
      if (Step.Exits)
        Live.fill(0);
      if (Step.UnknownTailCall) {
        Live.fill(0);
        for (size_t I = 0; I < Live.size(); ++I)
          if ((ArgumentFamilies >> I) & 1)
            Live[I] = 8;
      } else if (Step.UnknownCall || (Step.Callee != InvalidVA &&
                                      DispatchThunks.count(Step.Callee))) {
        Clear(Live, VolatileFamilies);
      } else if (Step.Callee != InvalidVA) {
        auto W = MayWrite.find(Step.Callee);
        Clear(Live, W != MayWrite.end() ? W->second : VolatileFamilies);
        if (auto R = EntryReads.find(Step.Callee); R != EntryReads.end())
          joinReads(Live, R->second);
      }
      Clear(Live, Step.Kills);
      for (size_t I = 0; I < Live.size(); ++I)
        if (Step.LowWrites[I] && Live[I] <= Step.LowWrites[I])
          Live[I] = 0;
      joinReads(Live, Step.Reads);
    }
    return Live;
  };
  std::vector<GPRReadWidths> LiveIn(F.Blocks.size(), GPRReadWidths{});
  for (bool Changed = true; Changed;) {
    Changed = false;
    for (size_t B = F.Blocks.size(); B-- > 0;) {
      GPRReadWidths LiveOut{};
      for (size_t S : F.Blocks[B].Succs)
        if (S < LiveIn.size())
          joinReads(LiveOut, LiveIn[S]);
      const GPRReadWidths In = Transfer(F.Blocks[B], LiveOut);
      if (In != LiveIn[B]) {
        LiveIn[B] = In;
        Changed = true;
      }
    }
  }
  return LiveIn.empty() ? GPRReadWidths{} : LiveIn[0];
}
} // namespace

CallRegisterSummaries
solveCallRegisterEffects(const std::map<va_t, LocalRegisterEffect> &Funcs,
                         GPRFamilyMask VolatileFamilies,
                         GPRFamilyMask ArgumentFamilies,
                         const std::set<va_t> &DispatchThunks,
                         const std::map<va_t, GPRReadWidths> &FixedEntryReads) {
  CallRegisterSummaries Result;
  // Unknown is absorbing: a function is unknown if it or any callee is.
  std::set<va_t> Unknown;
  for (const auto &[Entry, Effect] : Funcs)
    if (Effect.Unknown)
      Unknown.insert(Entry);
  for (bool Changed = true; Changed;) {
    Changed = false;
    for (const auto &[Entry, Effect] : Funcs) {
      if (Unknown.count(Entry))
        continue;
      for (va_t Callee : Effect.Callees)
        if (!Funcs.count(Callee) || Unknown.count(Callee)) {
          Unknown.insert(Entry);
          Changed = true;
          break;
        }
    }
  }

  // Least fixed point of Writes(F) = Local(F) | Writes(callees); recursion
  // converges because masks only grow.
  std::map<va_t, GPRFamilyMask> &Writes = Result.MayWrite;
  for (const auto &[Entry, Effect] : Funcs)
    if (!Unknown.count(Entry))
      Writes[Entry] = Effect.Writes;
  for (bool Changed = true; Changed;) {
    Changed = false;
    for (auto &[Entry, Mask] : Writes) {
      GPRFamilyMask Next = Mask;
      for (va_t Callee : Funcs.at(Entry).Callees)
        Next |= Writes.at(Callee);
      if (Next != Mask) {
        Mask = Next;
        Changed = true;
      }
    }
  }

  // Entry reads, also a least fixed point: a callee's reads only grow a
  // caller's.  A body we do not fully know has none, and neither does one
  // that tail-calls code whose reads are unknown: that code receives this
  // function's incoming registers.
  std::set<va_t> UnknownReads;
  for (const auto &[Entry, Effect] : Funcs)
    if (!FixedEntryReads.count(Entry) &&
        (Effect.Incomplete || Effect.UnknownEntryReads))
      UnknownReads.insert(Entry);
  for (bool Changed = true; Changed;) {
    Changed = false;
    for (const auto &[Entry, Effect] : Funcs) {
      if (UnknownReads.count(Entry) || FixedEntryReads.count(Entry))
        continue;
      const bool ForwardsToUnknown =
          llvm::any_of(Effect.Blocks, [&](const RegisterBlock &Block) {
            return llvm::any_of(Block.Steps, [&](const RegisterStep &Step) {
              return Step.TailCallee && !FixedEntryReads.count(Step.Callee) &&
                     (!Funcs.count(Step.Callee) ||
                      UnknownReads.count(Step.Callee));
            });
          });
      if (ForwardsToUnknown) {
        UnknownReads.insert(Entry);
        Changed = true;
      }
    }
  }
  std::map<va_t, GPRReadWidths> &Reads = Result.EntryReads;
  for (const auto &[Entry, Effect] : Funcs)
    if (!UnknownReads.count(Entry))
      Reads[Entry] = GPRReadWidths{};
  for (const auto &[Entry, Widths] : FixedEntryReads)
    Reads[Entry] = Widths;
  for (bool Changed = true; Changed;) {
    Changed = false;
    for (auto &[Entry, Widths] : Reads) {
      if (FixedEntryReads.count(Entry) || !Funcs.count(Entry))
        continue;
      GPRReadWidths Next = Widths;
      joinReads(Next, entryLiveWidths(Funcs.at(Entry), Writes, Reads,
                                      VolatileFamilies, ArgumentFamilies,
                                      DispatchThunks));
      if (Next != Widths) {
        Widths = Next;
        Changed = true;
      }
    }
  }

  // Incoming stack arguments, also a least fixed point: a tail call at the
  // entry stack pointer passes them on, so the callee's reads are this
  // function's too. A tail callee without a bounded summary leaves none.
  std::set<va_t> UnknownStack;
  for (const auto &[Entry, Effect] : Funcs)
    if (Effect.UnknownStackReads)
      UnknownStack.insert(Entry);
  for (bool Changed = true; Changed;) {
    Changed = false;
    for (const auto &[Entry, Effect] : Funcs) {
      if (UnknownStack.count(Entry))
        continue;
      if (llvm::any_of(Effect.StackTailCallees, [&](va_t Callee) {
            return !Funcs.count(Callee) || UnknownStack.count(Callee);
          })) {
        UnknownStack.insert(Entry);
        Changed = true;
      }
    }
  }
  for (const auto &[Entry, Effect] : Funcs)
    if (Effect.VariadicFrom >= 0)
      Result.VariadicFrom[Entry] = Effect.VariadicFrom;
  std::map<va_t, int> &StackArgs = Result.EntryStackArgs;
  for (const auto &[Entry, Effect] : Funcs)
    if (!UnknownStack.count(Entry))
      StackArgs[Entry] = Effect.StackArgs;
  for (bool Changed = true; Changed;) {
    Changed = false;
    for (auto &[Entry, Count] : StackArgs)
      for (va_t Callee : Funcs.at(Entry).StackTailCallees)
        if (StackArgs.at(Callee) > Count) {
          Count = StackArgs.at(Callee);
          Changed = true;
        }
  }
  return Result;
}

} // namespace neverd
