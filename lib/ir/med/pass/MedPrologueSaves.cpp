//===- MedPrologueSaves.cpp - Callee-save prologue storage ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedPrologueSaves.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedStackAlignment.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"

#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {

bool isEntryLiveInValue(const MedFunc &Func, const MedVar &V, uint64_t RegOff) {
  if (Func.Blocks.empty())
    return false;
  // An entry seed copies the incoming register.  SSA may give the copy a new
  // version (`COPY RCX.1 = RCX` when CL is seeded too) while later reads
  // still name the incoming one; both sides are the incoming value.
  for (const MedOp &Op : Func.Blocks.front().Ops) {
    if (Op.Opcode != NdOp::COPY)
      break;
    if (Op.Output.Kind == MedVar::Reg && Op.Output.RegOff == RegOff &&
        Op.NumInputs >= 1 && Op.Inputs[0].Kind == MedVar::Reg &&
        Op.Inputs[0].Id == Op.Output.Id && Op.Inputs[0].SSAVer == 0 &&
        (Op.Output == V || Op.Inputs[0] == V))
      return true;
  }
  return false;
}

namespace {

using ValueKey = std::pair<int, int>;

ValueKey keyOf(const MedVar &V) { return {V.Id, V.SSAVer}; }

/// A load or store of the bytes at Offset from the entry stack pointer.
struct FrameAccess {
  int64_t Offset = 0;
  uint16_t Size = 0;
  MedOp *Op = nullptr;

