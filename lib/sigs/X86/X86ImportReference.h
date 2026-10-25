//===- X86ImportReference.h - COFF import reference fields ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recognizes the canonical x86/x64 memory-indirect call/jump field shared
/// by pattern generation and reference checking. Prefixed forms are not
/// interpreted as unprefixed instructions.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SIGS_X86_X86IMPORTREFERENCE_H
#define NEVERD_SIGS_X86_X86IMPORTREFERENCE_H

#include "neverd/support/ISAEncoding.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>

namespace neverd::sigs {

inline constexpr llvm::StringLiteral COFFImportPointerPrefix = "__imp_";

/// FF /2 and FF /4 with mod=00, r/m=101: an absolute 32-bit slot in i386,
/// or a RIP-relative slot in x64. Offset names the four-byte address field.
inline bool isX86ImportReferenceField(llvm::ArrayRef<uint8_t> Code,
                                      size_t Offset, bool Is64Bit) {
  if (Offset < x86::kJmpIndirectDispOffset || Offset > Code.size() ||
      Code.size() - Offset < sizeof(uint32_t))
    return false;
  const size_t Opcode = Offset - x86::kJmpIndirectDispOffset;
  constexpr uint8_t CallMemoryModRM = 0x15;
  if (Code[Opcode] != x86::kJmpIndirectOp ||
      (Code[Opcode + 1] != CallMemoryModRM &&
       Code[Opcode + 1] != x86::kJmpIndirectModRM))
    return false;
  if (Opcode == 0)
    return true;
  // With no decoded boundary before FF, even a prefix-looking final byte
  // of the preceding instruction is conservatively left unsupported. In
  // particular, address/segment overrides must not acquire RIP semantics.
  const uint8_t Before = Code[Opcode - 1];
  if (Is64Bit && (Before & 0xf0) == 0x40)
    return false;
  switch (Before) {
  case 0xf0:
  case 0xf2:
  case 0xf3:
  case 0x2e:
  case 0x36:
  case 0x3e:
  case 0x26:
  case 0x64:
  case 0x65:
  case 0x66:
  case 0x67:
    return false;
  default:
    return true;
  }
}

} // namespace neverd::sigs

#endif // NEVERD_SIGS_X86_X86IMPORTREFERENCE_H
