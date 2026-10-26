//===- AArch64WritebackMemory.h - Original writeback contracts --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// GPR loads pack the completed X value below the base. SIMD loads publish
/// a complete V value and base through typed auxiliary results; stores return
/// the base. No output is published before the original instruction succeeds.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_AARCH64_AARCH64WRITEBACKMEMORY_H
#define NEVERD_IR_INTRINSICS_AARCH64_AARCH64WRITEBACKMEMORY_H

#include "neverd/ir/intrinsics/AArch64/AArch64AddressMemory.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>

namespace neverd {

#define NEVERD_A64_WRITEBACK_VALUE(Type, Name, Value)                          \
  inline constexpr Type Name = Value;
#define NEVERD_A64_WRITEBACK_TEXT(Name, Text)                                  \
  inline constexpr char Name[] = Text;
#include "neverd/ir/intrinsics/AArch64/AArch64WritebackMemory.def"

inline bool isAArch64WritebackMemoryIntrinsic(Intrinsic Id) {
  return Id == Intrinsic::A64_LoadWriteback ||
         Id == Intrinsic::A64_StoreWriteback ||
         Id == Intrinsic::A64_VectorLoadWriteback ||
         Id == Intrinsic::A64_VectorStoreWriteback;
}

inline bool isAArch64WritebackStoreIntrinsic(Intrinsic Id) {
  return Id == Intrinsic::A64_StoreWriteback ||
         Id == Intrinsic::A64_VectorStoreWriteback;
}

inline unsigned aarch64WritebackMemoryOperandCount(Intrinsic Id) {
  return Id == Intrinsic::A64_StoreWriteback ||
                 Id == Intrinsic::A64_VectorStoreWriteback
             ? kWritebackStoreOperands
             : kWritebackLoadOperands;
}

struct AArch64WritebackMemoryOperation {
  bool Load;
  unsigned AccessBytes;
  unsigned ResultBytes;
  bool Vector = false;
};

inline std::optional<AArch64WritebackMemoryOperation>
aarch64WritebackMemoryOperation(uint64_t Word) {
  if (Word > std::numeric_limits<uint32_t>::max())
    return std::nullopt;
  switch (Word & kWritebackOpcodeMask) {
#define NEVERD_A64_WRITEBACK_ENCODING(Encoding, Load, Access, Result)          \
  case Encoding:                                                               \
    return AArch64WritebackMemoryOperation{Load, Access, Result};
#define NEVERD_A64_WRITEBACK_VECTOR_ENCODING(Encoding, Load, Access)           \
  case Encoding:                                                               \
    return AArch64WritebackMemoryOperation{                                    \
        Load, Access, Load ? kAddressMemoryVectorBytes : 0, true};
#include "neverd/ir/intrinsics/AArch64/AArch64WritebackMemory.def"
  default:
    return std::nullopt;
  }
}

struct AArch64WritebackMemoryInstruction {
  uint32_t Word;

  auto operation() const { return *aarch64WritebackMemoryOperation(Word); }
  bool isLoad() const { return operation().Load; }
  bool isVector() const { return operation().Vector; }
  Intrinsic id() const {
    if (isVector())
      return isLoad() ? Intrinsic::A64_VectorLoadWriteback
                      : Intrinsic::A64_VectorStoreWriteback;
    return isLoad() ? Intrinsic::A64_LoadWriteback
                    : Intrinsic::A64_StoreWriteback;
  }
  unsigned accessBytes() const { return operation().AccessBytes; }
  unsigned destination() const { return Word & kAddressMemoryRegisterMask; }
  unsigned base() const {
    return (Word >> kAddressMemoryBaseShift) & kAddressMemoryRegisterMask;
  }
  bool stackBase() const { return base() == kAddressMemoryRegisterMask; }
  bool hasRegisterIndex() const { return false; }
  bool preIndex() const { return Word & kWritebackPreIndexBit; }
  AArch64AddressMemoryInstruction adjustment() const {
    return {Word & ~kWritebackModeMask};
  }
  uint64_t displacement() const { return adjustment().addressOffset(0); }

