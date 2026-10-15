//===- X86CetEncoding.h - Canonical shadow-stack encodings ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LIB_LIFT_X86_X86CETENCODING_H
#define NEVERD_LIB_LIFT_X86_X86CETENCODING_H

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>

namespace neverd::x86cet {
enum class Operation { Read, Increment };

/// The bounded canonical encoding domain shared by execution profiles and
/// recovery proofs. Matching bytes do not establish whether CET is enabled.
/// Callers must already establish 64-bit instruction mode; REX bytes do not
/// have the same instruction-prefix meaning in 32-bit mode.
/// With shadow stacks disabled, Intel SDM defines Read as preserving all
/// destination bits (also for r32); Increment instead raises #UD.
inline bool canonicalEncoding(llvm::ArrayRef<uint8_t> Bytes, Operation Op) {
  if ((Bytes.size() != 4 && Bytes.size() != 5) || Bytes[0] != 0xf3)
    return false;
  unsigned I = 1;
  if (Bytes.size() == 5) {
    // REX.W/B select width/bank. Other and repeated prefixes are outside
    // this domain, even when a decoder normalizes them to the same mnemonic.
    if (Bytes[I] != 0x40 && Bytes[I] != 0x41 && Bytes[I] != 0x48 &&
        Bytes[I] != 0x49)
      return false;
    ++I;
  }
  const bool Read = Op == Operation::Read;
  return Bytes[I] == 0x0f && Bytes[I + 1] == (Read ? 0x1e : 0xae) &&
         (Bytes[I + 2] & 0xf8) == (Read ? 0xc8 : 0xe8);
}
} // namespace neverd::x86cet
#endif
