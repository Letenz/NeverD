//===- AArch64Exclusive.cpp - Exclusive instruction admission -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AArch64Exclusive.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"

namespace neverd::emulation {
namespace {
#define NEVERD_AARCH64_FIELD(Name, Value) constexpr unsigned Name = Value;
#include "CheckedAArch64Instructions.def"
#undef NEVERD_AARCH64_FIELD
using Operation = AArch64ExclusiveInstruction::Operation;
struct Encoding {
  unsigned ID;
  uint32_t Mask, Value;
  Operation Kind;
  unsigned Width, Count;
};
constexpr Encoding Encodings[] = {
#define NEVERD_AARCH64_EXCLUSIVE(Name, Mask, Value, Kind, Width, Count)        \
  {AARCH64_INS_##Name, Mask, Value, Operation::Kind, Width, Count},
#include "AArch64ExclusiveInstructions.def"
#undef NEVERD_AARCH64_EXCLUSIVE
};
} // namespace
llvm::Expected<std::optional<AArch64ExclusiveInstruction>>
decodeAArch64Exclusive(const cs_insn &I, const AArch64MachineState &CPU) {
  if (I.size != aarch64::InstructionBytes || !I.detail)
    return llvm::make_error<UnsupportedExecutionError>();
  const uint32_t Word = llvm::support::endian::read32le(I.bytes);
  for (const auto &E : Encodings)
    if (I.id == E.ID && (Word & E.Mask) == E.Value)
      return decodeAArch64Exclusive(Word, CPU);
  return std::nullopt;
}
bool isAArch64Exclusive(uint32_t Word) {
  return llvm::any_of(
      Encodings, [Word](const auto &E) { return (Word & E.Mask) == E.Value; });
}
llvm::Expected<std::optional<AArch64ExclusiveInstruction>>
decodeAArch64Exclusive(uint32_t Word, const AArch64MachineState &CPU) {
  for (const auto &E : Encodings) {
    if ((Word & E.Mask) != E.Value)
      continue;
    const unsigned First = Word & RegisterMask;
    const unsigned Second = (Word >> SecondShift) & RegisterMask;
    const unsigned Base = (Word >> BaseShift) & RegisterMask;
    const unsigned Status = (Word >> IndexShift) & RegisterMask;
    if ((E.Kind == Operation::Load && E.Count == 2 && First == Second) ||
        (E.Kind == Operation::Store &&
         (Status == First || (E.Count == 2 && Status == Second) ||
          (Status == Base && Base != aarch64::GPRCount))))
      return llvm::make_error<UnsupportedExecutionError>();
    return AArch64ExclusiveInstruction{E.Kind,
                                       E.Width,
                                       E.Count,
                                       First,
                                       Second,
                                       Base,
                                       Status,
                                       Base == aarch64::GPRCount
                                           ? CPU.reg(AArch64Register::SP)
                                           : CPU.Registers[Base]};
  }
  return std::nullopt;
}
bool isAArch64ExclusiveAlignmentFault(const BackendFault &Fault) {
  return Fault.Kind == BackendFaultKind::Alignment &&
         Fault.Cause == BackendFaultCause::OperandAlignment && Fault.Address &&
         Fault.Size && llvm::isPowerOf2_64(*Fault.Size) &&
         *Fault.Size <= aarch64::ExclusiveGranule &&
         *Fault.Address % *Fault.Size &&
         (Fault.Access == BackendAccessKind::Read ||
          Fault.Access == BackendAccessKind::Write) &&
         !Fault.Interrupt && !Fault.ErrorCode;
}
} // namespace neverd::emulation
