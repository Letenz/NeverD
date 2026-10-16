//===- IntegerLiterals.h - Integer literals of source code ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The integer literals of a line of C, C++, Rust or Go source, and their
/// spelling in another base (neverd/LiteralRadixes.def). The worker re-spells
/// the literals of pseudocode the user picked a base for, and the GUI finds
/// the literal under the cursor and shows what each base would read; both
/// count a line's literals with the same scan, so the n-th literal of a line
/// is the same one in the code and in its display.
///
/// A spelling keeps the literal's type. A C literal takes the suffix that
/// gives it the type the written one had (C17 6.4.4.1, the same table in C++)
/// in both the LP64 and the LLP64 data model; a base no suffix keeps it in has
/// no spelling. Rust and Go literals keep their type in every base. Character
/// constants are C's: a Rust char is no integer, and a Go rune constant would
/// change the type a short variable declaration infers.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_INTEGERLITERALS_H
#define NEVERD_SUPPORT_INTEGERLITERALS_H

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neverd {

/// A base an integer literal reads in (neverd/LiteralRadixes.def).
enum class LiteralRadix : uint8_t {
#define NEVERD_LITERAL_RADIX(Id, Spelling) Id,
#include "neverd/LiteralRadixes.def"
};

/// The spelling of \p Radix in requests and saved edits, such as "hex".
inline std::string_view literalRadixName(LiteralRadix Radix) {
  switch (Radix) {
#define NEVERD_LITERAL_RADIX(Id, Spelling)                                     \
  case LiteralRadix::Id:                                                       \
    return Spelling;
#include "neverd/LiteralRadixes.def"
  }
  return {};
}

/// The base named \p Name, if any.
inline std::optional<LiteralRadix> literalRadixNamed(std::string_view Name) {
#define NEVERD_LITERAL_RADIX(Id, Spelling)                                     \
  if (Name == Spelling)                                                        \
    return LiteralRadix::Id;
#include "neverd/LiteralRadixes.def"
  return std::nullopt;
}

/// The literal grammar of a source language: C covers C, C++ and the C that
/// LLVM produces.
enum class LiteralSyntax : uint8_t { C, Rust, Go };

/// One integer literal of a line.
struct IntegerLiteral {
  /// The literal's bytes in the line, prefix and suffix included.
  std::size_t Begin = 0, End = 0;
  /// The literal as written.
  std::string Text;
  uint64_t Value = 0;
  LiteralRadix Radix = LiteralRadix::Decimal;
  /// The type suffix as written, such as `u`, `ULL` or `_u64`.
  std::string Suffix;
  /// The language fixes the value; a one-character constant above 0x7F or a
  /// longer one than an int holds does not.
  bool Exact = true;
};

namespace detail {

inline bool literalIdentifierStart(char C) {
  return std::isalpha(static_cast<unsigned char>(C)) || C == '_' ||
         static_cast<unsigned char>(C) >= 0x80;
}

inline bool literalIdentifierPart(char C) {
  return literalIdentifierStart(C) ||
         std::isdigit(static_cast<unsigned char>(C));
}

/// The offset after the quoted literal that opens at \p Begin with \p Quote,
/// or the line's end when it never closes.
inline std::size_t literalSkipQuoted(std::string_view Line, std::size_t Begin,
                                     char Quote) {
  for (std::size_t I = Begin + 1; I < Line.size(); ++I) {
    if (Line[I] == '\\')
      ++I;
    else if (Line[I] == Quote)
      return I + 1;
  }
  return Line.size();
}

/// The value of a C character constant's body (between the quotes), as an
/// int: GCC, Clang and MSVC put the first of several characters in the most
/// significant byte. Exact is false where the language leaves the value to
/// the implementation, and the body is none where it does not parse.
inline std::optional<std::pair<uint64_t, bool>>
literalCharacterValue(std::string_view Body) {
  std::vector<unsigned> Units;
  for (std::size_t I = 0; I < Body.size();) {
    unsigned char C = static_cast<unsigned char>(Body[I]);
    if (C != '\\') {
      Units.push_back(C);
      ++I;
      continue;
    }
    if (++I >= Body.size())
      return std::nullopt;
    const char E = Body[I++];
    switch (E) {
    case '\'':
    case '"':
    case '?':
    case '\\':
      Units.push_back(static_cast<unsigned char>(E));
      break;
    case 'a':
      Units.push_back(7);
      break;
    case 'b':
      Units.push_back(8);
      break;
    case 'f':
      Units.push_back(12);
      break;
    case 'n':
      Units.push_back(10);
      break;
    case 'r':
      Units.push_back(13);
      break;
    case 't':
      Units.push_back(9);
      break;
    case 'v':
      Units.push_back(11);
      break;
    case 'x': {
      unsigned Value = 0;
      std::size_t Digits = 0;
      for (; I < Body.size() &&
             std::isxdigit(static_cast<unsigned char>(Body[I]));
           ++I, ++Digits) {
        if (Value > 0xFF)
          return std::nullopt;
        const char D = Body[I];
        Value = Value * 16 + (std::isdigit(static_cast<unsigned char>(D))
                                  ? unsigned(D - '0')
                                  : unsigned(std::tolower(D) - 'a' + 10));
      }
      if (!Digits || Value > 0xFF)
        return std::nullopt;
      Units.push_back(Value);
      break;
    }
    default:
      if (E < '0' || E > '7')
        return std::nullopt; // Universal names and unknown escapes.
      unsigned Value = unsigned(E - '0');
      for (std::size_t Digits = 1;
           Digits < 3 && I < Body.size() && Body[I] >= '0' && Body[I] <= '7';
           ++Digits)
        Value = Value * 8 + unsigned(Body[I++] - '0');
      if (Value > 0xFF)
        return std::nullopt;
      Units.push_back(Value);
      break;
    }
  }
  if (Units.empty())
    return std::nullopt;
  // Compilers agree on up to four ASCII characters; a byte above 0x7F is a
  // negative char on some of them.
  bool Exact = Units.size() <= 4;
  uint64_t Value = 0;
  for (unsigned Unit : Units) {
    Exact = Exact && Unit <= 0x7F;
    Value = (Value << 8) | Unit;
  }
  return std::pair{Exact ? Value : 0, Exact};
}

/// The integer literal \p Token of \p Syntax, or none when it is no integer
/// literal: a floating literal, an unknown suffix, or a value beyond 64 bits.
inline std::optional<IntegerLiteral> literalToken(std::string_view Token,
                                                  LiteralSyntax Syntax) {
  if (Token.find('.') != std::string_view::npos)
    return std::nullopt;
  IntegerLiteral Literal;
  Literal.Text = std::string(Token);
  unsigned Base = 10;
  std::size_t At = 0;
  const auto Prefix = [&](char Lower) {
    return Token.size() > 2 && Token[0] == '0' &&
           std::tolower(static_cast<unsigned char>(Token[1])) == Lower;
  };
  if (Prefix('x')) {
    Base = 16;
    Literal.Radix = LiteralRadix::Hex;
    At = 2;
  } else if (Prefix('b')) {
    Base = 2;
    Literal.Radix = LiteralRadix::Binary;
    At = 2;
  } else if (Syntax != LiteralSyntax::C && Prefix('o')) {
    Base = 8;
    Literal.Radix = LiteralRadix::Octal;
    At = 2;
  } else if (Syntax != LiteralSyntax::Rust && Token.size() > 1 &&
             Token[0] == '0' &&
             std::isdigit(static_cast<unsigned char>(Token[1]))) {
    // Rust reads a leading zero as part of a decimal number.
    Base = 8;
    Literal.Radix = LiteralRadix::Octal;
    At = 1;
  }
  const bool Separators = Syntax != LiteralSyntax::C;
  std::size_t Digits = 0;
  uint64_t Value = 0;
  for (; At < Token.size(); ++At) {
    const char C = Token[At];
    if (Separators && C == '_')
      continue;
    unsigned Digit;
    if (std::isdigit(static_cast<unsigned char>(C)))
      Digit = unsigned(C - '0');
    else if (Base == 16 && std::isxdigit(static_cast<unsigned char>(C)))
      Digit = unsigned(std::tolower(static_cast<unsigned char>(C)) - 'a' + 10);
    else
      break;
    if (Digit >= Base)
      return std::nullopt;
    if (Value > (UINT64_MAX - Digit) / Base)
      return std::nullopt;
    Value = Value * Base + Digit;
    ++Digits;
  }
  if (!Digits && !(Base == 8 && At == 1))
    return std::nullopt;
  // An exponent makes a decimal floating literal; a hexadecimal one has a p,
  // which no suffix allows.
  if (Base == 10 && At < Token.size() &&
      std::tolower(static_cast<unsigned char>(Token[At])) == 'e')
    return std::nullopt;
  Literal.Value = Value;
  Literal.Suffix = std::string(Token.substr(At));
  std::string Lower;
  for (const char C : Literal.Suffix)
    Lower += char(std::tolower(static_cast<unsigned char>(C)));
  switch (Syntax) {
  case LiteralSyntax::C: {
    // `ll` and `LL` are suffixes, `lL` is not.
    const auto L = Literal.Suffix.find_first_of("lL");
    if (L != std::string::npos && L + 1 < Literal.Suffix.size() &&
        std::tolower(static_cast<unsigned char>(Literal.Suffix[L + 1])) ==
            'l' &&
        Literal.Suffix[L] != Literal.Suffix[L + 1])
      return std::nullopt;
    for (const std::string_view Valid :
         {"", "u", "l", "ul", "lu", "ll", "ull", "llu"})
      if (Lower == Valid)
        return Literal;
    return std::nullopt;
  }
  case LiteralSyntax::Rust: {
    std::string_view Type = Lower;
    if (!Type.empty() && Type.front() == '_')
      Type.remove_prefix(1);
    for (const std::string_view Valid :
         {"", "u8", "u16", "u32", "u64", "u128", "usize", "i8", "i16", "i32",
          "i64", "i128", "isize"})
      if (Type == Valid && (Type.empty() == Lower.empty()))
        return Literal;
    return std::nullopt;
  }
  case LiteralSyntax::Go:
    if (Lower.empty())
      return Literal;
    return std::nullopt;
  }
  return std::nullopt;
}

/// A C type as its width in bits and signedness.
using LiteralType = std::pair<unsigned, bool>;

/// The type C gives a literal of \p Value written in decimal or another base
/// with \p Suffix, where long has \p LongBits: the first of the suffix's list
/// that holds the value (C17 6.4.4.1). None when no type of the list does.
inline std::optional<LiteralType> literalCType(uint64_t Value, bool Decimal,
                                               std::string_view Suffix,
                                               unsigned LongBits) {
  bool Unsigned = false;
  unsigned Longs = 0;
  for (const char C : Suffix) {
    if (C == 'u' || C == 'U')
      Unsigned = true;
    else
      ++Longs;
  }
  const LiteralType Int{32, true}, UInt{32, false}, Long{LongBits, true},
      ULong{LongBits, false}, LongLong{64, true}, ULongLong{64, false};
  std::vector<LiteralType> Types;
  if (!Unsigned && Longs == 0)
    Types = Decimal ? std::vector{Int, Long, LongLong}
                    : std::vector{Int, UInt, Long, ULong, LongLong, ULongLong};
  else if (!Unsigned && Longs == 1)
    Types = Decimal ? std::vector{Long, LongLong}
                    : std::vector{Long, ULong, LongLong, ULongLong};
  else if (!Unsigned)
    Types = Decimal ? std::vector{LongLong} : std::vector{LongLong, ULongLong};
  else if (Longs == 0)
    Types = {UInt, ULong, ULongLong};
  else if (Longs == 1)
    Types = {ULong, ULongLong};
  else
    Types = {ULongLong};
  for (const auto &Type : Types) {
    const unsigned Bits = Type.second ? Type.first - 1 : Type.first;
    if (Bits >= 64 || Value <= (uint64_t{1} << Bits) - 1)
      return Type;
  }
  return std::nullopt;
}

/// The C type of \p Literal under LP64 and LLP64, which differ in long's width.
inline std::optional<std::pair<LiteralType, LiteralType>>
literalCTypes(const IntegerLiteral &Literal) {
  if (Literal.Radix == LiteralRadix::Char)
    return std::pair{LiteralType{32, true}, LiteralType{32, true}};
  const bool Decimal = Literal.Radix == LiteralRadix::Decimal;
  const auto LP64 = literalCType(Literal.Value, Decimal, Literal.Suffix, 64);
  const auto LLP64 = literalCType(Literal.Value, Decimal, Literal.Suffix, 32);
  if (!LP64 || !LLP64)
    return std::nullopt;
  return std::pair{*LP64, *LLP64};
}

/// \p Value's digits in \p Base: hexadecimal in upper case, as the
/// decompiler writes it.
inline std::string literalDigits(uint64_t Value, unsigned Base) {
  std::string Digits;
  do {
    Digits.insert(Digits.begin(), "0123456789ABCDEF"[Value % Base]);
    Value /= Base;
  } while (Value);
  return Digits;
}

/// \p Value as a C character constant, the most significant byte first, or
/// none unless it is an int whose every byte is an ASCII character.
inline std::optional<std::string> literalCharacter(uint64_t Value) {
  if (Value == 0)
    return std::string("'\\0'");
  if (Value > 0x7FFFFFFF)
    return std::nullopt;
  std::vector<unsigned> Bytes;
  for (; Value; Value >>= 8)
    Bytes.insert(Bytes.begin(), unsigned(Value & 0xFF));
  std::string Text = "'";
  for (const unsigned Byte : Bytes) {
    if (Byte == 0 || Byte > 0x7F)
      return std::nullopt;
    switch (Byte) {
    case '\'':
      Text += "\\'";
      continue;
    case '\\':
      Text += "\\\\";
      continue;
    case '\n':
      Text += "\\n";
      continue;
    case '\t':
      Text += "\\t";
      continue;
    case '\r':
      Text += "\\r";
      continue;
    case 7:
      Text += "\\a";
      continue;
    case 8:
      Text += "\\b";
      continue;
    case 11:
      Text += "\\v";
      continue;
    case 12:
      Text += "\\f";
      continue;
    default:
      break;
    }
    if (Byte >= 0x20 && Byte < 0x7F) {
      Text += char(Byte);
      continue;
    }
    // Octal escapes end after three digits; a hexadecimal one would take the
    // next character's digits too.
    Text += '\\';
    Text += char('0' + ((Byte >> 6) & 7));
    Text += char('0' + ((Byte >> 3) & 7));
    Text += char('0' + (Byte & 7));
  }
  return Text + "'";
}

} // namespace detail

/// The integer literals of \p Line in order, outside comments and string
/// literals. \p InComment says whether the line starts inside a block
/// comment, and on return whether the next line does.
inline std::vector<IntegerLiteral>
integerLiterals(std::string_view Line, LiteralSyntax Syntax, bool &InComment) {
  using namespace detail;
  std::vector<IntegerLiteral> Literals;
  const std::size_t N = Line.size();
  for (std::size_t I = 0; I < N;) {
    if (InComment) {
      const auto End = Line.find("*/", I);
      if (End == std::string_view::npos)
        return Literals;
      I = End + 2;
      InComment = false;
      continue;
    }
    const char C = Line[I];
    if (C == '/' && I + 1 < N && Line[I + 1] == '/')
      return Literals;
    if (C == '/' && I + 1 < N && Line[I + 1] == '*') {
      InComment = true;
      I += 2;
      continue;
    }
    if (C == '"') {
      I = literalSkipQuoted(Line, I, '"');
      continue;
    }
    if (C == '`' && Syntax == LiteralSyntax::Go) {
      const auto End = Line.find('`', I + 1);
      I = End == std::string_view::npos ? N : End + 1;
      continue;
    }
    if (C == '\'') {
      if (Syntax == LiteralSyntax::Rust) {
        // A char literal, or a label or lifetime such as 'a, which has no
        // closing quote after its first character.
        std::size_t J = I + 1;
        if (J < N && Line[J] == '\\') {
          I = literalSkipQuoted(Line, I, '\'');
          continue;
        }
        ++J;
        while (J < N && (static_cast<unsigned char>(Line[J]) & 0xC0) == 0x80)
          ++J;
        if (J < N && Line[J] == '\'') {
          I = J + 1;
          continue;
        }
        while (J < N && literalIdentifierPart(Line[J]))
          ++J;
        I = J;
        continue;
      }
      const std::size_t End = literalSkipQuoted(Line, I, '\'');
      if (Syntax == LiteralSyntax::C && End > I + 2 && Line[End - 1] == '\'')
        if (const auto Value =
                literalCharacterValue(Line.substr(I + 1, End - I - 2))) {
          IntegerLiteral Literal;
          Literal.Begin = I;
          Literal.End = End;
          Literal.Text = std::string(Line.substr(I, End - I));
          Literal.Value = Value->first;
          Literal.Exact = Value->second;
          Literal.Radix = LiteralRadix::Char;
          Literals.push_back(std::move(Literal));
        }
      I = End;
      continue;
    }
    if (literalIdentifierStart(C)) {
      std::size_t J = I + 1;
      while (J < N && literalIdentifierPart(Line[J]))
        ++J;
      const std::string_view Word = Line.substr(I, J - I);
      // A prefixed string or character literal: L'x', u8"x", b'x', r#"x"#.
      if (J < N && (Line[J] == '"' || Line[J] == '\'')) {
        I = literalSkipQuoted(Line, J, Line[J]);
        continue;
      }
      if (Syntax == LiteralSyntax::Rust && (Word == "r" || Word == "br") &&
          J < N && Line[J] == '#') {
        std::size_t Quote = J;
        while (Quote < N && Line[Quote] == '#')
          ++Quote;
        if (Quote < N && Line[Quote] == '"') {
          const std::string Close =
              "\"" + std::string(Line.substr(J, Quote - J));
          const auto End = Line.find(Close, Quote + 1);
          I = End == std::string_view::npos ? N : End + Close.size();
          continue;
        }
      }
      I = J;
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(C))) {
      const char Base =
          I + 1 < N && C == '0'
              ? char(std::tolower(static_cast<unsigned char>(Line[I + 1])))
              : '\0';
      const bool Prefixed = Base == 'x' || Base == 'b' || Base == 'o';
      std::size_t J = I + 1;
      while (J < N) {
        const char D = Line[J];
        if (std::isalnum(static_cast<unsigned char>(D)) || D == '_' ||
            D == '.') {
          ++J;
          continue;
        }
        // An exponent's sign belongs to the number: C's preprocessing number
        // takes it after any e or p, Rust and Go after a digit's decimal e
        // or a Go hexadecimal p.
        const char E =
            char(std::tolower(static_cast<unsigned char>(Line[J - 1])));
        const bool AfterDigit =
            J >= I + 2 && std::isdigit(static_cast<unsigned char>(Line[J - 2]));
        if ((D == '+' || D == '-') &&
            (Syntax == LiteralSyntax::C
                 ? (E == 'e' || E == 'p')
                 : AfterDigit && ((E == 'e' && !Prefixed) ||
                                  (E == 'p' && Base == 'x' &&
                                   Syntax == LiteralSyntax::Go)))) {
          ++J;
          continue;
        }
        break;
      }
      if (auto Literal = literalToken(Line.substr(I, J - I), Syntax)) {
        Literal->Begin = I;
        Literal->End = J;
        Literals.push_back(std::move(*Literal));
      }
      I = J;
      continue;
    }
    ++I;
  }
  return Literals;
}

