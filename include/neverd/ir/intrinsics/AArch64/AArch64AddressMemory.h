//===- AArch64AddressMemory.h - Original memory contracts ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Retain the memory instruction, rather than evaluating its address with an
/// unchecked integer sum. No image subtype proves runtime SCTLR2 controls.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_AARCH64_AARCH64ADDRESSMEMORY_H
#define NEVERD_IR_INTRINSICS_AARCH64_AARCH64ADDRESSMEMORY_H

#include "neverd/ir/intrinsics/Intrinsics.h"

#include <bit>
#include <climits>
#include <cstdint>
#include <limits>
#include <optional>

namespace neverd {

#define NEVERD_A64_ADDRESS_MEMORY_VALUE(Type, Name, Value)                     \
  inline constexpr Type Name = Value;
#define NEVERD_A64_ADDRESS_MEMORY_TEXT(Name, Text)                             \
  inline constexpr char Name[] = Text;
#include "neverd/ir/intrinsics/AArch64/AArch64AddressMemory.def"

inline bool isAArch64AddressMemoryIntrinsic(Intrinsic Id) {
  switch (Id) {
#define NEVERD_A64_ADDRESS_MEMORY_OPERATION(Id, Mnemonic) case Intrinsic::Id:
#include "neverd/ir/intrinsics/AArch64/AArch64AddressMemory.def"
    return true;
  default:
    return false;
  }
}

inline unsigned aarch64AddressMemoryOperandCount(Intrinsic Id) {
  return Id == Intrinsic::A64_StoreRegisterOffset ? kAddressStoreOperandCount
                                                  : kAddressLoadOperandCount;
}

struct AArch64AddressMemoryOperation {
  Intrinsic Id;
  unsigned AccessBytes;
  unsigned ResultBytes;
  bool Signed;
};

inline std::optional<AArch64AddressMemoryOperation>
aarch64AddressMemoryOperation(uint64_t Word) {
  if (Word > std::numeric_limits<uint32_t>::max())
    return std::nullopt;
  switch (Word & kAddressMemoryOpcodeMask) {
#define NEVERD_A64_ADDRESS_MEMORY_ENCODING(Id, Word, Access, Result, Signed)   \
  case Word:                                                                   \
    return AArch64AddressMemoryOperation{Intrinsic::Id, Access, Result, Signed};
#include "neverd/ir/intrinsics/AArch64/AArch64AddressMemory.def"
  default:
    return std::nullopt;
  }
}

struct AArch64AddressMemoryInstruction {
  uint32_t Word;

