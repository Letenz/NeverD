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
