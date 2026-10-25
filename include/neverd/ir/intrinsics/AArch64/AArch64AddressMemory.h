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

struct AArch64AddressMemoryInstruction {
  uint32_t Word;

  unsigned destination() const { return Word & kAddressMemoryRegisterMask; }
  unsigned base() const {
    return (Word >> kAddressMemoryBaseShift) & kAddressMemoryRegisterMask;
  }
  unsigned index() const {
    return (Word >> kAddressMemoryIndexShift) & kAddressMemoryRegisterMask;
  }
  bool stackBase() const { return base() == kAddressMemoryRegisterMask; }
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
  if (Word > std::numeric_limits<uint32_t>::max() ||
      (Word & kAddressLoadOpcodeMask) != kAddressLoadOpcode)
    return std::nullopt;
  return AArch64AddressMemoryInstruction{static_cast<uint32_t>(Word)};
}

inline bool aarch64AddressMemoryShapeIsValid(
    Intrinsic Id, unsigned NumInputs, unsigned OutputBytes, unsigned BaseBytes,
    unsigned IndexBytes, unsigned WordBytes, std::optional<uint64_t> Word) {
  return Id == Intrinsic::A64_Load64RegisterOffset && Word &&
         NumInputs == kAddressLoadOperandCount + 1 &&
         OutputBytes == kAddressMemoryPointerBytes &&
         BaseBytes == kAddressMemoryPointerBytes &&
         IndexBytes == kAddressMemoryPointerBytes &&
         WordBytes == kAddressMemoryWordBytes &&
         aarch64AddressMemoryInstruction(*Word).has_value();
}

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_AARCH64_AARCH64ADDRESSMEMORY_H
