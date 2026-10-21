//===- ImportedMemoryCopy.h - Native imported copy reads --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Authenticates a bounded AArch64 memcpy/memmove source read from native
/// instructions and loader import identity. Shared by address proof creation
/// and replay; a local function's spelling supplies no such authority.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_IMPORTEDMEMORYCOPY_H
#define NEVERD_IR_LOW_IMPORTEDMEMORYCOPY_H

#include "neverd/ir/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/Support/Endian.h"

#include <optional>

namespace neverd {

struct ImportedMemoryCopyRead {
  NdVar Source;
  NdVar Length;
  uint16_t Size = 0;
  va_t LengthInstructionAddr = InvalidVA;
  int LengthOpSeq = -1;
};

std::optional<va_t> exactImmutableDataSpanOwner(const BinaryImage &Image,
                                                va_t Start, uint64_t Size,
                                                va_t ExpectedOwner);

/// The instruction map must expose decoded records with Addr, Size, Ops and
/// the CFG builder's control-boundary flags. Accept only a direct native BL
/// to a loader-owned copy import and a literal MOVZ length in the same bounded
/// straight-line window. The caller separately proves all-path source/length
/// flow and immutable ownership of the complete read span.
template <typename InstructionMap>
std::optional<ImportedMemoryCopyRead>
importedMemoryCopyRead(const BinaryImage &Image, const InstructionMap &Insns,
                       const LowOp &Call) {
  if (Image.Arch != Arch::AArch64 || Image.IsRelocatable ||
      Call.Opcode != NdOp::CALL || Call.NumInputs != 1 ||
      !Call.Inputs[0].isConst() || Call.Inputs[0].Size != 8)
    return std::nullopt;
  const auto *Import = Image.findImportStubAt(Call.Inputs[0].Offset);
  if (!Import || !libc::isMemCopyName(Import->Name))
    return std::nullopt;
  const auto *CallBytes = Image.readVA(Call.Addr, 4);
  if (!CallBytes)
    return std::nullopt;
  const uint32_t Branch = llvm::support::endian::read32le(CallBytes);
  if ((Branch & 0xfc000000u) != 0x94000000u)
    return std::nullopt;
  int64_t Displacement = int64_t(Branch & 0x03ffffffu) << 2;
  if (Displacement & (int64_t{1} << 27))
    Displacement -= int64_t{1} << 28;
  if (Call.Addr + uint64_t(Displacement) != Call.Inputs[0].Offset)
    return std::nullopt;

  const auto &TRI = getTargetRegInfo(Image.Arch);
  const auto Registers = TRI.integerParamRegs(Image.abiFormat());
  if (Registers.size() < 3)
    return std::nullopt;
  const NdVar Length = NdVar::reg(Registers[2], 8);
  auto It = Insns.find(Call.Addr);
  if (It == Insns.end() || It->second.Size != 4 || !It->second.IsCall ||
      It->second.IsInstructionGuard || It->second.IsRet ||
      It->second.IsOpaqueTerminator || It->second.IsResumableTerminator ||
      It->second.Ops.size() >
          ::neverd::limits::ir::kImportedMemoryCopyReadItSecondOpsSizeLimit ||
      Call.Addr > InvalidVA - 4)
    return std::nullopt;
  bool FoundCall = false;
  for (const LowOp &Op : It->second.Ops) {
    if (Op.Seq == Call.Seq && Op.Addr == Call.Addr && Op.Opcode == NdOp::CALL) {
      if (FoundCall || Op.NumInputs != Call.NumInputs ||
          Op.Inputs[0] != Call.Inputs[0] || Op.Output != Call.Output)
        return std::nullopt;
      FoundCall = true;
      continue;
    }
    if (FoundCall || Op.Opcode != NdOp::COPY || Op.NumInputs != 1 ||
        Op.Output != NdVar::reg(TRI.LinkRegister, 8) ||
        !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 8 ||
        Op.Inputs[0].Offset != Call.Addr + 4)
      return std::nullopt;
  }
  if (!FoundCall)
    return std::nullopt;
  va_t Next = Call.Addr;
  for (unsigned Visited = 0; It != Insns.begin() && Visited != 32; ++Visited) {
    --It;
    const auto &Record = It->second;
    if (Record.Size != 4 || Record.Addr > InvalidVA - Record.Size ||
        Record.Addr + Record.Size != Next || Record.IsInstructionGuard ||
        Record.IsBranch || Record.IsCall || Record.IsRet ||
        Record.IsOpaqueTerminator || Record.IsResumableTerminator ||
        Record.Ops.size() >
            ::neverd::limits::ir::kImportedMemoryCopyReadRecordOpsSizeLimit)
      return std::nullopt;
    Next = Record.Addr;
    for (auto Op = Record.Ops.rbegin(); Op != Record.Ops.rend(); ++Op) {
      if (Op->Output.isReg() && Op->Output.Offset > InvalidVA - Op->Output.Size)
        return std::nullopt;
      if (!Op->Output.isReg() || Op->Output.Size == 0 ||
          Op->Output.Offset >= Length.Offset + Length.Size ||
          Length.Offset >= Op->Output.Offset + Op->Output.Size)
        continue;
      if (Op->Output != Length || Op->Opcode != NdOp::COPY ||
          Op->NumInputs != 1 || !Op->Inputs[0].isConst() ||
          (Op->Inputs[0].Provenance != ConstantAddressProvenance::Scalar &&
           Op->Inputs[0].Provenance != ConstantAddressProvenance::Unknown) ||
          Op->Inputs[0].Offset == 0 || Op->Inputs[0].Offset > UINT16_MAX)
        return std::nullopt;
      if (Image.InstructionAddressMaterializations.count(Op->Addr))
        return std::nullopt;
      for (const auto &Relocation : Image.Relocations)
        if (Relocation.Address >= Op->Addr && Relocation.Address - Op->Addr < 4)
          return std::nullopt;
      const auto *Bytes = Image.readVA(Op->Addr, 4);
      if (!Bytes)
        return std::nullopt;
      const uint32_t Word = llvm::support::endian::read32le(Bytes);
      const unsigned Shift = ((Word >> 21) & 3u) * 16;
      if ((Word & 0x7f800000u) != 0x52800000u ||
          (Word & 31u) != Registers[2] / 8 ||
          (!(Word & 0x80000000u) && Shift >= 32) ||
          (uint64_t((Word >> 5) & 0xffffu) << Shift) != Op->Inputs[0].Offset)
        return std::nullopt;
      return ImportedMemoryCopyRead{NdVar::reg(Registers[1], 8), Length,
                                    static_cast<uint16_t>(Op->Inputs[0].Offset),
                                    Op->Addr, Op->Seq};
    }
  }
  return std::nullopt;
}

/// Replay the source-read witness against current native bytes, import
/// identity, length setup and complete immutable section ownership.
template <typename InstructionMap>
bool importedMemoryCopyDereferenceMatches(
    const BinaryImage &Image, const InstructionMap &Insns, const LowOp &Call,
    const RelocatedInstructionAddressOccurrence &Occurrence,
    const NdVar &CurrentValue, va_t CurrentAddress, va_t ExpectedInstruction) {
  const auto Read = importedMemoryCopyRead(Image, Insns, Call);
  return Read && Call.Addr == ExpectedInstruction &&
         Call.Addr == Occurrence.DereferenceInstructionAddr &&
         Call.Seq == Occurrence.DereferenceOpSeq &&
         Occurrence.DereferenceOpcode == NdOp::CALL &&
         Read->Source == CurrentValue &&
         Read->Source == Occurrence.DereferenceAddressWitness &&
         Read->Size == Occurrence.DereferenceAccessSize &&
         exactImmutableDataSpanOwner(Image, CurrentAddress, Read->Size,
                                     Occurrence.TargetOwnerVA)
             .has_value();
}

} // namespace neverd

#endif // NEVERD_IR_LOW_IMPORTEDMEMORYCOPY_H
