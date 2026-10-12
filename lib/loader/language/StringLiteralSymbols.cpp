//===- StringLiteralSymbols.cpp - Symbols that spell literals -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Reads the names MSVC gives the string literals it pools, and finds the
/// literal a name spells in the image's bytes.
///
//===----------------------------------------------------------------------===//

#include "neverd/loader/StringLiteralSymbols.h"

#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <iterator>

using namespace neverd;

namespace {

/// Introduces an escaped byte; `$` after it introduces a byte in two digits.
constexpr char EscapeIntroducer = '?';
constexpr char HexEscape = '$';
/// Ends a number written in digits, and the bytes a name spells.
constexpr char FieldEnd = '@';
/// The digits of a hexadecimal number, `A` for 0 to `P` for 15.
constexpr char FirstDigit = 'A';
constexpr char LastDigit = 'P';
/// A hash is 32 bits, so eight digits at most; a size is 64.
constexpr size_t MaxHashDigits = 8;
constexpr size_t MaxSizeDigits = 16;

struct LiteralPrefix {
  llvm::StringLiteral Spelling;
  unsigned UnitBytes;
};

constexpr LiteralPrefix LiteralPrefixes[] = {
#define NEVERD_STRING_LITERAL_PREFIX(Prefix, UnitBytes) {Prefix, UnitBytes},
#include "neverd/loader/StringLiteralSymbols.def"
};

constexpr unsigned OpenUnitBytes[] = {
#define NEVERD_STRING_LITERAL_UNIT(UnitBytes) UnitBytes,
#include "neverd/loader/StringLiteralSymbols.def"
};

bool isNameDigit(char C) { return C >= FirstDigit && C <= LastDigit; }

/// Takes a number in at most \p MaxDigits digits and the `@` that ends it.
std::optional<uint64_t> takeNumber(llvm::StringRef &Text, size_t MaxDigits) {
  uint64_t Value = 0;
  size_t Digits = 0;
  for (; Digits < Text.size() && isNameDigit(Text[Digits]); ++Digits) {
    if (Digits == MaxDigits)
      return std::nullopt;
    Value = Value << 4 | static_cast<uint64_t>(Text[Digits] - FirstDigit);
  }
  if (Digits == Text.size() || Text[Digits] != FieldEnd)
    return std::nullopt;
  Text = Text.drop_front(Digits + 1);
  return Value;
}

/// Takes a literal's size: one decimal digit for 1 to 10, else digits and
/// `@`.
std::optional<uint64_t> takeSize(llvm::StringRef &Text) {
  if (Text.empty())
    return std::nullopt;
  if (llvm::isDigit(Text.front())) {
    const uint64_t Size = static_cast<uint64_t>(Text.front() - '0') + 1;
    Text = Text.drop_front();
    return Size;
  }
  if (Text.front() == FieldEnd)
    return std::nullopt;
  return takeNumber(Text, MaxSizeDigits);
}

/// The byte that `?` and \p Spelling spell.
std::optional<uint8_t> escapedByte(char Spelling) {
  switch (Spelling) {
#define NEVERD_STRING_LITERAL_ESCAPE(Spelled, Byte)                            \
  case Spelled:                                                                \
    return static_cast<uint8_t>(Byte);
#include "neverd/loader/StringLiteralSymbols.def"
  default:
    break;
  }
#define NEVERD_STRING_LITERAL_ESCAPE_RANGE(First, Last, Byte)                  \
  if (Spelling >= First && Spelling <= Last)                                   \
    return static_cast<uint8_t>(Byte + (Spelling - First));
#include "neverd/loader/StringLiteralSymbols.def"
  return std::nullopt;
}

/// Takes the next byte a name spells.
std::optional<uint8_t> takeByte(llvm::StringRef &Text) {
  const char First = Text.front();
  Text = Text.drop_front();
  if (First != EscapeIntroducer)
    return static_cast<uint8_t>(First);
  if (Text.empty())
    return std::nullopt;
  const char Escape = Text.front();
  Text = Text.drop_front();
  if (Escape != HexEscape)
    return escapedByte(Escape);
  if (Text.size() < 2 || !isNameDigit(Text[0]) || !isNameDigit(Text[1]))
    return std::nullopt;
  const auto Byte = static_cast<uint8_t>((Text[0] - FirstDigit) << 4 |
                                         (Text[1] - FirstDigit));
  Text = Text.drop_front(2);
  return Byte;
}

/// The bytes the file holds at [\p Addr, \p Addr + \p Size), within one
/// section where the image has them.
std::optional<llvm::ArrayRef<uint8_t>> fileBytes(const BinaryImage &Img,
                                                 va_t Addr, uint64_t Size) {
  const Segment *Seg = Img.getSegmentFor(Addr);
  if (!Seg || !Seg->isReadable() || Addr < Seg->VA)
    return std::nullopt;
  const uint64_t Offset = Addr - Seg->VA;
  uint64_t Limit = Seg->Data.size();
  // A segment may hold zeros past the section's bytes in the file.
  if (const Section *Sec = Img.getSectionFor(Addr); Sec && Sec->VA >= Seg->VA)
    Limit =
        std::min(Limit, Sec->VA - Seg->VA + std::min(Sec->Size, Sec->FileSz));
  if (Offset >= Limit || Size > Limit - Offset)
    return std::nullopt;
  return llvm::ArrayRef<uint8_t>(Seg->Data.data() + Offset,
                                 static_cast<size_t>(Size));
}

/// Where the first zero of the \p UnitBytes wide characters of \p Bytes
/// starts, when only zeros follow it: `L"%04d\0"` keeps a second terminator,
/// and binary data its zeros among other bytes.
std::optional<uint64_t> firstOfTrailingZeros(llvm::ArrayRef<uint8_t> Bytes,
                                             unsigned UnitBytes) {
  if (Bytes.size() < UnitBytes || Bytes.size() % UnitBytes)
    return std::nullopt;
  const auto IsZero = [](uint8_t Byte) { return Byte == 0; };
  for (size_t At = 0; At < Bytes.size(); At += UnitBytes)
    if (llvm::all_of(Bytes.slice(At, UnitBytes), IsZero))
      return llvm::all_of(Bytes.drop_front(At), IsZero)
                 ? std::optional<uint64_t>(At)
                 : std::nullopt;
  return std::nullopt;
}

} // namespace

