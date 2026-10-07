//===- StringScan.h - text strings in binary data ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Finds terminated text strings in raw bytes: ASCII and UTF-8 C strings and
/// UTF-16 and UTF-32 strings in either byte order.  Every string ends in a
/// zero code unit and holds only printable characters; wide strings also stay
/// within the Unicode blocks of written languages (StringEncodings.def).
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
#include "neverd/support/StringEncodings.def"
};

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
  /// ASCII and UTF-8 C strings, and UTF-16LE strings.
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

/// Reports the strings in \p Data in offset order, none overlapping another.
/// A wide string starts at a multiple of its code unit size; where strings
/// of different encodings overlap, the one starting first wins.
void scan(llvm::ArrayRef<uint8_t> Data, const ScanOptions &Options,
          llvm::function_ref<void(FoundString &&)> Found);

} // namespace neverd::strings

#endif // NEVERD_SUPPORT_STRINGSCAN_H
