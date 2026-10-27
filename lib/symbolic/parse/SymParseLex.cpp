//===- SymParseLex.cpp - Lexing symbolic expression text ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the lexer declared in SymParseDetail.h: the scan from input text
/// to the identifier, number and punctuator tokens the grammar in SymParse.cpp
/// consumes.
///
//===----------------------------------------------------------------------===//

#include "SymParseDetail.h"

#include <cctype>

namespace neverd::symbolic::detail {

namespace {

bool isDigit(char C) { return C >= '0' && C <= '9'; }

bool isRadixDigit(char C, unsigned Radix) {
  switch (Radix) {
  case 16:
    return isDigit(C) || (C >= 'a' && C <= 'f') || (C >= 'A' && C <= 'F');
  case 2:
    return C == '0' || C == '1';
  default:
    return isDigit(C);
  }
}

} // namespace

void Lexer::advance() {
  while (Pos < Text.size() &&
         std::isspace(static_cast<unsigned char>(Text[Pos])))
    ++Pos;

  Cur = Token{};
  Cur.Offset = Pos;
  if (Pos >= Text.size())
    return;

  char C = Text[Pos];

  if (isIdentStart(C)) {
    size_t Start = Pos;
    while (Pos < Text.size() && isIdentBody(Text[Pos]))
      ++Pos;
    Cur.K = Token::Ident;
    Cur.Text = Text.slice(Start, Pos);
    return;
  }

  if (isDigit(C)) {
    unsigned Radix = 10;
    if (C == '0' && Pos + 1 < Text.size()) {
      char P = Text[Pos + 1];
      if (P == 'x' || P == 'X')
        Radix = 16;
      else if (P == 'b' || P == 'B')
        Radix = 2;
      if (Radix != 10)
        Pos += 2;
    }
    size_t Start = Pos;
    while (Pos < Text.size() && isRadixDigit(Text[Pos], Radix))
      ++Pos;
    Cur.K = Token::Number;
    Cur.Digits = Text.slice(Start, Pos);
    Cur.Radix = Radix;
    return;
  }

  // Only these prefixes have a two-character spelling. Dispatch once instead
  // of repeatedly measuring and comparing every punctuator at every token.
  // Taking the pair first preserves maximal munch; all other bytes retain the
  // one-character fallback, including invalid punctuation and embedded NULs.
  size_t Length = 1;
  if (Pos + 1 < Text.size()) {
    const char Next = Text[Pos + 1];
    switch (C) {
    case '<':
    case '>':
      Length += Next == C || Next == '=';
      break;
    case '=':
    case '!':
      Length += Next == '=';
      break;
    case '&':
    case '|':
      Length += Next == C;
      break;
    default:
      break;
    }
  }
  Cur.K = Token::Punct;
  Cur.Text = Text.slice(Pos, Pos + Length);
  Pos += Length;
}

} // namespace neverd::symbolic::detail
