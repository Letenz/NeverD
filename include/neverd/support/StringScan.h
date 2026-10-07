//===- StringScan.h - text strings in binary data ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Finds terminated text strings in raw bytes: ASCII and UTF-8 C strings,
/// UTF-16 and UTF-32 strings in either byte order, and C strings in one
/// legacy code page.  Every string ends in a zero code unit and holds only
/// printable characters; StringEncodings.def states which characters each
/// kind may hold.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_STRINGSCAN_H
#define NEVERD_SUPPORT_STRINGSCAN_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>

namespace neverd::strings {

enum class Encoding : uint8_t {
#define NEVERD_STRING_ENCODING(Id, Name, Spelling, UnitBytes, BigEndian) Id,
#define NEVERD_LEGACY_ENCODING(Id, Name, Spelling, Decoder, Table) Id,
#include "neverd/support/StringEncodings.def"
};

/// Whether \p E is an 8-bit code page rather than a Unicode encoding.
bool isLegacyEncoding(Encoding E);

/// The option and JSON name of \p E ("utf-16le"); empty past the last
/// encoding.
llvm::StringRef encodingName(Encoding E);
/// Bytes in one code unit of \p E.
unsigned encodingUnitBytes(Encoding E);
/// How a listing names \p E (`text "UTF-16LE", ...`); empty for ASCII.
llvm::StringRef encodingSpelling(Encoding E);
std::optional<Encoding> encodingNamed(llvm::StringRef Name);

/// A set of encodings, one bit per Encoding.
using EncodingSet = uint32_t;
constexpr EncodingSet encodingBit(Encoding E) {
  return EncodingSet(1) << static_cast<unsigned>(E);
}

/// The largest minimum length a scan accepts.
constexpr unsigned MaxMinChars = 1024;

struct ScanOptions {
  /// ASCII and UTF-8 C strings, and UTF-16LE strings.  At most one legacy
  /// encoding: C strings that are not UTF-8 are read in it.
  EncodingSet Encodings = encodingBit(Encoding::ASCII) |
                          encodingBit(Encoding::UTF8) |
                          encodingBit(Encoding::UTF16LE);
  /// Characters a string needs, its terminator not counted.
  unsigned MinChars = 4;
};

struct FoundString {
  /// Offset into the scanned bytes and length in bytes, without the
  /// terminator.
  uint64_t Offset = 0, Bytes = 0;
  unsigned Chars = 0;
  Encoding Kind = Encoding::ASCII;
  /// The characters as UTF-8.
  std::string Text;
};

/// One character \p Data decodes to.  Bytes that do not decode in the
/// encoding are characters of one byte with no code point.
struct DecodedCharacter {
  uint64_t Offset = 0;
  unsigned Bytes = 1;
  /// The code points; a Big5 pair decodes to two, and an undecodable byte to
  /// none.
  uint32_t Code[2] = {0, 0};
  unsigned Codes = 0;
};

/// Decodes \p Data in \p E from its first byte, reporting each character in
/// offset order; wide encodings decode whole units only.
void decode(llvm::ArrayRef<uint8_t> Data, Encoding E,
            llvm::function_ref<void(const DecodedCharacter &)> Found);

/// Whether a listing shows \p Code as itself: not a control, a formatting
/// or direction mark, a byte order mark or a private-use character.
bool isShownCharacter(uint32_t Code);

/// Reports the strings in \p Data in offset order, none overlapping another.
/// A wide string starts at a multiple of its code unit size; where strings
/// of different encodings overlap, the one starting first wins.
void scan(llvm::ArrayRef<uint8_t> Data, const ScanOptions &Options,
          llvm::function_ref<void(FoundString &&)> Found);

} // namespace neverd::strings

#endif // NEVERD_SUPPORT_STRINGSCAN_H
