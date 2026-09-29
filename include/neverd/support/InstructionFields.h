//===- InstructionFields.h - Forms and fields of instructions ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The shapes an encoding table such as BranchEncoding.def gives its rows: an
/// instruction form, a field of an instruction, and the bits a decoder
/// concatenates from several fields.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_INSTRUCTIONFIELDS_H
#define NEVERD_SUPPORT_INSTRUCTIONFIELDS_H

#include "llvm/Support/MathExtras.h"

#include <cstdint>

namespace neverd {

/// An instruction form: the words, or Thumb halfwords, whose bits under
/// \c Mask are \c Match.
struct InstructionForm {
  uint32_t Mask;
  uint32_t Match;
  constexpr bool matches(uint32_t Word) const { return (Word & Mask) == Match; }
};

/// A field of an instruction word or halfword.
struct BitField {
  unsigned Low;
  unsigned Width;
  constexpr uint32_t extract(uint32_t Word) const {
    return (Word >> Low) & llvm::maskTrailingOnes<uint32_t>(Width);
  }
  /// The field's bits in place.
  constexpr uint32_t mask() const {
    return llvm::maskTrailingOnes<uint32_t>(Width) << Low;
  }
};

/// Bits concatenated from instruction fields, the first most significant.
class BitString {
public:
  BitString &append(uint64_t Bits, unsigned Width) {
    Value = Value << Width | Bits;
    Size += Width;
    return *this;
  }
  BitString &append(BitField Field, uint32_t Word) {
    return append(Field.extract(Word), Field.Width);
  }
  BitString &append(const BitString &Other) {
    return append(Other.Value, Other.Size);
  }
  uint64_t zeroExtended() const { return Value; }
  int64_t signExtended() const { return llvm::SignExtend64(Value, Size); }

private:
  uint64_t Value = 0;
  unsigned Size = 0;
};

} // namespace neverd

#endif // NEVERD_SUPPORT_INSTRUCTIONFIELDS_H