  /// This is a finite storage proof only. Execution always obtains the base
  /// from the machine instruction. A post-index access need not dereference
  /// its outgoing base, but that base must remain within the materialization.
  std::optional<AArch64AddressMemorySpan>
  controlIndependentSpan(uint64_t Base, uint64_t MaximumIndex) const {
    if (!aarch64WritebackMemoryOperation(Word) || MaximumIndex)
      return std::nullopt;
    const auto Updated = adjustment().controlIndependentAddress(Base, 0);
    if (!Updated)
      return std::nullopt;
    auto Access = adjustment();
    if (!preIndex())
      Access.Word &= ~(kAddressMemoryUnscaledOffsetMask
                       << kAddressMemoryUnscaledOffsetShift);
    const auto Span = Access.controlIndependentSpan(Base, 0);
    if (!Span)
      return std::nullopt;
    const auto Begin = std::min(Span->Begin, *Updated);
    const auto End = std::max(Span->Begin + Span->Bytes, *Updated);
    return AArch64AddressMemorySpan{Begin, End - Begin};
  }
  uint32_t scratchWord() const {
    return (Word & ~kAddressMemoryBaseDestinationMask) |
           kAddressMemoryScratchValue |
           (kAddressMemoryScratchBase << kAddressMemoryBaseShift);
  }
};

inline std::optional<AArch64WritebackMemoryInstruction>
aarch64WritebackMemoryInstruction(uint64_t Word) {
  if (!aarch64WritebackMemoryOperation(Word))
    return std::nullopt;
  AArch64WritebackMemoryInstruction Instruction{static_cast<uint32_t>(Word)};
  // Both loads and stores have constrained-unpredictable writeback overlap.
  // Choosing different scratch registers cannot resolve that ambiguity.
  if (!Instruction.isVector() && !Instruction.stackBase() &&
      Instruction.destination() == Instruction.base())
    return std::nullopt;
  return Instruction;
}

inline bool aarch64WritebackMemoryShapeIsValid(
    Intrinsic Id, unsigned NumInputs, unsigned OutputBytes, unsigned BaseBytes,
    unsigned WordBytes, std::optional<uint64_t> Word, unsigned DataBytes = 0,
    std::optional<uint64_t> Data = std::nullopt) {
  if (!isAArch64WritebackMemoryIntrinsic(Id) || !Word ||
      NumInputs != aarch64WritebackMemoryOperandCount(Id) + 1 ||
      BaseBytes != kAddressMemoryPointerBytes ||
      WordBytes != kAddressMemoryWordBytes)
    return false;
  const auto Instruction = aarch64WritebackMemoryInstruction(*Word);
  if (!Instruction || Instruction->id() != Id)
    return false;
  if (Instruction->isLoad())
    return OutputBytes == (Instruction->isVector() ? 0 : kWritebackPairBytes) &&
           !DataBytes;
  if (Instruction->isVector())
    return OutputBytes == kAddressMemoryPointerBytes &&
           DataBytes == kAddressMemoryVectorBytes;
  return OutputBytes == kAddressMemoryPointerBytes &&
         DataBytes == kAddressMemoryPointerBytes &&
         (Instruction->destination() != kAddressMemoryRegisterMask ||
          (Data && !*Data));
}

inline constexpr uint8_t aarch64WritebackMemoryOutputCount(Intrinsic Id) {
  switch (Id) {
#define NEVERD_A64_WRITEBACK_MULTI_OUTPUT(Operation, Count)                    \
  case Intrinsic::Operation:                                                   \
    return Count;
#include "neverd/ir/intrinsics/AArch64/AArch64WritebackMemory.def"
  default:
    return 0;
  }
}

/// The primary value/input shape is shared with LowIR; these typed auxiliary
/// values become explicit SSA definitions in MedIR and HighIR.
template <typename Outputs, typename Size>
inline bool aarch64WritebackMemoryOutputsAreValid(Intrinsic Id,
                                                  const Outputs &Values,
                                                  Size size) {
  if (Values.size() != aarch64WritebackMemoryOutputCount(Id))
    return false;
  return Id != Intrinsic::A64_VectorLoadWriteback ||
         (size(Values[kWritebackValueIndex]) == kAddressMemoryVectorBytes &&
          size(Values[kWritebackBaseIndex]) == kAddressMemoryPointerBytes);
}

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_AARCH64_AARCH64WRITEBACKMEMORY_H
