//===- StringScan.cpp - text strings in binary data ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/support/StringScan.h"

#include "llvm/Support/ConvertUTF.h"

#include <algorithm>
#include <iterator>
#include <optional>
#include <vector>

namespace neverd::strings {

namespace {

#include "TextEncodingTables.inc"

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
  const uint16_t *Table;
};

constexpr EncodingInfo Encodings[] = {
#define NEVERD_STRING_ENCODING(Id, Name, Spelling, UnitBytes, BigEndian)       \
  {Name, Spelling, UnitBytes, BigEndian, Decoder::Unicode, nullptr},
#define NEVERD_LEGACY_ENCODING(Id, Name, Spelling, Kind, Table)                \
  {Name, Spelling, 1, false, Decoder::Kind, Table},
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

/// A letter or typographic mark a single-byte code page string may hold.
bool isLegacyLetter(uint32_t C) {
#define NEVERD_LEGACY_LETTERS(First, Last)                                     \
  if (C >= First && C <= Last)                                                 \
    return true;
#include "neverd/support/StringEncodings.def"
  return false;
}

bool isIdeograph(uint32_t C) {
  return (C >= 0x3400 && C <= 0x9fff) || (C >= 0xac00 && C <= 0xd7a3);
}

/// An ideograph of the first level of a national standard (GB2312, Big5,
/// JIS X 0208) or a KS X 1001 Hangul syllable.
bool isCommonIdeograph(uint32_t C) {
  if (C >= 0x3400 && C <= 0x9fff)
    return CommonHan[(C - 0x3400) >> 6] >> ((C - 0x3400) & 63) & 1;
  if (C >= 0xac00 && C <= 0xd7a3)
    return CommonHangul[(C - 0xac00) >> 6] >> ((C - 0xac00) & 63) & 1;
  return false;
}

/// Strings with at most this many ideographs need all of them common.
constexpr unsigned ShortIdeographicText = 10;
/// From this many characters on, text beyond ASCII may not repeat one
/// character in two in five positions.
constexpr size_t TablePatternLength = 6;

/// The writing system of a letter beyond ASCII (StringEncodings.def).
enum class Family : uint8_t {
  None,
  Latin,
  Greek,
  Cyrillic,
  Hebrew,
  Arabic,
  Indic,
  Thai,
  Japanese,
  Chinese,
  Han,
  Korean
};

Family familyOf(uint32_t C) {
#define NEVERD_SCRIPT_FAMILY(First, Last, Kind)                                \
  if (C >= First && C <= Last)                                                 \
    return Family::Kind;
#include "neverd/support/StringEncodings.def"
  return Family::None;
}

/// Where a candidate string's characters came from: each source needs its
/// own amount of evidence that they are text.
enum class Source : uint8_t { UTF8, Wide, DoubleByte, SingleByte };

bool isASCIILetter(uint32_t C) {
  return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z');
}

bool isEastAsian(Family Kind) {
  return Kind == Family::Han || Kind == Family::Japanese ||
         Kind == Family::Chinese || Kind == Family::Korean;
}

/// The case of a letter: 1 uppercase, -1 lowercase, 0 none or unknown.
int letterCase(uint32_t C) {
  if ((C >= 'A' && C <= 'Z') || (C >= 0xc0 && C <= 0xde && C != 0xd7) ||
      (C >= 0x391 && C <= 0x3a9) || (C >= 0x400 && C <= 0x42f))
    return 1;
  if ((C >= 'a' && C <= 'z') || (C >= 0xdf && C <= 0xff && C != 0xf7) ||
      (C >= 0x3b1 && C <= 0x3c9) || (C >= 0x430 && C <= 0x45f))
    return -1;
  // Latin Extended-A pairs each capital with the small letter after it.
  if (C >= 0x100 && C <= 0x17f)
    return C & 1 ? -1 : 1;
  return 0;
}

/// Whether \p Codes read as text rather than numbers that decode:
///  - its letters beyond ASCII come from one writing system (ideographs go
///    with Chinese, Japanese or Korean); a word does not mix ASCII letters
///    with them, except accented Latin; and a word holding cased letters
///    beyond ASCII is lowercase, Capitalized or uppercase;
///  - no character beyond ASCII makes up half of them, as fill bytes do,
///    and in a longer string beyond ASCII no character fills two in five
///    positions, as a table of pairs does;
///  - its ideographs are common ones, all of them in a short string and
///    nine in ten in a long one (random values rarely are);
///  - East Asian text has two such characters, and other text three
///    letters, four from a single-byte code page or outside Latin, its
///    letters beyond ASCII inside words and, in a Latin script, at most three
///    in five letters accented.
bool readsAsText(llvm::ArrayRef<uint32_t> Codes, Source From) {
  Family Script = Family::None;
  bool Han = false;
  unsigned ASCIILetters = 0, ASCIIAlnum = 0, Letters = 0, Inside = 0;
  unsigned EastAsian = 0, Ideographs = 0, Common = 0;
  std::vector<uint32_t> Beyond;
  // The current word: whether it holds ASCII letters and letters beyond,
  // and the cases of its letters so far.
  bool WordASCII = false, WordBeyond = false, CaseValid = true;
  unsigned WordLetters = 0;
  int FirstCase = 0, RestCase = 0;
  for (size_t I = 0; I <= Codes.size(); ++I) {
    const uint32_t C = I < Codes.size() ? Codes[I] : 0;
    const Family Kind = C >= 0x80 ? familyOf(C) : Family::None;
    const bool ASCIILetter = isASCIILetter(C);
    if (!ASCIILetter && Kind == Family::None) {
      // A word ends.
      if (WordASCII && WordBeyond && Script != Family::Latin)
        return false;
      if (WordBeyond && !CaseValid)
        return false;
      WordASCII = WordBeyond = false;
      CaseValid = true;
      WordLetters = 0;
      FirstCase = RestCase = 0;
    } else if (const int Case = letterCase(C)) {
      // Lowercase, Capitalized or uppercase: after the first letter, every
      // letter has one case, lowercase unless the first is uppercase.
      if (WordLetters++ == 0)
        FirstCase = Case;
      else if (RestCase && Case != RestCase)
        CaseValid = false;
      else if (!RestCase && FirstCase == -1 && Case == 1)
        CaseValid = false;
      else
        RestCase = Case;
    }
    if (I == Codes.size())
      break;
    if (C < 0x80) {
      ASCIILetters += ASCIILetter;
      ASCIIAlnum += ASCIILetter || (C >= '0' && C <= '9');
      WordASCII |= ASCIILetter;
      continue;
    }
    Beyond.push_back(C);
    if (isIdeograph(C)) {
      ++Ideographs;
      Common += isCommonIdeograph(C);
    }
    if (Kind == Family::None)
      continue;
    WordBeyond = true;
    ++Letters;
    EastAsian += isEastAsian(Kind);
    const auto isLetter = [&](size_t At) {
      return At < Codes.size() &&
             (isASCIILetter(Codes[At]) || familyOf(Codes[At]) != Family::None);
    };
    Inside += isLetter(I + 1) || (I > 0 && isLetter(I - 1));
    if (Kind == Family::Han) {
      Han = true;
      continue;
    }
    if (Script != Family::None && Script != Kind)
      return false;
    Script = Kind;
  }
  if (Han && Script != Family::None && !isEastAsian(Script))
    return false;
  const auto longestRun = [](std::vector<uint32_t> Values) {
    std::sort(Values.begin(), Values.end());
    size_t Longest = 0;
    for (size_t I = 0, Run = 0; I < Values.size(); ++I) {
      Run = I && Values[I] == Values[I - 1] ? Run + 1 : 1;
      Longest = std::max(Longest, Run);
    }
    return Longest;
  };
  if (Beyond.size() >= 2) {
    const size_t Longest = longestRun(Beyond);
    if (Longest * 2 > Beyond.size() && (Beyond.size() >= 3 || Longest == 2))
      return false;
    if (Codes.size() >= TablePatternLength &&
        longestRun(Codes) * 5 >= Codes.size() * 2)
      return false;
  }
  if (Ideographs <= ShortIdeographicText ? Common < Ideographs
                                         : Common * 10 < Ideographs * 9)
    return false;
  if (EastAsian || From == Source::DoubleByte)
    return EastAsian >= 2;
  if (From == Source::Wide && !Letters)
    return ASCIIAlnum >= 2;
  const unsigned AllLetters = ASCIILetters + Letters;
  const unsigned Needed =
      (Script == Family::Latin || !Letters) && From != Source::SingleByte ? 3
                                                                          : 4;
  return AllLetters >= Needed && Inside * 5 >= Letters * 4 &&
         (Script != Family::Latin || Letters * 5 <= AllLetters * 3) &&
         (From != Source::SingleByte || Letters);
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

/// Decodes one character of a legacy code page as the WHATWG Encoding
/// Standard does, storing its one or two code points.  Returns its length
/// in bytes, or 0 for a byte sequence the code page does not map.
unsigned decodeLegacy(const EncodingInfo &Info, const uint8_t *At,
                      const uint8_t *End, uint32_t (&Code)[2],
                      unsigned &Codes) {
  const uint8_t Lead = At[0];
  Codes = 1;
  if (Lead < 0x80) {
    Code[0] = Lead;
    return 1;
  }
  const auto trail = [&](unsigned Index) -> int {
    return At + Index < End ? At[Index] : -1;
  };
  const auto mapped = [&](uint32_t Value, unsigned Bytes) -> unsigned {
    Code[0] = Value;
    return Value ? Bytes : 0;
  };
  switch (Info.Kind) {
  case Decoder::Unicode:
    return 0;
  case Decoder::SingleByte:
    return mapped(Info.Table[Lead - 0x80], 1);
  case Decoder::GB18030: {
    if (Lead == 0x80)
      return mapped(0x20ac, 1);
    const int Second = trail(1);
    if (Lead == 0xff || Second < 0)
      return 0;
    if (Second >= 0x30 && Second <= 0x39) {
      const int Third = trail(2), Fourth = trail(3);
      if (Third < 0x81 || Third > 0xfe || Fourth < 0x30 || Fourth > 0x39)
        return 0;
      const uint32_t Pointer =
          (((Lead - 0x81u) * 10 + (Second - 0x30u)) * 126 + (Third - 0x81u)) *
              10 +
          (Fourth - 0x30u);
      if ((Pointer > 39419 && Pointer < 189000) || Pointer > 1237575)
        return 0;
      if (Pointer >= 189000)
        return mapped(0x10000 + Pointer - 189000, 4);
      if (Pointer == 7457)
        return mapped(0xe7c7, 4);
      const auto *Range = std::upper_bound(
          std::begin(GB18030Ranges), std::end(GB18030Ranges), Pointer,
          [](uint32_t Value, const uint32_t (&Entry)[2]) {
            return Value < Entry[0];
          });
      --Range;
      return mapped((*Range)[1] + (Pointer - (*Range)[0]), 4);
    }
    if (Second < 0x40 || Second == 0x7f || Second == 0xff)
      return 0;
    const uint32_t Pointer =
        (Lead - 0x81u) * 190 + (Second - (Second < 0x7f ? 0x40u : 0x41u));
    return mapped(Pointer < std::size(GB18030Index) ? GB18030Index[Pointer] : 0,
                  2);
  }
  case Decoder::Big5: {
    const int Second = trail(1);
    if (Lead == 0x80 || Lead == 0xff || Second < 0x40 ||
        (Second > 0x7e && Second < 0xa1) || Second > 0xfe)
      return 0;
    const uint32_t Pointer =
        (Lead - 0x81u) * 157 + (Second - (Second < 0x7f ? 0x40u : 0x62u));
    // Four pointers decode to a letter and a combining mark.
    for (const auto &[Special, First, Mark] :
         {std::tuple{1133u, 0xcau, 0x304u}, std::tuple{1135u, 0xcau, 0x30cu},
          std::tuple{1164u, 0xeau, 0x304u}, std::tuple{1166u, 0xeau, 0x30cu}})
      if (Pointer == Special) {
        Code[0] = First;
        Code[1] = Mark;
        Codes = 2;
        return 2;
      }
    return mapped(Pointer < std::size(Big5Index) ? Big5Index[Pointer] : 0, 2);
  }
  case Decoder::ShiftJIS: {
    if (Lead == 0x80)
      return mapped(0x80, 1);
    if (Lead >= 0xa1 && Lead <= 0xdf)
      return mapped(0xff61 + Lead - 0xa1, 1);
    const int Second = trail(1);
    if (!((Lead >= 0x81 && Lead <= 0x9f) || (Lead >= 0xe0 && Lead <= 0xfc)) ||
        Second < 0x40 || Second == 0x7f || Second > 0xfc)
      return 0;
    const uint32_t Pointer = (Lead - (Lead < 0xa0 ? 0x81u : 0xc1u)) * 188 +
                             Second - (Second < 0x7f ? 0x40u : 0x41u);
    // User-defined characters live in the private-use area.
    if (Pointer >= 8836 && Pointer <= 10715)
      return mapped(0xe000 + Pointer - 8836, 2);
    return mapped(Pointer < std::size(JIS0208Index) ? JIS0208Index[Pointer] : 0,
                  2);
  }
  case Decoder::EUCKR: {
    const int Second = trail(1);
    if (Lead == 0x80 || Lead == 0xff || Second < 0x41 || Second > 0xfe)
      return 0;
    const uint32_t Pointer = (Lead - 0x81u) * 190 + (Second - 0x41u);
    return mapped(Pointer < std::size(EUCKRIndex) ? EUCKRIndex[Pointer] : 0, 2);
  }
  }
  return 0;
}

void appendUTF8(std::string &Out, uint32_t C) {
  char Buffer[UNI_MAX_UTF8_BYTES_PER_CODE_POINT];
  char *End = Buffer;
  llvm::ConvertCodePointToUTF8(C, End);
  Out.append(Buffer, End);
}

/// A C string in \p Legacy from \p Start: its terminator, or nothing when
/// it holds a character that is not text or does not end.  Collects the
/// code points.
const uint8_t *legacyString(const EncodingInfo &Legacy, const uint8_t *Start,
                            const uint8_t *End, std::vector<uint32_t> &Codes) {
  const bool SingleByte = Legacy.Kind == Decoder::SingleByte;
  for (const uint8_t *Cursor = Start; Cursor < End;) {
    if (!*Cursor)
      return Cursor;
    // 0xFFFF marks data, not text, though 0xFF is a letter in some pages.
    if (SingleByte && *Cursor == 0xff && Cursor + 1 < End && Cursor[1] == 0xff)
      return nullptr;
    uint32_t Code[2];
    unsigned Count = 0;
    const unsigned Size = decodeLegacy(Legacy, Cursor, End, Code, Count);
    if (!Size)
      return nullptr;
    for (unsigned I = 0; I < Count; ++I) {
      const uint32_t C = Code[I];
      // Halfwidth katakana come from single bytes 0xA1-0xDF, a quarter of
      // all bytes, and modern text does not use them.
      const bool Text = C < 0x80 ? isTextASCII(C)
                                 : isPrintableBeyondASCII(C) &&
                                       (!SingleByte || isLegacyLetter(C)) &&
                                       !(C >= 0xff61 && C <= 0xff9f);
      if (!Text)
        return nullptr;
      Codes.push_back(C);
    }
    Cursor += Size;
  }
  return nullptr;
}

void appendUTF8(std::string &Out, llvm::ArrayRef<uint32_t> Codes) {
  for (const uint32_t C : Codes)
    appendUTF8(Out, C);
}

/// The code points of the well-formed UTF-8 text [\p At, \p End).
std::vector<uint32_t> utf8Codes(const uint8_t *At, const uint8_t *End) {
  std::vector<uint32_t> Codes;
  while (At < End) {
    uint32_t Code = *At;
    const unsigned Size = Code < 0x80 ? 1 : decodeUTF8(At, End, Code);
    Codes.push_back(Code);
    At += Size ? Size : 1;
  }
  return Codes;
}

/// ASCII and UTF-8 strings ending in NUL, and where a run after a NUL is not
/// UTF-8, a string in \p Legacy.
void scanCStrings(llvm::ArrayRef<uint8_t> Data, const ScanOptions &Options,
                  const EncodingInfo *Legacy, Encoding LegacyKind,
                  std::vector<FoundString> &Out) {
  const bool ASCII = Options.Encodings & encodingBit(Encoding::ASCII);
  const bool UTF8 = Options.Encodings & encodingBit(Encoding::UTF8);
  if (!ASCII && !UTF8 && !Legacy)
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
    const bool Terminated = Cursor < End && !*Cursor;
    if (Terminated) {
      if (Chars >= Options.MinChars && (Multibyte ? UTF8 : ASCII) &&
          (!Multibyte || readsAsText(utf8Codes(Start, Cursor), Source::UTF8))) {
        FoundString Found;
        Found.Offset = static_cast<uint64_t>(Start - Begin);
        Found.Bytes = static_cast<uint64_t>(Cursor - Start);
        Found.Chars = Chars;
        Found.Kind = Multibyte ? Encoding::UTF8 : Encoding::ASCII;
        Found.Text.assign(reinterpret_cast<const char *>(Start), Found.Bytes);
        Out.push_back(std::move(Found));
      }
      continue;
    }
    // A run UTF-8 does not read may be text in the legacy code page, where
    // a pool of strings would hold one: after a NUL.  Random bytes decode
    // in a double-byte code page most of the time, so starting anywhere
    // else would read most data again from every byte.
    if (!Legacy || Cursor == End || (Start != Begin && Start[-1]))
      continue;
    std::vector<uint32_t> Codes;
    const uint8_t *Terminator = legacyString(*Legacy, Start, End, Codes);
    if (!Terminator || Codes.size() < Options.MinChars ||
        std::all_of(Codes.begin(), Codes.end(),
                    [](uint32_t C) { return C < 0x80; }) ||
        !readsAsText(Codes, Legacy->Kind == Decoder::SingleByte
                                ? Source::SingleByte
                                : Source::DoubleByte))
      continue;
    FoundString Found;
    Found.Offset = static_cast<uint64_t>(Start - Begin);
    Found.Bytes = static_cast<uint64_t>(Terminator - Start);
    Found.Chars = static_cast<unsigned>(Codes.size());
    Found.Kind = LegacyKind;
    appendUTF8(Found.Text, Codes);
    Out.push_back(std::move(Found));
    Cursor = Terminator;
  }
}

template <unsigned Unit, bool BigEndian> uint32_t readUnit(const uint8_t *At) {
  uint32_t Value = 0;
  for (unsigned I = 0; I < Unit; ++I)
    Value = Value << 8 | At[BigEndian ? I : Unit - 1 - I];
  return Value;
}

/// The code point of the wide character at \p At and its length in bytes,
/// or 0 for a unit that does not start a character.  With \p TextOnly, a
/// character outside the text blocks counts as no character either.
template <unsigned Unit, bool BigEndian>
unsigned decodeWide(const uint8_t *At, const uint8_t *End, uint32_t &Code,
                    bool TextOnly) {
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
  } else if ((Code >= 0xd800 && Code <= 0xdfff) || Code > 0x10ffff) {
    return 0;
  }
  return !TextOnly || wideText().contains(Code) ? Used : 0;
}

/// UTF-16 or UTF-32 strings ending in a zero unit, at multiples of the unit
/// size, that read as text (readsAsText).  Random 16-bit data decodes into
/// the large ideograph blocks a third of the time, so a string mostly beyond
/// ASCII also needs a zero unit before it, as in a pool of strings, and must
/// not be ASCII read one byte off (every unit's low byte zero).
template <unsigned Unit, bool BigEndian>
void scanWideStrings(llvm::ArrayRef<uint8_t> Data, Encoding Kind,
                     unsigned MinChars, std::vector<FoundString> &Out) {
  const uint8_t *Begin = Data.data();
  const uint8_t *End = Begin + Data.size() / Unit * Unit;
  for (const uint8_t *Cursor = Begin; Cursor < End; Cursor += Unit) {
    const uint8_t *Start = Cursor;
    unsigned Chars = 0, ASCII = 0;
    bool LowBytesZero = true;
    uint32_t Code = 0;
    // Counted first; the text is decoded only for a candidate that ends.
    while (Cursor < End) {
      const unsigned Used =
          decodeWide<Unit, BigEndian>(Cursor, End, Code, true);
      if (!Used)
        break;
      ++Chars;
      ASCII += Code < 0x80;
      LowBytesZero &= Code < 0x80 || (Code & 0xff) == 0;
      Cursor += Used;
    }
    if (Cursor >= End || readUnit<Unit, BigEndian>(Cursor) || Chars < MinChars)
      continue;
    // Text mostly beyond ASCII also sits where a pool of strings puts it,
    // after a zero unit, and is not ASCII read one byte off.
    if (ASCII * 2 < Chars &&
        (LowBytesZero ||
         (Start != Begin && readUnit<Unit, BigEndian>(Start - Unit))))
      continue;
    std::vector<uint32_t> Codes;
    Codes.reserve(Chars);
    for (const uint8_t *At = Start; At < Cursor;) {
      At += decodeWide<Unit, BigEndian>(At, End, Code, true);
      Codes.push_back(Code);
    }
    if (!readsAsText(Codes, Source::Wide))
      continue;
    FoundString Found;
    Found.Offset = static_cast<uint64_t>(Start - Begin);
    Found.Bytes = static_cast<uint64_t>(Cursor - Start);
    Found.Chars = Chars;
    Found.Kind = Kind;
    appendUTF8(Found.Text, Codes);
    Out.push_back(std::move(Found));
  }
}

template <unsigned Unit, bool BigEndian>
void decodeWideText(llvm::ArrayRef<uint8_t> Data,
                    llvm::function_ref<void(const DecodedCharacter &)> Found) {
  const uint8_t *Begin = Data.data();
  const uint8_t *End = Begin + Data.size() / Unit * Unit;
  for (const uint8_t *At = Begin; At < End;) {
    DecodedCharacter Character;
    Character.Offset = static_cast<uint64_t>(At - Begin);
    uint32_t Code = 0;
    const unsigned Used = decodeWide<Unit, BigEndian>(At, End, Code, false);
    Character.Bytes = Used ? Used : Unit;
    if (Used) {
      Character.Code[0] = Code;
      Character.Codes = 1;
    }
    Found(Character);
    At += Character.Bytes;
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

bool isLegacyEncoding(Encoding E) { return info(E).Kind != Decoder::Unicode; }

std::optional<Encoding> encodingNamed(llvm::StringRef Name) {
  for (unsigned I = 0; I < std::size(Encodings); ++I)
    if (Encodings[I].Name.equals_insensitive(Name))
      return static_cast<Encoding>(I);
#define NEVERD_ENCODING_ALIAS(Alias, Id)                                       \
  if (Name.equals_insensitive(Alias))                                          \
    return Encoding::Id;
#include "neverd/support/StringEncodings.def"
  return std::nullopt;
}

bool isShownCharacter(uint32_t C) {
  return C >= 0x20 && C != 0x7f && !(C >= 0x80 && C < 0xa0) &&
         !(C >= 0x200b && C <= 0x200f) && !(C >= 0x202a && C <= 0x202e) &&
         !(C >= 0x2060 && C <= 0x206f) && C != 0xfeff &&
         !(C >= 0xd800 && C <= 0xf8ff) && C < 0xf0000;
}

void decode(llvm::ArrayRef<uint8_t> Data, Encoding E,
            llvm::function_ref<void(const DecodedCharacter &)> Found) {
  const EncodingInfo &Info = info(E);
  switch (E) {
  case Encoding::UTF16LE:
    return decodeWideText<2, false>(Data, Found);
  case Encoding::UTF16BE:
    return decodeWideText<2, true>(Data, Found);
  case Encoding::UTF32LE:
    return decodeWideText<4, false>(Data, Found);
  case Encoding::UTF32BE:
    return decodeWideText<4, true>(Data, Found);
  default:
    break;
  }
  const uint8_t *Begin = Data.data();
  const uint8_t *End = Begin + Data.size();
  for (const uint8_t *At = Begin; At < End;) {
    DecodedCharacter Character;
    Character.Offset = static_cast<uint64_t>(At - Begin);
    if (*At < 0x80) {
      Character.Code[0] = *At;
      Character.Codes = 1;
    } else if (Info.Kind != Decoder::Unicode) {
      unsigned Codes = 0;
      if (const unsigned Size =
              decodeLegacy(Info, At, End, Character.Code, Codes)) {
        Character.Bytes = Size;
        Character.Codes = Codes;
      }
    } else if (E == Encoding::UTF8) {
      uint32_t Code = 0;
      if (const unsigned Size = decodeUTF8(At, End, Code)) {
        Character.Bytes = Size;
        Character.Code[0] = Code;
        Character.Codes = 1;
      }
    }
    Found(Character);
    At += Character.Bytes;
  }
}

void scan(llvm::ArrayRef<uint8_t> Data, const ScanOptions &Options,
          llvm::function_ref<void(FoundString &&)> Found) {
  const unsigned MinChars = std::max(1u, Options.MinChars);
  std::vector<FoundString> All;
  ScanOptions Effective = Options;
  Effective.MinChars = MinChars;
  // The legacy code page searched, the first one the options name.
  const EncodingInfo *Legacy = nullptr;
  Encoding LegacyKind = Encoding::ASCII;
  for (unsigned I = 0; I < std::size(Encodings) && !Legacy; ++I)
    if (Encodings[I].Kind != Decoder::Unicode &&
        (Options.Encodings & encodingBit(static_cast<Encoding>(I)))) {
      Legacy = &Encodings[I];
      LegacyKind = static_cast<Encoding>(I);
    }
  scanCStrings(Data, Effective, Legacy, LegacyKind, All);
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
  // The first string to start keeps the bytes; at one offset, the longer,
  // then the Unicode encoding.
  std::sort(All.begin(), All.end(),
            [](const FoundString &A, const FoundString &B) {
              if (A.Offset != B.Offset)
                return A.Offset < B.Offset;
              if (A.Bytes != B.Bytes)
                return A.Bytes > B.Bytes;
              return A.Kind < B.Kind;
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