  bool overlaps(int64_t Begin, int64_t End) const {
    return Offset < End && Begin < Offset + Size;
  }
};

/// Every plain load and store of \p Func whose address resolves against the
/// entry stack pointer, all by one proof.
void frameAccessesOf(MedFunc &Func, Arch Architecture, BinaryFormat Format,
                     StackEntryKind Entry, std::vector<FrameAccess> &Loads,
                     std::vector<FrameAccess> &Stores) {
  std::vector<MedOp *> Accesses;
  for (MedBlock &Block : Func.Blocks)
    for (MedOp &Op : Block.Ops)
      if ((Op.Opcode == NdOp::LOAD ||
           (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2)) &&
          Op.NumInputs >= 1 && Op.MemoryOrdering == NdMemoryOrdering::None &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default)
        Accesses.push_back(&Op);
  std::vector<MedVar> Addresses;
  Addresses.reserve(Accesses.size());
  for (const MedOp *Op : Accesses)
    Addresses.push_back(Op->Inputs[0]);
  const std::vector<std::optional<int64_t>> Offsets =
      entryStackOffsets(Func, Addresses, Architecture, Format, Entry);
  for (size_t I = 0; I < Accesses.size(); ++I) {
    if (!Offsets[I])
      continue;
    MedOp &Op = *Accesses[I];
    if (Op.Opcode == NdOp::LOAD)
      Loads.push_back({*Offsets[I], Op.Output.Size, &Op});
    else
      Stores.push_back({*Offsets[I], Op.Inputs[1].Size, &Op});
  }
}

void markCalleeSavePrologueSaves(MedFunc &Func, Arch Architecture,
                                 BinaryFormat Format, StackEntryKind Entry,
                                 std::vector<FrameAccess> &Loads,
                                 std::vector<FrameAccess> &Stores) {
  const auto &TRI = getTargetRegInfo(Architecture);
  const BinaryFormat Convention =
      Func.CC == CallingConv::Win64 ? BinaryFormat::COFF : BinaryFormat::ELF;
  // Win64 also hands the callee its register arguments' home slots, above the
  // return address: MSVC saves nonvolatile registers there.
  const IntegerArgumentLayout Layout =
      TRI.integerArgumentLayout(Convention == BinaryFormat::COFF);
  const int64_t HomeBegin = Layout.EntryStackBase - Layout.CallStackBase;

  // Every read of a value: the operations and joins that take it.
  std::map<ValueKey, std::vector<const MedOp *>> OpReads;
  std::set<ValueKey> OtherReads;
  auto NoteRead = [&](const MedVar &V, const MedOp *By) {
    if (V.isConst() || V.Id < 0)
      return;
    if (By)
      OpReads[keyOf(V)].push_back(By);
    else
      OtherReads.insert(keyOf(V));
  };
  for (const MedBlock &Block : Func.Blocks) {
    for (const PhiNode &Phi : Block.Phis) {
      for (const auto &Incoming : Phi.Args)
        NoteRead(Incoming.second, nullptr);
      if (Phi.ExceptionalEntry)
        NoteRead(*Phi.ExceptionalEntry, nullptr);
    }
    for (const MedOp &Op : Block.Ops)
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        NoteRead(Op.Inputs[I], &Op);
  }
  frameAccessesOf(Func, Architecture, Format, Entry, Loads, Stores);
  for (const MedCallClobber &Clobber : Func.CallClobbers) {
    NoteRead(Clobber.Value, nullptr);
    NoteRead(Clobber.PreservedInput, nullptr);
  }
  auto Unread = [&](const MedVar &V) {
    return !OpReads.count(keyOf(V)) && !OtherReads.count(keyOf(V));
  };

  std::vector<MedOp *> Saves;
  for (const FrameAccess &Save : Stores) {
    const MedVar &Saved = Save.Op->Inputs[1];
    if (Saved.Kind != MedVar::Reg || !Saved.Size ||
        !isEntryLiveInValue(Func, Saved, Saved.RegOff) ||
        !(TRI.isCallPreserved(Saved.RegOff, Saved.Size, Convention) ||
          TRI.isFramePointer(Saved.RegOff) || TRI.isLinkRegister(Saved.RegOff)))
      continue;
    const int64_t End = Save.Offset + Saved.Size;
    const bool OwnFrame =
        Func.FrameSize > 0 && Save.Offset >= -Func.FrameSize && End <= 0;
    const bool Home = Save.Offset >= HomeBegin && End <= Layout.EntryStackBase;
    if (!OwnFrame && !Home)
      continue;
    // A later store into the same bytes makes them storage the body uses.
    if (llvm::any_of(Stores, [&](const FrameAccess &Other) {
          return Other.Op != Save.Op && Other.overlaps(Save.Offset, End);
        }))
      continue;
    auto SameRegister = [&](const MedVar &V) {
      return V.Kind == MedVar::Reg && V.RegOff == Saved.RegOff &&
             V.Size == Saved.Size;
    };
    // Each read of the bytes is a restore nothing uses: a load into that
    // register, or into a value only a copy into it takes, or a load no one
    // reads.  Those loads read nothing a source statement uses either.
    const bool OnlyRestored = llvm::all_of(Loads, [&](const FrameAccess &Read) {
      if (!Read.overlaps(Save.Offset, End))
        return true;
      if (Read.Offset != Save.Offset || Read.Size != Saved.Size)
        return false;
      const MedVar &Out = Read.Op->Output;
      if (Unread(Out))
        return true;
      const auto It = OpReads.find(keyOf(Out));
      if (SameRegister(Out) || OtherReads.count(keyOf(Out)) ||
          It == OpReads.end() || It->second.size() != 1)
        return false;
      const MedOp &Copy = *It->second.front();
      return Copy.Opcode == NdOp::COPY && Copy.NumInputs == 1 &&
             SameRegister(Copy.Output) && Unread(Copy.Output);
    });
    if (OnlyRestored)
      Saves.push_back(Save.Op);
  }
  for (MedOp *Save : Saves)
    Save->PrologueSave = true;
}

bool hasStackReturnAddress(Arch Architecture, StackEntryKind Entry) {
  const auto &TRI = getTargetRegInfo(Architecture);
  return TRI.CallPushesReturnAddress && Entry == StackEntryKind::Call &&
         TRI.PointerSize;
}

void markReturnAddressReads(unsigned PointerSize,
                            llvm::ArrayRef<FrameAccess> Loads,
                            llvm::ArrayRef<FrameAccess> Stores) {
  // A store into the slot replaces the return address, as a function that
  // returns somewhere else does.
  if (llvm::any_of(Stores, [&](const FrameAccess &Write) {
        return Write.overlaps(0, PointerSize);
      }))
    return;
  for (const FrameAccess &Read : Loads)
    if (Read.Offset == 0 && Read.Size == PointerSize)
      Read.Op->ReturnAddressRead = true;
}

/// Saving an incoming register into scratch storage does not read its value
/// when every path overwrites the bytes before observing them. Keep the proof
/// in source preparation so HighC and LLVMC discard the same transport. Calls,
/// opaque memory accesses and exceptional edges stop the proof; a partial
/// overwrite kills only its own bytes. Operation positions remain unchanged.
void markUnobservedEntrySpills(MedFunc &Func, Arch Architecture,
                               BinaryFormat Format,
                               llvm::ArrayRef<FrameAccess> Loads,
                               llvm::ArrayRef<FrameAccess> Stores) {
  if (Func.Blocks.empty() || Func.SkippedSSA || Func.AnalystStackFrame)
    return;
  const auto &TRI = getTargetRegInfo(Architecture);
  const auto Layout = TRI.integerArgumentLayout(Format);
  const int64_t HomeBegin = Layout.EntryStackBase - Layout.CallStackBase;
  const auto Private = [&](int64_t At, uint16_t Bytes) {
    return Bytes && ((Func.FrameSize > 0 && At >= -Func.FrameSize && At < 0 &&
                      At + Bytes <= 0) ||
                     (Layout.CallStackBase && At >= HomeBegin &&
                      At < Layout.EntryStackBase &&
                      Bytes <= Layout.EntryStackBase - At));
  };
  std::map<const MedOp *, const FrameAccess *> Accesses;
  for (const FrameAccess &Access : Loads)
    Accesses.emplace(Access.Op, &Access);
  for (const FrameAccess &Access : Stores)
    Accesses.emplace(Access.Op, &Access);
  std::map<int, size_t> Blocks;
  std::set<va_t> BlockEntries;
  std::map<const MedOp *, std::pair<size_t, size_t>> Positions;
  for (size_t B = 0; B < Func.Blocks.size(); ++B) {
    Blocks.emplace(Func.Blocks[B].Id, B);
    BlockEntries.insert(Func.Blocks[B].StartAddr);
    for (size_t I = 0; I < Func.Blocks[B].Ops.size(); ++I) {
      const MedOp &Op = Func.Blocks[B].Ops[I];
      Positions.emplace(&Op, std::pair{B, I});
    }
  }
  size_t Budget = limits::ir::kSourceEntrySpillWork;
  for (const FrameAccess &Save : Stores) {
    const MedVar &Value = Save.Op->Inputs[1];
    if (Save.Op->PrologueSave || Value.Kind != MedVar::Reg ||
        Value.Size > sizeof(uint64_t) || !Private(Save.Offset, Value.Size) ||
        !isEntryLiveInValue(Func, Value, Value.RegOff))
      continue;
    const auto [FirstBlock, FirstOp] = Positions.at(Save.Op);
    const uint64_t All = (uint64_t{1} << Value.Size) - 1;
    using State = std::tuple<size_t, size_t, uint64_t>;
    std::vector<State> Work{{FirstBlock, FirstOp + 1, All}};
    std::map<std::pair<size_t, size_t>, uint64_t> Seen;
    bool Safe = true;
    while (Safe && !Work.empty()) {
      auto [B, Begin, Pending] = Work.back();
      Work.pop_back();
      auto &Visited = Seen[{B, Begin}];
      Pending &= ~Visited;
      if (!Pending)
        continue;
      Visited |= Pending;
      const MedBlock &Block = Func.Blocks[B];
      if (!Block.ExceptionalSuccs.empty()) {
        Safe = false;
        break;
      }
      bool Returned = false;
      for (size_t I = Begin; Pending && I < Block.Ops.size(); ++I) {
        if (!Budget) {
          Safe = false;
          break;
        }
        --Budget;
        const MedOp &Op = Block.Ops[I];
        if (Op.MemoryOrdering != NdMemoryOrdering::None ||
            Op.MemoryAddressSpace != NdMemoryAddressSpace::Default) {
          Safe = false;
          break;
        }
        if (Op.Opcode == NdOp::STORE || Op.Opcode == NdOp::LOAD) {
          const auto At = Accesses.find(&Op);
          if (At == Accesses.end() ||
              !Private(At->second->Offset, At->second->Size)) {
            Safe = false;
            break;
          }
          uint64_t Overlap = 0;
          for (unsigned Byte = 0; Byte < Value.Size; ++Byte)
            if (At->second->overlaps(Save.Offset + Byte,
                                     Save.Offset + Byte + 1))
              Overlap |= uint64_t{1} << Byte;
          if (Op.Opcode == NdOp::LOAD && (Pending & Overlap)) {
            Safe = false;
            break;
          }
          if (Op.Opcode == NdOp::STORE)
            Pending &= ~Overlap;
        } else if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
                   Op.Opcode == NdOp::INTRINSIC ||
                   Op.Opcode == NdOp::INDIR_BR ||
                   Op.Opcode == NdOp::ATOMIC_XCHG ||
                   Op.Opcode == NdOp::ATOMIC_ADD ||
                   Op.Opcode == NdOp::ATOMIC_CMPXCHG) {
          Safe = false;
          break;
        } else if (Op.Opcode == NdOp::RETURN) {
          // An unresolved result may return a frame pointer, including a PHI
          // of different offsets. Only a scalar constant or void return proves
          // the remaining bytes cannot escape through the result.
          for (unsigned K = 0; K < Op.NumInputs; ++K)
            Safe &= Op.Inputs[K].isConst();
          Returned = true;
          break;
        } else if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR) {
          if (!Op.NumInputs || !Op.Inputs[0].isConst() ||
              !BlockEntries.count(Op.Inputs[0].ConstVal)) {
            Safe = false;
            break;
          }
        } else {
          // These are scalar moves, address calculations and flag updates,
          // without an implicit memory observer or a trapping operation.
          switch (Op.Opcode) {
          case NdOp::NOP:
          case NdOp::COPY:
          case NdOp::INT_ADD:
          case NdOp::INT_SUB:
          case NdOp::INT_MULT:
          case NdOp::INT_AND:
          case NdOp::INT_OR:
          case NdOp::INT_XOR:
          case NdOp::INT_LEFT:
          case NdOp::INT_RIGHT:
          case NdOp::INT_ASHR:
          case NdOp::INT_EQUAL:
          case NdOp::INT_NOTEQUAL:
          case NdOp::INT_LESS:
          case NdOp::INT_LESSEQUAL:
          case NdOp::INT_SLESS:
          case NdOp::INT_SLESSEQUAL:
          case NdOp::INT_ZEXT:
          case NdOp::INT_SEXT:
          case NdOp::INT_NEGATE:
          case NdOp::INT_NEG2:
          case NdOp::INT_NOT:
          case NdOp::INT_CARRY:
          case NdOp::INT_SOVF:
          case NdOp::INT_SBOR:
          case NdOp::BOOL_AND:
          case NdOp::BOOL_OR:
          case NdOp::BOOL_XOR:
          case NdOp::BOOL_NOT:
          case NdOp::CONCAT:
          case NdOp::SUBBYTES:
          case NdOp::SELECT:
            break;
          default:
            Safe = false;
            break;
          }
          if (!Safe)
            break;
        }
      }
      if (!Safe || !Pending || Returned)
        continue;
      if (Block.Succs.empty()) {
        Safe = false;
        break;
      }
      for (int Successor : Block.Succs) {
        const auto Next = Blocks.find(Successor);
        if (Next == Blocks.end()) {
          Safe = false;
          break;
        }
        Work.emplace_back(Next->second, 0, Pending);
      }
    }
    if (Safe)
      Save.Op->PrologueSave = true;
    if (!Budget)
      return;
  }
}

