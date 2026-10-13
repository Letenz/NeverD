//===- StringLiteralSymbols.h - Symbols that spell literals -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Symbols whose names spell a string literal instead of naming an object.
/// MSVC pools each literal under a name made of its character type, its size
/// and its leading bytes (StringLiteralSymbols.def).  Such a name says what
/// the bytes hold, not what a reader calls them: a listing names the string
/// by its text, as it names a string no symbol names.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_STRINGLITERALSYMBOLS_H
#define NEVERD_LOADER_STRINGLITERALSYMBOLS_H

#include "neverd/Common.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>

namespace neverd {

struct BinaryImage;

/// What the name of a string literal's symbol spells.
struct StringLiteralName {
  /// The literal's size in bytes, its terminator included.
  uint64_t Bytes = 0;
  /// The width of its characters where the name says it (wchar_t), or 0
  /// where its bytes decide.
  unsigned UnitBytes = 0;
  /// Its leading bytes in memory order, without the terminator; a name
  /// spells at most 31.
  llvm::SmallVector<uint8_t, 32> Leading;
};

/// What \p Name spells when it names a string literal; none for any other
/// name, a malformed one included.
std::optional<StringLiteralName> parseStringLiteralName(llvm::StringRef Name);

/// Whether \p Name spells a string literal rather than naming an object.
inline bool isStringLiteralName(llvm::StringRef Name) {
  return parseStringLiteralName(Name).has_value();
}

/// A string literal that a symbol names and the image holds.
struct SymbolStringLiteral {
  /// Bytes of its characters before the first zero one: 0 for "".
  uint64_t Bytes = 0;
  /// Bytes of one character: 1, 2 or 4.
  unsigned UnitBytes = 1;
  /// Bytes of the whole literal, every zero character after its text
  /// included: `L"%04d\0"` ends in two.
  uint64_t Size = 0;
};

/// The literal that a symbol named \p Name names at \p Addr, when \p Img
/// holds it there: every byte is in the file, the bytes the name spells
/// begin it, and its text ends at its first zero character, which only zero
/// characters follow.  None for any other name, for a symbol whose bytes do
/// not match (a stale symbol file), and for a literal of binary data rather
/// than characters (`"\x01\x06\0\0\0\x05P..."`).
std::optional<SymbolStringLiteral>
symbolStringLiteralAt(const BinaryImage &Img, va_t Addr, llvm::StringRef Name);

/// The name of the image's own symbol at \p Addr that spells a literal, or
/// an empty one.  Only MSVC names literals so, and only in COFF: a symbol
/// file's names are the debug information's to give.
llvm::StringRef stringLiteralSymbolAt(const BinaryImage &Img, va_t Addr);

} // namespace neverd

#endif // NEVERD_LOADER_STRINGLITERALSYMBOLS_H