  AArch64AddressMemoryOperation operation() const {
    return *aarch64AddressMemoryOperation(Word);
  }
  bool isLoad() const { return operation().ResultBytes != 0; }
  unsigned accessBytes() const { return operation().AccessBytes; }
  bool usesFullWidthIndex() const { return Word & kAddressMemoryWideIndexBit; }
  unsigned indexBytes() const {
    return usesFullWidthIndex() ? kAddressMemoryPointerBytes
                                : kAddressMemoryNarrowIndexBytes;
  }
  uint64_t extendIndex(uint64_t Index) const {
    if (usesFullWidthIndex())
      return Index;
    const uint64_t Narrow = Index & kAddressMemoryNarrowMask;
    return (Word & kAddressMemorySignedIndexBit) &&
                   (Narrow & kAddressMemoryNarrowSignBit)
               ? Narrow | ~kAddressMemoryNarrowMask
               : Narrow;
  }
  unsigned destination() const { return Word & kAddressMemoryRegisterMask; }
  unsigned base() const {
    return (Word >> kAddressMemoryBaseShift) & kAddressMemoryRegisterMask;
  }
  unsigned index() const {
    return (Word >> kAddressMemoryIndexShift) & kAddressMemoryRegisterMask;
  }
  bool stackBase() const { return base() == kAddressMemoryRegisterMask; }
  unsigned indexShift() const {
    return Word & kAddressMemoryScaleMask ? std::countr_zero(accessBytes()) : 0;
  }
  /// Return an address only when CPTA on/off and every CPACHECK choice for
  /// the entire access have identical bits. This grants no mapping, tagging,
  /// alignment or no-fault evidence; consumers still need their memory model.
  std::optional<uint64_t> controlIndependentAddress(uint64_t Base,
                                                    uint64_t Index) const {
    if (!aarch64AddressMemoryOperation(Word) || stackBase() ||
        (index() == kAddressMemoryRegisterMask && Index))
      return std::nullopt;
    const uint64_t Address = Base + (extendIndex(Index) << indexShift());
    const bool PreviousDetection = bool(Base & kAddressMemorySignBit) !=
                                   bool(Base & kAddressMemoryPoisonBit);
    const bool Detected =
        ((Address ^ Base) & kAddressMemoryTagMask) || PreviousDetection;
    const uint64_t Checked =
        (Address & ~(kAddressMemoryPreservedMask | kAddressMemoryPoisonBit)) |
        (Base & kAddressMemoryPreservedMask) |
        (Base & kAddressMemorySignBit ? 0 : kAddressMemoryPoisonBit);
    if (Detected && Address != Checked)
      return std::nullopt;
    constexpr uint64_t Region =
        kAddressMemoryPreservedMask | kAddressMemoryPoisonBit;
    const uint64_t LastByte = accessBytes() - 1;
    if (Address > std::numeric_limits<uint64_t>::max() - LastByte ||
        ((Address ^ (Address + LastByte)) & Region))
      return std::nullopt;
    return Address;
  }
  /// A sufficient proof for every nonnegative raw index in [0, MaximumIndex].
  /// Monotonic addition must remain in one preserved-address region through
  /// the final accessed byte. This grants no memory or selector-domain proof.
  bool controlIndependentAddressRange(uint64_t Base,
                                      uint64_t MaximumIndex) const {
    if (!aarch64AddressMemoryOperation(Word) ||
        (index() == kAddressMemoryRegisterMask && MaximumIndex) ||
        (!usesFullWidthIndex() && MaximumIndex > kAddressMemoryNarrowMask) ||
        extendIndex(MaximumIndex) != MaximumIndex ||
        MaximumIndex > (std::numeric_limits<uint64_t>::max() >> indexShift()))
      return false;
    const uint64_t Offset = MaximumIndex << indexShift();
    const uint64_t LastByte = accessBytes() - 1;
    if (Base > std::numeric_limits<uint64_t>::max() - Offset ||
        Base + Offset > std::numeric_limits<uint64_t>::max() - LastByte)
      return false;
    const uint64_t End = Base + Offset + LastByte;
    const uint64_t Region =
        kAddressMemoryPreservedMask | kAddressMemoryPoisonBit;
    return !((Base ^ End) & Region) &&
           controlIndependentAddress(Base, 0).has_value() &&
           controlIndependentAddress(Base, MaximumIndex).has_value();
  }
  uint32_t scratchWord() const {
    // Replacing XZR's index with a zero-valued GPR preserves its value. The
    // original shift stays encoded and is applied exactly once by the load.
    return (Word & ~kAddressMemoryRegistersMask) | kAddressMemoryScratchValue |
           (kAddressMemoryScratchBase << kAddressMemoryBaseShift) |
           (kAddressMemoryScratchIndex << kAddressMemoryIndexShift);
  }
};

inline std::optional<AArch64AddressMemoryInstruction>
aarch64AddressMemoryInstruction(uint64_t Word) {
  if (!aarch64AddressMemoryOperation(Word))
    return std::nullopt;
  return AArch64AddressMemoryInstruction{static_cast<uint32_t>(Word)};
}

inline bool aarch64AddressMemoryShapeIsValid(
    Intrinsic Id, unsigned NumInputs, unsigned OutputBytes, unsigned BaseBytes,
    unsigned IndexBytes, unsigned WordBytes, std::optional<uint64_t> Word,
    std::optional<uint64_t> Index, unsigned DataBytes = 0,
    std::optional<uint64_t> Data = std::nullopt) {
  const auto Instruction =
      Word ? aarch64AddressMemoryInstruction(*Word) : std::nullopt;
  if (!Instruction || Id != Instruction->operation().Id ||
      NumInputs != aarch64AddressMemoryOperandCount(Id) + 1 ||
      OutputBytes != (Instruction->isLoad() ? Instruction->accessBytes() : 0) ||
      BaseBytes != kAddressMemoryPointerBytes ||
      IndexBytes != Instruction->indexBytes() ||
      WordBytes != kAddressMemoryWordBytes ||
      (Instruction->index() == kAddressMemoryRegisterMask &&
       (!Index || *Index)))
    return false;
  if (Instruction->isLoad())
    return DataBytes == 0;
  return DataBytes == Instruction->accessBytes() &&
         (Instruction->destination() != kAddressMemoryRegisterMask ||
          (Data && !*Data));
}

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_AARCH64_AARCH64ADDRESSMEMORY_H
