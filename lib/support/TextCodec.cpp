//===- TextCodec.cpp - Decoders of the string encodings -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "TextCodec.h"

#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/Unicode.h"

#include <algorithm>
#include <iterator>
#include <tuple>

namespace neverd::strings {
namespace detail {
namespace {

#include "TextEncodingIndexes.inc"

constexpr EncodingInfo Encodings[] = {
#define NEVERD_STRING_ENCODING(Id, Name, Spelling, UnitBytes, BigEndian)       \
  {Name, Spelling, UnitBytes, BigEndian, Decoder::Unicode, nullptr},
#define NEVERD_LEGACY_ENCODING(Id, Name, Spelling, Kind, Table)                \
  {Name, Spelling, 1, false, Decoder::Kind, Table},
#include "neverd/support/StringEncodings.def"
};

/// Bytes of \p C in UTF-8.
unsigned utf8Length(uint32_t C) {
  return C < 0x80 ? 1 : C < 0x800 ? 2 : C < 0x10000 ? 3 : 4;
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
    const unsigned Used = decodeWide<Unit, BigEndian>(At, End, Code);
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

llvm::ArrayRef<EncodingInfo> encodings() { return Encodings; }

const EncodingInfo &info(Encoding E) {
  return Encodings[static_cast<unsigned>(E)];
}

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

void appendUTF8(std::string &Out, llvm::ArrayRef<uint32_t> Codes) {
  for (const uint32_t C : Codes)
    appendUTF8(Out, C);
}

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

} // namespace detail

using namespace detail;

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

std::string foldCase(llvm::StringRef Text) {
  std::string Out;
  Out.reserve(Text.size());
  const auto *At = Text.bytes_begin();
  const auto *End = Text.bytes_end();
  while (At < End) {
    if (*At < 0x80) {
      Out += static_cast<char>(*At >= 'A' && *At <= 'Z' ? *At + 32 : *At);
      ++At;
      continue;
    }
    uint32_t Code = 0;
    const unsigned Size = decodeUTF8(At, End, Code);
    if (!Size) {
      Out += static_cast<char>(*At++);
      continue;
    }
    appendUTF8(Out, static_cast<uint32_t>(llvm::sys::unicode::foldCharSimple(
                        static_cast<int>(Code))));
    At += Size;
  }
  return Out;
}

std::optional<TextStart> textFrom(llvm::ArrayRef<uint8_t> Data, Encoding E,
                                  uint64_t Offset) {
  std::optional<TextStart> Start;
  uint64_t Text = 0;
  decode(Data, E, [&](const DecodedCharacter &Character) {
    // A byte that decodes to nothing holds no text.
    if (!Character.Codes)
      return;
    if (!Start && Character.Offset >= Offset)
      Start = TextStart{Character.Offset, Text, 0};
    if (Start) {
      Start->Chars += Character.Codes;
      return;
    }
    for (unsigned I = 0; I < Character.Codes; ++I)
      Text += utf8Length(Character.Code[I]);
  });
  return Start;
}

} // namespace neverd::strings
