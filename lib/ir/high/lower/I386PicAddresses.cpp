//===- I386PicAddresses.cpp - i386 ELF GOT-relative data addresses --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// i386 has no PC-relative data addressing, so position-independent code
/// finds its data through the GOT: `call $+5; pop ebx; add ebx, GOTPC` puts
/// the GOT's address in a register, and `[ebx + table@GOTOFF]` reads the
/// table.  The CFG proves which register value is that GOT base, and an
/// unlinked object places the GOT at address zero (MedIR's
/// I386ELFGOTBaseZero model).  The base then converts to zero, and the
/// constant a GOT-relative address adds to it is the address of the data it
/// names, so the C reads the data object instead of a number.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/high/MedToHigh.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <iterator>
#include <set>
#include <utility>

namespace neverd {

void MedToHighConverter::collectI386GotBase(const MedFunc &Med) {
  I386GotBase.clear();
  I386GotRooted.clear();
  for (const MedScalarAddressModel &Model : Med.ScalarAddressModels)
    if (Model.Model == RelocatedInstructionScalarModelOccurrence::ModelKind::
                           I386ELFGOTBaseZero &&
        !Model.Value.isConst() && Model.Value.Size)
      I386GotBase.insert(varKey(Model.Value));
  if (I386GotBase.empty())
    return;
  auto IsBase = [&](const MedVar &V) {
    return !V.isConst() && I386GotBase.count(varKey(V));
  };
  auto IsRooted = [&](const MedVar &V) {
    return IsBase(V) || (!V.isConst() && I386GotRooted.count(varKey(V)));
  };
  // A copy of the base is the base.  A sum of the base and terms that are no
  // constants (an index) is rooted at it: a constant it adds later is still
  // the GOT-relative displacement.  SSA defines each value once, so the walk
  // ends when no value changes kind.
  for (bool Changed = true; Changed;) {
    Changed = false;
    for (const MedBlock &Block : Med.Blocks)
      for (const MedOp &Op : Block.Ops) {
        if (!Op.Output.Size || Op.Output.isConst() || IsRooted(Op.Output))
          continue;
        if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
            Op.Inputs[0].Size == Op.Output.Size && IsBase(Op.Inputs[0])) {
          I386GotBase.insert(varKey(Op.Output));
          Changed = true;
        } else if (Op.Opcode == NdOp::INT_ADD && Op.NumInputs == 2 &&
                   IsRooted(Op.Inputs[0]) != IsRooted(Op.Inputs[1]) &&
                   !Op.Inputs[IsRooted(Op.Inputs[0]) ? 1 : 0].isConst()) {
          I386GotRooted.insert(varKey(Op.Output));
          Changed = true;
        }
      }
  }
}

void MedToHighConverter::collectI386GetPcValues(const MedFunc &Med) {
  // LowToMed binds a call-next/POP only where the raw PC is authenticated:
  // outside an unlinked ELF object, whose GOTPC model replaces it.  Reading
  // the bound value as that PC leaves the call's pushed slot without a
  // reader, so the slot does not survive as a local.
  I386GetPcValues.clear();
  std::set<VarKey> Conflicting;
  for (const MedI386GetPcModel &Model : Med.I386GetPcModels)
    for (const MedVar *Value : {&Model.Output, &Model.Value}) {
      if (!Value->Size || Value->isConst())
        continue;
      const VarKey Key = varKey(*Value);
      const auto [It, Inserted] = I386GetPcValues.try_emplace(
          Key, std::make_pair(Model.PCValue, Value->Size));
      if (!Inserted && It->second != std::make_pair(Model.PCValue, Value->Size))
        Conflicting.insert(Key);
    }
  for (const VarKey &Key : Conflicting)
    I386GetPcValues.erase(Key);

  // The pop read the call's push through the same stack pointer value; once
  // the pop reads as the PC, nothing reads the slot, which lies below the
  // stack pointer after the pop.
  I386GetPcPushes.clear();
  if (TargetArch != Arch::X86)
    return;
  // A pop whose value nothing used is gone, and so is its read of the slot:
  // the push of `call $+5` then has no reader at all.  The `pop r32` right
  // after the call is the slot's only reader, since i386 code keeps nothing
  // below its stack pointer; a load of the slot that survived keeps it.
  std::set<VarKey> ReadSlots;
  for (const MedBlock &Block : Med.Blocks)
    for (const MedOp &Op : Block.Ops)
      if (Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1 &&
          !Op.Inputs[0].isConst() && !I386GetPcValues.count(varKey(Op.Output)))
        ReadSlots.insert(varKey(Op.Inputs[0]));
  constexpr uint8_t CallNext[] = {0xE8, 0, 0, 0, 0};
  constexpr uint8_t PopOpcode = 0x58, PopRegisterMask = 0x07;
  for (const MedBlock &Block : Med.Blocks)
    for (const MedOp &Op : Block.Ops) {
      if (Op.Opcode != NdOp::STORE || Op.NumInputs < 2 ||
          Op.Inputs[0].isConst() || ReadSlots.count(varKey(Op.Inputs[0])) ||
          !Op.Inputs[1].isConst() || Op.Inputs[1].Size != 4 ||
          Op.Addr == InvalidVA ||
          Op.Inputs[1].ConstVal != Op.Addr + sizeof(CallNext) || !Image)
        continue;
      const uint8_t *Bytes = Image->readVA(Op.Addr, sizeof(CallNext) + 1);
      if (Bytes &&
          std::equal(std::begin(CallNext), std::end(CallNext), Bytes) &&
          (Bytes[sizeof(CallNext)] & ~PopRegisterMask) == PopOpcode)
        I386GetPcPushes.insert({Op.Addr, Op.OriginSeq});
    }
  for (const MedI386GetPcModel &Model : Med.I386GetPcModels) {
    if (!I386GetPcValues.count(varKey(Model.Value)))
      continue;
    const MedOp *Pop = uniqueMedDefinition(Model.Value);
    if (!Pop || Pop->Opcode != NdOp::LOAD || Pop->NumInputs != 1 ||
        Pop->Inputs[0].isConst())
      continue;
    const VarKey Slot = varKey(Pop->Inputs[0]);
    for (const MedBlock &Block : Med.Blocks)
      for (const MedOp &Op : Block.Ops)
        if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2 &&
            !Op.Inputs[0].isConst() && varKey(Op.Inputs[0]) == Slot &&
            Op.Inputs[1].isConst() && Op.Inputs[1].Size == Model.Value.Size &&
            Op.Inputs[1].ConstVal == Model.PCValue)
          I386GetPcPushes.insert({Op.Addr, Op.OriginSeq});
  }
}

