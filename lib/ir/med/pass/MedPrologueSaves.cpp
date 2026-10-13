//===- MedPrologueSaves.cpp - Callee-save prologue storage ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedPrologueSaves.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedStackAlignment.h"

#include "llvm/ADT/STLExtras.h"

#include <map>
#include <set>
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

} // namespace

void markCalleeSavePrologueSaves(MedFunc &Func, Arch Architecture,
                                 BinaryFormat Format, StackEntryKind Entry) {
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
  std::vector<FrameAccess> Loads, Stores;
  std::vector<MedOp *> Accesses;
  auto NoteRead = [&](const MedVar &V, const MedOp *By) {
    if (V.isConst() || V.Id < 0)
      return;
    if (By)
      OpReads[keyOf(V)].push_back(By);
    else
      OtherReads.insert(keyOf(V));
  };
  for (MedBlock &Block : Func.Blocks) {
    for (const PhiNode &Phi : Block.Phis) {
      for (const auto &Incoming : Phi.Args)
        NoteRead(Incoming.second, nullptr);
      if (Phi.ExceptionalEntry)
        NoteRead(*Phi.ExceptionalEntry, nullptr);
    }
    for (MedOp &Op : Block.Ops) {
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        NoteRead(Op.Inputs[I], &Op);
      if ((Op.Opcode == NdOp::LOAD ||
           (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2)) &&
          Op.NumInputs >= 1 && Op.MemoryOrdering == NdMemoryOrdering::None &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default)
        Accesses.push_back(&Op);
    }
  }
  // One proof resolves every address against the entry stack pointer.
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

} // namespace neverd