/// An exact declared call operand observes every loaded value byte. A source
/// frame has no initial contents below its entry SP. Prove absence
/// of a write, rather than mistaking absence of a known producer for that
/// proof. Unioning writes from every predecessor is deliberately conservative:
/// one path writing a byte prevents this proof from calling it unwritten.
void replaceUnwrittenCallFrameReads(MedFunc &Func, Arch Architecture,
                                    llvm::ArrayRef<FrameAccess> Loads,
                                    llvm::ArrayRef<FrameAccess> Stores) {
  if (Func.Blocks.empty() || Func.FrameSize <= 0 || Func.SkippedSSA ||
      Func.AnalystStackFrame)
    return;
  size_t Remaining = limits::ir::kUnwrittenSourceFrameReadWork;
  std::set<std::pair<ValueKey, uint16_t>> Required;
  for (const auto &Block : Func.Blocks) {
    if (Remaining < Block.Ops.size())
      return;
    Remaining -= Block.Ops.size();
    for (const auto &Op : Block.Ops) {
      if (!hasBoundIntegerCallArguments(Op))
        continue;
      if (Remaining < Op.NumInputs)
        return;
      Remaining -= Op.NumInputs;
      for (size_t I = 0; I < Op.IntegerCallSignature->Parameters.size(); ++I) {
        const auto &Parameter = Op.IntegerCallSignature->Parameters[I];
        const auto &Value = Op.Inputs[I + 1];
        if (Parameter.Kind == SourceABICarrierKind::Stack && !Value.isConst() &&
            Value.Id >= 0 && Value.Size == Parameter.Bytes)
          Required.emplace(keyOf(Value), Value.Size);
      }
    }
  }
  // General loads may have only their initialized bits observed later. Their
  // partial-byte source model is separate from a full declared argument read.
  if (Required.empty() || Remaining < Stores.size())
    return;
  Remaining -= Stores.size();
  std::map<int, const MedBlock *> Blocks;
  std::map<const MedOp *, std::pair<int, size_t>> Positions;
  std::map<const MedOp *, const FrameAccess *> Writes;
  std::set<std::pair<int, int>> IncomingEdges, OutgoingEdges;
  for (const auto &Write : Stores)
    Writes.emplace(Write.Op, &Write);
  for (const auto &Block : Func.Blocks) {
    if (Remaining <
            1 + Block.Ops.size() + Block.Preds.size() + Block.Succs.size() ||
        !Blocks.emplace(Block.Id, &Block).second)
      return;
    Remaining -= 1 + Block.Ops.size() + Block.Preds.size() + Block.Succs.size();
    for (size_t I = 0; I < Block.Ops.size(); ++I)
      Positions.emplace(&Block.Ops[I], std::pair{Block.Id, I});
    for (int Pred : Block.Preds)
      IncomingEdges.emplace(Pred, Block.Id);
    for (int Succ : Block.Succs)
      OutgoingEdges.emplace(Block.Id, Succ);
  }
  // Exceptional or incomplete graph roots cannot inherit the ordinary frame.
  if (IncomingEdges != OutgoingEdges)
    return;
  for (const auto &[From, To] : IncomingEdges)
    if (!Blocks.count(From) || !Blocks.count(To))
      return;
  for (const auto &Block : Func.Blocks)
    if (!Block.ExceptionalPreds.empty() || !Block.ExceptionalSuccs.empty())
      return;
  for (const auto &Read : Loads) {
    if (!Required.count({keyOf(Read.Op->Output), Read.Size}) || Read.Op->Dead ||
        Read.Op->ReturnAddressRead || !Read.Size ||
        Read.Offset < -Func.FrameSize || Read.Offset > -int64_t(Read.Size))
      continue;
    if (Remaining < Read.Size)
      return;
    Remaining -= Read.Size;
    std::vector<bool> Written(Read.Size);
    const auto Position = Positions.at(Read.Op);
    std::vector<std::pair<int, size_t>> Pending{Position};
    std::set<int> Visited;
    bool Complete = true, ReachedEntry = false;
    while (Complete && !Pending.empty()) {
      const auto [Id, End] = Pending.back();
      Pending.pop_back();
      if (!Remaining)
        return;
      --Remaining;
      const auto &Block = *Blocks.at(Id);
      if (!Visited.insert(Id).second) {
        // A backedge to the query also executes its suffix before another
        // observation. Do not publish a first-iteration-only conclusion.
        if (Id == Position.first)
          Complete = false;
        continue;
      }
      ReachedEntry |= Id == Func.Blocks.front().Id;
      if (Block.Preds.empty() && Id != Func.Blocks.front().Id) {
        Complete = false;
        break;
      }
      if (Remaining < End + Block.Preds.size())
        return;
      Remaining -= End + Block.Preds.size();
      for (size_t I = 0; I < End; ++I) {
        const auto &Op = Block.Ops[I];
        if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
            Op.Opcode == NdOp::INTRINSIC || Op.Opcode == NdOp::ATOMIC_XCHG ||
            Op.Opcode == NdOp::ATOMIC_ADD ||
            Op.Opcode == NdOp::ATOMIC_CMPXCHG) {
          Complete = false;
          break;
        }
        if (Op.Opcode != NdOp::STORE)
          continue;
        const auto Write = Writes.find(&Op);
        if (Write == Writes.end()) {
          Complete = false; // An unresolved address might alias this frame.
          break;
        }
        if (Remaining < Read.Size)
          return;
        Remaining -= Read.Size;
        for (size_t Byte = 0; Byte < Read.Size; ++Byte)
          Written[Byte] = Written[Byte] || Write->second->overlaps(
                                               Read.Offset + int64_t(Byte),
                                               Read.Offset + int64_t(Byte) + 1);
      }
      for (int Pred : Block.Preds)
        Pending.emplace_back(Pred, Blocks.at(Pred)->Ops.size());
    }
    if (Complete && ReachedEntry && llvm::is_contained(Written, false)) {
      // Keep the same definition and operation position: call records name
      // both. Native lifting never invokes this source-only preparation.
      Read.Op->Opcode = NdOp::COPY;
      Read.Op->NumInputs = 1;
      Read.Op->Inputs[0] = MedVar::makeUnspecified(Read.Size, Architecture);
    }
  }
}

} // namespace

