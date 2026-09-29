//===- BranchEncoding.h - Encodings of direct branches ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The rows of BranchEncoding.def as typed constants: the instruction forms,
/// the fields and the other constants of the direct branches and linker
/// thunks NeverD decodes, which ISAEncoding.h's constants of the same
/// instructions are defined from.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_BRANCHENCODING_H
#define NEVERD_SUPPORT_BRANCHENCODING_H

#include "neverd/support/InstructionFields.h"

#include "llvm/Support/MathExtras.h"

#include <cstdint>
#include <optional>

namespace neverd {
namespace branch {

#define NEVERD_BRANCH_FORM(Name, Mask, Match)                                  \
  inline constexpr InstructionForm Name{Mask, Match};
#define NEVERD_BRANCH_FIELD(Name, Low, Width)                                  \
  inline constexpr BitField Name{Low, Width};
#define NEVERD_BRANCH_VALUE(Name, Value) inline constexpr uint32_t Name = Value;
#include "neverd/support/BranchEncoding.def"

/// How far an A64 `b` or `bl` \p Word branches, in bytes from itself.
inline int64_t a64BranchDisplacement(uint32_t Word) {
  return llvm::SignExtend64(A64BranchOffset.extract(Word),
                            A64BranchOffset.Width) *
         A64InstructionBytes;
}

/// Where an A64 `b` or `bl` \p Word at \p Address branches, or std::nullopt
/// when that lies outside the 64-bit address space.
inline std::optional<uint64_t> a64BranchTarget(uint32_t Word,
                                               uint64_t Address) {
  const int64_t Displacement = a64BranchDisplacement(Word);
  const uint64_t Magnitude = Displacement < 0
                                 ? 0 - static_cast<uint64_t>(Displacement)
                                 : static_cast<uint64_t>(Displacement);
  if (Displacement < 0 ? Address < Magnitude : Address > ~Magnitude)
    return std::nullopt;
  return Address + static_cast<uint64_t>(Displacement);
}

/// Whether an A64 `b` or `bl` can branch \p Displacement bytes: a whole
/// number of instructions its offset field holds.
inline bool isA64BranchDisplacement(int64_t Displacement) {
  constexpr unsigned InstructionBits =
      llvm::ConstantLog2<A64InstructionBytes>();
  return Displacement % A64InstructionBytes == 0 &&
         llvm::isIntN(A64BranchOffset.Width + InstructionBits, Displacement);
}

/// \p Word with its A64 `b` or `bl` offset field set to branch
/// \p Displacement bytes.  The bits below a whole instruction, and those the
/// field cannot hold, are cut off; isA64BranchDisplacement says whether any
/// were.
inline uint32_t withA64BranchDisplacement(uint32_t Word, int64_t Displacement) {
  constexpr unsigned InstructionBits =
      llvm::ConstantLog2<A64InstructionBytes>();
  const uint32_t Offset = static_cast<uint32_t>(
      static_cast<uint64_t>(Displacement >> InstructionBits));
  return (Word & ~A64BranchOffset.mask()) |
         ((Offset << A64BranchOffset.Low) & A64BranchOffset.mask());
}

} // namespace branch
} // namespace neverd

#endif // NEVERD_SUPPORT_BRANCHENCODING_H