std::optional<StringLiteralName>
neverd::parseStringLiteralName(llvm::StringRef Name) {
  const auto *Prefix =
      llvm::find_if(LiteralPrefixes, [&](const LiteralPrefix &Candidate) {
        return Name.starts_with(Candidate.Spelling);
      });
  if (Prefix == std::end(LiteralPrefixes))
    return std::nullopt;
  llvm::StringRef Text = Name.drop_front(Prefix->Spelling.size());
  // A literal holds its terminator at least, and its characters whole.
  const unsigned Least = std::max(Prefix->UnitBytes, 1u);
  const auto Size = takeSize(Text);
  if (!Size || *Size < Least || *Size % Least ||
      !takeNumber(Text, MaxHashDigits))
    return std::nullopt;
  StringLiteralName Literal;
  Literal.Bytes = *Size;
  Literal.UnitBytes = Prefix->UnitBytes;
  while (!Text.empty() && Text.front() != FieldEnd) {
    const auto Byte = takeByte(Text);
    if (!Byte)
      return std::nullopt;
    Literal.Leading.push_back(*Byte);
  }
  if (Text.empty())
    return std::nullopt;
  Text = Text.drop_front();
  // Whole-program optimization adds the hash of the literal's module.
  if (!Text.empty() && (!takeNumber(Text, MaxHashDigits) || !Text.empty()))
    return std::nullopt;
  // The name spells no more than the bytes before the terminator, and wide
  // characters whole, high byte first; Windows keeps them low byte first.
  if (Literal.Leading.size() > Literal.Bytes - Least ||
      Literal.Leading.size() % Least)
    return std::nullopt;
  for (auto At = Literal.Leading.begin(); At != Literal.Leading.end();
       At += Least)
    std::reverse(At, At + Least);
  return Literal;
}

std::optional<SymbolStringLiteral>
neverd::symbolStringLiteralAt(const BinaryImage &Img, va_t Addr,
                              llvm::StringRef Name) {
  const auto Spelled = parseStringLiteralName(Name);
  if (!Spelled)
    return std::nullopt;
  const auto Bytes = fileBytes(Img, Addr, Spelled->Bytes);
  if (!Bytes || !llvm::equal(Spelled->Leading,
                             Bytes->take_front(Spelled->Leading.size())))
    return std::nullopt;
  const auto Literal =
      [&](unsigned UnitBytes) -> std::optional<SymbolStringLiteral> {
    const auto Text = firstOfTrailingZeros(*Bytes, UnitBytes);
    if (!Text)
      return std::nullopt;
    return SymbolStringLiteral{*Text, UnitBytes, Bytes->size()};
  };
  if (Spelled->UnitBytes)
    return Literal(Spelled->UnitBytes);
  for (const unsigned UnitBytes : OpenUnitBytes)
    if (auto Found = Literal(UnitBytes))
      return Found;
  return std::nullopt;
}