/// The base a decompiler writes \p Value in when the user picked none:
/// hexadecimal, as the disassembly shows numbers, unless the value is at most
/// ten or its decimal digits read rounder than its hexadecimal ones (100,
/// 999, 5000, 1000000 against 0x64, 0x3E7, 0x1388, 0xF4240). The line is
/// Ghidra's PrintLanguage::mostNaturalBase: a run of trailing 0s or 9s in
/// decimal, longer than the run of trailing 0s or Fs in hexadecimal, with
/// little left before it.
inline LiteralRadix naturalLiteralRadix(uint64_t Value) {
  if (Value <= 10)
    return LiteralRadix::Decimal;
  unsigned DecimalRun = 0;
  uint64_t Rest = Value;
  if (const uint64_t Repeated = Rest % 10; Repeated == 0 || Repeated == 9)
    for (; Rest && Rest % 10 == Repeated; Rest /= 10)
      ++DecimalRun;
  const uint64_t Limit = DecimalRun == 0   ? 0
                         : DecimalRun == 1 ? (Value % 10 == 9 ? 0 : 1)
                         : DecimalRun == 2 ? 10
                         : DecimalRun <= 4 ? 100
                                           : 1000;
  if (DecimalRun == 0 || Rest > Limit)
    return LiteralRadix::Hex;
  unsigned HexRun = 0;
  Rest = Value;
  if (const uint64_t Repeated = Rest & 0xF; Repeated == 0 || Repeated == 0xF)
    for (; Rest && (Rest & 0xF) == Repeated; Rest >>= 4)
      ++HexRun;
  return DecimalRun > HexRun ? LiteralRadix::Decimal : LiteralRadix::Hex;
}

