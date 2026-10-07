//===- StringScan.cpp - text strings in binary data ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/support/StringScan.h"

#include "llvm/Support/ConvertUTF.h"

#include <algorithm>
#include <vector>

namespace neverd::strings {

namespace {

struct EncodingInfo {
  llvm::StringRef Name, Spelling;
  unsigned UnitBytes;
  bool BigEndian;
};

constexpr EncodingInfo Encodings[] = {
#define NEVERD_STRING_ENCODING(Id, Name, Spelling, UnitBytes, BigEndian)       \
  {Name, Spelling, UnitBytes, BigEndian},
#include "neverd/support/StringEncodings.def"
};

const EncodingInfo &info(Encoding E) {
  return Encodings[static_cast<unsigned>(E)];
}

bool isTextASCII(uint32_t C) {
  return (C >= 0x20 && C < 0x7f) || C == '\t' || C == '\n' || C == '\r';
}

/// A printable character beyond ASCII: no control, surrogate, private-use
/// or noncharacter code point.
bool isPrintableBeyondASCII(uint32_t C) {
  if (C < 0xa0 || C > 0x10ffff)
    return false;
  if ((C >= 0xd800 && C <= 0xdfff) || (C >= 0xe000 && C <= 0xf8ff) ||
      (C >= 0xfdd0 && C <= 0xfdef) || (C & 0xfffe) == 0xfffe || C >= 0xf0000)
    return false;
  return true;
}

/// Which code points a wide string may hold: printable ASCII and the
/// printable characters of the text blocks, a bit each below U+10000.
class WideTextSet {
public:
  WideTextSet() {
    for (uint32_t C = 0; C < 0x80; ++C)
      if (isTextASCII(C))
        set(C);
#define NEVERD_TEXT_BLOCK(First, Last)                                         \
  for (uint32_t C = First; C <= Last && C <= 0xffff; ++C)                      \
    if (isPrintableBeyondASCII(C))                                             \
      set(C);
#include "neverd/support/StringEncodings.def"
  }
  bool contains(uint32_t C) const {
    if (C <= 0xffff)
      return Bits[C >> 6] >> (C & 63) & 1;
#define NEVERD_TEXT_BLOCK(First, Last)                                         \
  if (First > 0xffff && C >= First && C <= Last)                               \
    return isPrintableBeyondASCII(C);
#include "neverd/support/StringEncodings.def"
    return false;
  }

private:
  void set(uint32_t C) { Bits[C >> 6] |= uint64_t(1) << (C & 63); }
  uint64_t Bits[0x10000 / 64] = {};
};

const WideTextSet &wideText() {
  static const WideTextSet Set;
  return Set;
}

/// The length of the well-formed UTF-8 sequence of two to four bytes at
/// \p At, storing its code point in \p Code, or 0.
unsigned decodeUTF8(const uint8_t *At, const uint8_t *End, uint32_t &Code) {
  const uint8_t Lead = At[0];
  const auto continues = [&](unsigned Index) {
    return At + Index < End && (At[Index] & 0xc0) == 0x80;
  };
  if (Lead < 0xc2 || Lead > 0xf4 || !continues(1))
    return 0;
  if (Lead < 0xe0) {
    Code = (Lead & 0x1fu) << 6 | (At[1] & 0x3fu);
    return 2;
  }
  if (!continues(2))
    return 0;
  if (Lead < 0xf0) {
    Code = (Lead & 0x0fu) << 12 | (At[1] & 0x3fu) << 6 | (At[2] & 0x3fu);
    return Code >= 0x800 && (Code < 0xd800 || Code > 0xdfff) ? 3 : 0;
  }
  if (!continues(3))
    return 0;
  Code = (Lead & 0x07u) << 18 | (At[1] & 0x3fu) << 12 | (At[2] & 0x3fu) << 6 |
         (At[3] & 0x3fu);
  return Code >= 0x10000 && Code <= 0x10ffff ? 4 : 0;
}

void appendUTF8(std::string &Out, uint32_t C) {
  char Buffer[UNI_MAX_UTF8_BYTES_PER_CODE_POINT];
  char *End = Buffer;
  llvm::ConvertCodePointToUTF8(C, End);
  Out.append(Buffer, End);
}

/// ASCII and UTF-8 strings ending in NUL.
void scanCStrings(llvm::ArrayRef<uint8_t> Data, const ScanOptions &Options,
                  std::vector<FoundString> &Out) {
  const bool ASCII = Options.Encodings & encodingBit(Encoding::ASCII);
  const bool UTF8 = Options.Encodings & encodingBit(Encoding::UTF8);
  if (!ASCII && !UTF8)
    return;
  const uint8_t *Begin = Data.data();
  const uint8_t *End = Begin + Data.size();
  for (const uint8_t *Cursor = Begin; Cursor < End; ++Cursor) {
    const uint8_t *Start = Cursor;
    unsigned Chars = 0;
    bool Multibyte = false;
    while (Cursor < End && *Cursor) {
      if (isTextASCII(*Cursor)) {
        ++Chars;
        ++Cursor;
        continue;
      }
      uint32_t Code = 0;
      const unsigned Size = UTF8 ? decodeUTF8(Cursor, End, Code) : 0;
      if (!Size || !isPrintableBeyondASCII(Code))
        break;
      ++Chars;
      Cursor += Size;
      Multibyte = true;
    }
    if (Cursor == End || *Cursor || Chars < Options.MinChars ||
        (Multibyte ? !UTF8 : !ASCII))
      continue;
    FoundString Found;
    Found.Offset = static_cast<uint64_t>(Start - Begin);
    Found.Bytes = static_cast<uint64_t>(Cursor - Start);
    Found.Chars = Chars;
    Found.Kind = Multibyte ? Encoding::UTF8 : Encoding::ASCII;
    Found.Text.assign(reinterpret_cast<const char *>(Start), Found.Bytes);
    Out.push_back(std::move(Found));
  }
}

template <unsigned Unit, bool BigEndian> uint32_t readUnit(const uint8_t *At) {
  uint32_t Value = 0;
  for (unsigned I = 0; I < Unit; ++I)
    Value = Value << 8 | At[BigEndian ? I : Unit - 1 - I];
  return Value;
}

/// The code point of the wide character at \p At and its length in bytes,
/// or 0 for a unit that does not start a text character.
template <unsigned Unit, bool BigEndian>
unsigned decodeWide(const uint8_t *At, const uint8_t *End, uint32_t &Code) {
  Code = readUnit<Unit, BigEndian>(At);
  unsigned Used = Unit;
  if (Unit == 2 && Code >= 0xd800 && Code <= 0xdbff) {
    if (End - At < 4)
      return 0;
    const uint32_t Low = readUnit<Unit, BigEndian>(At + 2);
    if (Low < 0xdc00 || Low > 0xdfff)
      return 0;
    Code = 0x10000 + ((Code - 0xd800) << 10) + (Low - 0xdc00);
    Used = 4;
  }
  return wideText().contains(Code) ? Used : 0;
}

/// UTF-16 or UTF-32 strings ending in a zero unit, at multiples of the unit
/// size.  At least half of a wide string's characters are printable ASCII:
/// random 16-bit data decodes into the large ideograph blocks too often for
/// other text to be told from it by the code points alone.
template <unsigned Unit, bool BigEndian>
void scanWideStrings(llvm::ArrayRef<uint8_t> Data, Encoding Kind,
                     unsigned MinChars, std::vector<FoundString> &Out) {
  const uint8_t *Begin = Data.data();
  const uint8_t *End = Begin + Data.size() / Unit * Unit;
  for (const uint8_t *Cursor = Begin; Cursor < End; Cursor += Unit) {
    const uint8_t *Start = Cursor;
    unsigned Chars = 0, ASCIIChars = 0;
    uint32_t Code = 0;
    // Counted first; the text is decoded only for a string that is kept.
    while (Cursor < End) {
      const unsigned Used = decodeWide<Unit, BigEndian>(Cursor, End, Code);
      if (!Used)
        break;
      ++Chars;
      ASCIIChars += Code < 0x80;
      Cursor += Used;
    }
    if (Cursor >= End || readUnit<Unit, BigEndian>(Cursor) ||
        Chars < MinChars || ASCIIChars * 2 < Chars)
      continue;
    FoundString Found;
    Found.Offset = static_cast<uint64_t>(Start - Begin);
    Found.Bytes = static_cast<uint64_t>(Cursor - Start);
    Found.Chars = Chars;
    Found.Kind = Kind;
    for (const uint8_t *At = Start; At < Cursor;) {
      At += decodeWide<Unit, BigEndian>(At, End, Code);
      appendUTF8(Found.Text, Code);
    }
    Out.push_back(std::move(Found));
  }
}

} // namespace

llvm::StringRef encodingName(Encoding E) {
  const unsigned Index = static_cast<unsigned>(E);
  return Index < std::size(Encodings) ? Encodings[Index].Name
                                      : llvm::StringRef();
}

unsigned encodingUnitBytes(Encoding E) { return info(E).UnitBytes; }

llvm::StringRef encodingSpelling(Encoding E) { return info(E).Spelling; }

std::optional<Encoding> encodingNamed(llvm::StringRef Name) {
  for (unsigned I = 0; I < std::size(Encodings); ++I)
    if (Encodings[I].Name.equals_insensitive(Name))
      return static_cast<Encoding>(I);
  return std::nullopt;
}

void scan(llvm::ArrayRef<uint8_t> Data, const ScanOptions &Options,
          llvm::function_ref<void(FoundString &&)> Found) {
  const unsigned MinChars = std::max(1u, Options.MinChars);
  std::vector<FoundString> All;
  ScanOptions Effective = Options;
  Effective.MinChars = MinChars;
  scanCStrings(Data, Effective, All);
  const auto searched = [&](Encoding E) {
    return (Options.Encodings & encodingBit(E)) != 0;
  };
  if (searched(Encoding::UTF16LE))
    scanWideStrings<2, false>(Data, Encoding::UTF16LE, MinChars, All);
  if (searched(Encoding::UTF16BE))
    scanWideStrings<2, true>(Data, Encoding::UTF16BE, MinChars, All);
  if (searched(Encoding::UTF32LE))
    scanWideStrings<4, false>(Data, Encoding::UTF32LE, MinChars, All);
  if (searched(Encoding::UTF32BE))
    scanWideStrings<4, true>(Data, Encoding::UTF32BE, MinChars, All);
  // The first string to start keeps the bytes; at one offset, the longer.
  std::sort(
      All.begin(), All.end(), [](const FoundString &A, const FoundString &B) {
        return A.Offset != B.Offset ? A.Offset < B.Offset : A.Bytes > B.Bytes;
      });
  uint64_t Free = 0;
  for (auto &String : All) {
    if (String.Offset < Free)
      continue;
    Free = String.Offset + String.Bytes + info(String.Kind).UnitBytes;
    Found(std::move(String));
  }
}

} // namespace neverd::strings