void markCalleeSavePrologueSaves(MedFunc &Func, Arch Architecture,
                                 BinaryFormat Format, StackEntryKind Entry) {
  std::vector<FrameAccess> Loads, Stores;
  markCalleeSavePrologueSaves(Func, Architecture, Format, Entry, Loads, Stores);
}

void markReturnAddressReads(MedFunc &Func, Arch Architecture,
                            BinaryFormat Format, StackEntryKind Entry) {
  if (!hasStackReturnAddress(Architecture, Entry))
    return;
  std::vector<FrameAccess> Loads, Stores;
  frameAccessesOf(Func, Architecture, Format, Entry, Loads, Stores);
  markReturnAddressReads(getTargetRegInfo(Architecture).PointerSize, Loads,
                         Stores);
}

void markSourceStackAccesses(MedFunc &Func, Arch Architecture,
                             BinaryFormat Format, StackEntryKind Entry) {
  std::vector<FrameAccess> Loads, Stores;
  markCalleeSavePrologueSaves(Func, Architecture, Format, Entry, Loads, Stores);
  // Annotate before source-only replacement. No access proof survives the
  // replacements at the end of this preparation.
  if (hasStackReturnAddress(Architecture, Entry))
    markReturnAddressReads(getTargetRegInfo(Architecture).PointerSize, Loads,
                           Stores);
  markUnobservedEntrySpills(Func, Architecture, Format, Loads, Stores);
  replaceUnwrittenCallFrameReads(Func, Architecture, Loads, Stores);
}

} // namespace neverd