/// \p Literal spelled in \p Radix with its type, or none when no spelling in
/// that base has it, the base is Number, or the language fixes no value.
inline std::optional<std::string>
spellIntegerLiteral(const IntegerLiteral &Literal, LiteralRadix Radix,
                    LiteralSyntax Syntax) {
  using namespace detail;
  if (Radix == LiteralRadix::Number || !Literal.Exact)
    return std::nullopt;
  if (Radix == Literal.Radix)
    return Literal.Text;
  if (Radix == LiteralRadix::Char) {
    // A character constant is a C int.
    const auto Types =
        Syntax == LiteralSyntax::C ? literalCTypes(Literal) : std::nullopt;
    if (!Types || Types->first != LiteralType{32, true} ||
        Types->second != LiteralType{32, true})
      return std::nullopt;
    return literalCharacter(Literal.Value);
  }
  std::string Digits;
  switch (Radix) {
  case LiteralRadix::Hex:
    Digits = "0x" + literalDigits(Literal.Value, 16);
    break;
  case LiteralRadix::Decimal:
    Digits = literalDigits(Literal.Value, 10);
    break;
  case LiteralRadix::Octal:
    Digits = Syntax == LiteralSyntax::C
                 ? (Literal.Value ? "0" + literalDigits(Literal.Value, 8)
                                  : std::string("0"))
                 : "0o" + literalDigits(Literal.Value, 8);
    break;
  case LiteralRadix::Binary:
    Digits = "0b" + literalDigits(Literal.Value, 2);
    break;
  default:
    return std::nullopt;
  }
  if (Syntax != LiteralSyntax::C)
    return Digits + Literal.Suffix;
  const auto Types = literalCTypes(Literal);
  if (!Types)
    return std::nullopt;
  // The written suffix first, then the shortest that gives the same type.
  const bool Lower = Literal.Suffix.find_first_of("ul") != std::string::npos;
  std::vector<std::string> Suffixes{Literal.Suffix};
  for (const std::string_view Suffix : {"", "u", "ll", "ull"}) {
    std::string Spelled(Suffix);
    if (!Lower)
      for (auto &C : Spelled)
        C = char(std::toupper(static_cast<unsigned char>(C)));
    Suffixes.push_back(std::move(Spelled));
  }
  const bool Decimal = Radix == LiteralRadix::Decimal;
  for (const auto &Suffix : Suffixes) {
    const auto LP64 = literalCType(Literal.Value, Decimal, Suffix, 64);
    const auto LLP64 = literalCType(Literal.Value, Decimal, Suffix, 32);
    if (LP64 == Types->first && LLP64 == Types->second)
      return Digits + Suffix;
  }
  return std::nullopt;
}

} // namespace neverd

#endif // NEVERD_SUPPORT_INTEGERLITERALS_H
