//===- TextCodec.h - Decoders of the string encodings -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// How the bytes of each string encoding become code points: UTF-8, UTF-16
/// and UTF-32 in either byte order, and the legacy code pages, decoded as the
/// WHATWG Encoding Standard does.  A new encoding is a row of
/// StringEncodings.def and, for a new kind of code page, a Decoder case here.
/// Private to the support library.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_SUPPORT_TEXTCODEC_H
#define NEVERD_LIB_SUPPORT_TEXTCODEC_H

#include "neverd/support/StringScan.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <string>
#include <vector>

namespace neverd::strings::detail {

/// How an encoding's bytes become code points.
enum class Decoder : uint8_t {
  Unicode,
  GB18030,
  Big5,
  ShiftJIS,
  EUCKR,
  SingleByte
};

struct EncodingInfo {
  llvm::StringRef Name, Spelling;
  unsigned UnitBytes;
  bool BigEndian;
  Decoder Kind;
  /// A single-byte code page's characters for bytes 0x80-0xFF.
  const uint16_t *Table;
};

/// Every encoding, indexed by Encoding.
llvm::ArrayRef<EncodingInfo> encodings();
const EncodingInfo &info(Encoding E);

/// The length of the well-formed UTF-8 sequence of two to four bytes at
/// \p At, storing its code point in \p Code, or 0.
unsigned decodeUTF8(const uint8_t *At, const uint8_t *End, uint32_t &Code);

/// Decodes one character of a legacy code page as the WHATWG Encoding
/// Standard does, storing its one or two code points.  Returns its length
/// in bytes, or 0 for a byte sequence the code page does not map.
unsigned decodeLegacy(const EncodingInfo &Info, const uint8_t *At,
                      const uint8_t *End, uint32_t (&Code)[2], unsigned &Codes);

template <unsigned Unit, bool BigEndian> uint32_t readUnit(const uint8_t *At) {
  uint32_t Value = 0;
  for (unsigned I = 0; I < Unit; ++I)
    Value = Value << 8 | At[BigEndian ? I : Unit - 1 - I];
  return Value;
}

/// The code point of the wide character at \p At and its length in bytes,
/// or 0 for a unit that does not start a character.
template <unsigned Unit, bool BigEndian>
unsigned decodeWide(const uint8_t *At, const uint8_t *End, uint32_t &Code) {
  Code = readUnit<Unit, BigEndian>(At);
  if (Unit == 2 && Code >= 0xd800 && Code <= 0xdbff) {
    if (End - At < 4)
      return 0;
    const uint32_t Low = readUnit<Unit, BigEndian>(At + 2);
    if (Low < 0xdc00 || Low > 0xdfff)
      return 0;
    Code = 0x10000 + ((Code - 0xd800) << 10) + (Low - 0xdc00);
    return 4;
  }
  if ((Code >= 0xd800 && Code <= 0xdfff) || Code > 0x10ffff)
    return 0;
  return Unit;
}

void appendUTF8(std::string &Out, uint32_t C);
void appendUTF8(std::string &Out, llvm::ArrayRef<uint32_t> Codes);

/// The code points of the well-formed UTF-8 text [\p At, \p End).
std::vector<uint32_t> utf8Codes(const uint8_t *At, const uint8_t *End);

} // namespace neverd::strings::detail

#endif // NEVERD_LIB_SUPPORT_TEXTCODEC_H