ExprPtr MedToHighConverter::i386GetPcSum(const MedOp &Op) const {
  if (I386GetPcValues.empty() ||
      (Op.Opcode != NdOp::INT_ADD && Op.Opcode != NdOp::INT_SUB) ||
      Op.NumInputs != 2 || Op.Output.Size == 0 ||
      Op.Output.Size > sizeof(uint64_t))
    return nullptr;
  auto PC = [&](const MedVar &V) -> std::optional<uint64_t> {
    if (V.isConst())
      return std::nullopt;
    const auto It = I386GetPcValues.find(varKey(V));
    if (It == I386GetPcValues.end() || It->second.second != V.Size)
      return std::nullopt;
    return It->second.first;
  };
  auto Scalar = [](const MedVar &V) -> std::optional<uint64_t> {
    if (!V.isConst() || (V.Provenance != ConstantAddressProvenance::Unknown &&
                         V.Provenance != ConstantAddressProvenance::Scalar))
      return std::nullopt;
    return V.ConstVal;
  };
  std::optional<uint64_t> Value;
  if (const auto Base = PC(Op.Inputs[0]), Offset = Scalar(Op.Inputs[1]);
      Base && Offset)
    Value = Op.Opcode == NdOp::INT_ADD ? *Base + *Offset : *Base - *Offset;
  else if (const auto Offset = Scalar(Op.Inputs[0]), Base = PC(Op.Inputs[1]);
           Op.Opcode == NdOp::INT_ADD && Base && Offset)
    Value = *Offset + *Base;
  if (!Value)
    return nullptr;
  const uint64_t Mask = Op.Output.Size >= sizeof(uint64_t)
                            ? ~uint64_t{0}
                            : (uint64_t{1} << (8 * Op.Output.Size)) - 1;
  return HighExpr::makeConst(*Value & Mask, Op.Output.Size,
                             ConstantAddressProvenance::Scalar);
}

std::optional<unsigned>
MedToHighConverter::i386GotDisplacement(const MedOp &Op) const {
  if (I386GotBase.empty() || Op.Opcode != NdOp::INT_ADD || Op.NumInputs != 2)
    return std::nullopt;
  for (unsigned I = 0; I < 2; ++I) {
    const MedVar &Displacement = Op.Inputs[I];
    const MedVar &Base = Op.Inputs[1 - I];
    if (Displacement.isConst() && !Base.isConst() &&
        (Displacement.Provenance == ConstantAddressProvenance::Unknown ||
         Displacement.Provenance == ConstantAddressProvenance::Scalar) &&
        (I386GotBase.count(varKey(Base)) || I386GotRooted.count(varKey(Base))))
      return I;
  }
  return std::nullopt;
}

ExprPtr MedToHighConverter::i386GotRelativeAddress(const MedOp &Op,
                                                   unsigned Displacement) {
  const MedVar &Constant = Op.Inputs[Displacement];
  const MedVar &Base = Op.Inputs[1 - Displacement];
  const uint64_t Mask = Op.Output.Size >= sizeof(uint64_t)
                            ? ~uint64_t{0}
                            : (uint64_t{1} << (8 * Op.Output.Size)) - 1;
  const uint64_t Address = Constant.ConstVal & Mask;
  // Data the image holds is an object to read; anything else stays the
  // number it is, as a GOT slot offset would.
  const Segment *Seg = Image ? Image->getSegmentFor(Address) : nullptr;
  const bool Data = Seg && Seg->isReadable() && !Seg->isExecutable();
  ExprPtr Target = HighExpr::makeConst(
      Address, Op.Output.Size,
      Data ? ConstantAddressProvenance::DataAddress : Constant.Provenance,
      Constant.AddressOwnerVA);
  if (I386GotBase.count(varKey(Base)))
    return Target;
  auto Sum = HighExpr::makeBinop(NdOp::INT_ADD, medvarToExpr(Base), Target);
  Sum->Type = NdType::makeInt(Op.Output.Size, false);
  return Sum;
}

} // namespace neverd
