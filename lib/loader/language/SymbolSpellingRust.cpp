//===- SymbolSpellingRust.cpp - Rust symbol names -------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Rust names in both of rustc's manglings.  The legacy one is an Itanium
/// nested name whose identifiers escape path characters
/// (RustSymbolSpellings.def) and whose last component is a hash
/// (`17h0123456789abcdefE`); the hash identifies the crate build, not the
/// item, so a reader never sees it.  The v0 one (`_R...`) is read by LLVM's
/// demangler.
///
/// A C identifier stem joins the words of the path.  A qualified self type,
/// `<alloc::string::String as core::fmt::Write>::write_fmt`, contributes the
/// last word of the type and of the trait (`String_Write_write_fmt`): the
/// trait keeps the methods of `Display` and `Debug` apart.
///
//===----------------------------------------------------------------------===//

#include "SymbolSpellingDetail.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/Support/ConvertUTF.h"

#include <optional>
#include <vector>

using namespace neverd;
using namespace neverd::symbol_spelling;

namespace {

/// The legacy hash component: `h` and 16 lowercase hex digits.
constexpr size_t LegacyHashSize = 17;

bool isLegacyHash(llvm::StringRef Component) {
  return Component.size() == LegacyHashSize && Component.front() == 'h' &&
         llvm::all_of(Component.drop_front(), [](char C) {
           return llvm::isDigit(C) || (C >= 'a' && C <= 'f');
         });
}

/// The length-prefixed components of legacy name \p Name (`_ZN...E`), or none
/// when it is not one.
std::optional<llvm::SmallVector<llvm::StringRef, 8>>
legacyComponents(llvm::StringRef Name) {
  if (!Name.consume_front("_ZN") || !Name.consume_back("E"))
    return std::nullopt;
  llvm::SmallVector<llvm::StringRef, 8> Components;
  while (!Name.empty()) {
    size_t Length = 0;
    if (Name.consumeInteger(10, Length) || Length == 0 || Length > Name.size())
      return std::nullopt;
    Components.push_back(Name.take_front(Length));
    Name = Name.drop_front(Length);
  }
  if (Components.size() < 2 || !isLegacyHash(Components.back()))
    return std::nullopt;
  return Components;
}

/// Legacy identifier \p Text with its escapes read, or none when one does not
/// read.
std::optional<std::string> readLegacyIdentifier(llvm::StringRef Text) {
  // An identifier that would start with `$` starts with `_$` instead.
  if (Text.starts_with("_$"))
    Text = Text.drop_front();
  std::string Out;
  while (!Text.empty()) {
    if (Text.consume_front("..")) {
      Out += "::";
      continue;
    }
    if (!Text.starts_with("$")) {
      Out += Text.front();
      Text = Text.drop_front();
      continue;
    }
    const size_t End = Text.find('$', 1);
    if (End == llvm::StringRef::npos)
      return std::nullopt;
    const llvm::StringRef Code = Text.slice(1, End);
    Text = Text.drop_front(End + 1);
    bool Known = false;
#define NEVERD_RUST_LEGACY_ESCAPE(Escape, Replacement)                         \
  if (!Known && Code == Escape) {                                              \
    Out += Replacement;                                                        \
    Known = true;                                                              \
  }
#include "neverd/loader/RustSymbolSpellings.def"
    if (Known)
      continue;
    unsigned Scalar = 0;
    if (!Code.starts_with("u") || Code.size() < 2 ||
        Code.drop_front().getAsInteger(16, Scalar))
      return std::nullopt;
    char Encoded[UNI_MAX_UTF8_BYTES_PER_CODE_POINT];
    char *Cursor = Encoded;
    if (!llvm::ConvertCodePointToUTF8(Scalar, Cursor))
      return std::nullopt;
    Out.append(Encoded, Cursor);
  }
  return Out;
}

/// Where the top-level `Separator`s of \p Text are: outside any bracket.
llvm::SmallVector<llvm::StringRef, 8> splitTopLevel(llvm::StringRef Text,
                                                    llvm::StringRef Separator) {
  llvm::SmallVector<llvm::StringRef, 8> Pieces;
  int Depth = 0;
  size_t Start = 0;
  for (size_t I = 0; I < Text.size(); ++I) {
    const char C = Text[I];
    if (C == '<' || C == '(' || C == '[' || C == '{') {
      ++Depth;
      continue;
    }
    if (C == '>' || C == ')' || C == ']' || C == '}') {
      // `->` in a function type is not a closing bracket.
      if (C == '>' && I > 0 && Text[I - 1] == '-')
        continue;
      --Depth;
      continue;
    }
    if (Depth == 0 && Text.substr(I).starts_with(Separator)) {
      Pieces.push_back(Text.slice(Start, I));
      I += Separator.size() - 1;
      Start = I + 1;
    }
  }
  Pieces.push_back(Text.substr(Start));
  return Pieces;
}

/// \p Segment without the generic arguments it ends in: `Vec<T>` is `Vec`.
llvm::StringRef withoutGenericArguments(llvm::StringRef Segment) {
  return Segment.take_front(Segment.find('<'));
}

std::string segmentWord(llvm::StringRef Segment, bool First);
std::string rustPathStem(llvm::StringRef Path);

/// Whether \p Type is spelled as a path, not as a reference, slice, tuple or
/// function type.
bool isPathType(llvm::StringRef Type) {
  return !Type.empty() && (llvm::isAlpha(Type.front()) || Type.front() == '_' ||
                           Type.front() == '<');
}

/// The word a type contributes to a stem: the last word of its path, and a
/// name for the types that have no path.
std::string typeWord(llvm::StringRef Type) {
  Type = Type.trim();
  for (llvm::StringRef Prefix :
       {"&mut ", "&", "*const ", "*mut ", "dyn ", "impl "})
    if (Type.consume_front(Prefix))
      return typeWord(Type);
  if (Type.starts_with("["))
    return Type.contains(';') ? "array" : "slice";
  if (Type.starts_with("("))
    return Type == "()" ? "unit" : "tuple";
  if (Type.starts_with("fn(") || Type.starts_with("unsafe fn(") ||
      Type.starts_with("extern "))
    return "fn";
  const auto Segments = splitTopLevel(Type, "::");
  return segmentWord(Segments.back(), Segments.size() == 1);
}

/// The word one path segment contributes to a stem.  \p First is whether it
/// starts the path, where `<...>` is a qualified self type rather than a
/// turbofish's generic arguments.
std::string segmentWord(llvm::StringRef Segment, bool First) {
  if (Segment.starts_with("<") && Segment.ends_with(">")) {
    if (!First)
      return {};
    const llvm::StringRef Inner = Segment.drop_front().drop_back();
    const auto Parts = splitTopLevel(Inner, " as ");
    // An inherent impl's methods read under the type's whole path, as the
    // legacy mangling prints them: `<core::fmt::Formatter>::pad` is
    // `core::fmt::Formatter::pad`.
    if (Parts.size() == 1 && isPathType(Parts.front().trim()))
      return rustPathStem(Parts.front().trim());
    std::vector<std::string> Words;
    for (llvm::StringRef Part : Parts)
      Words.push_back(typeWord(Part));
    return joinStemWords(Words);
  }
  if (Segment.starts_with("{") && Segment.ends_with("}")) {
    // v0 `{closure#1}`, `{shim:vtable#0}`; legacy `{{closure}}`.
    llvm::StringRef Inner = Segment.drop_front().drop_back();
    if (Inner.starts_with("{") && Inner.ends_with("}"))
      Inner = Inner.drop_front().drop_back();
    auto [Kind, Index] = Inner.split('#');
    auto [Namespace, Name] = Kind.split(':');
    std::string Word = joinStemWords({Name.str(), Namespace.str()});
    unsigned Number = 0;
    if (!Index.getAsInteger(10, Number) && Number != 0)
      Word += llvm::utostr(Number);
    return Word;
  }
  return withoutGenericArguments(Segment).str();
}

/// The stem of Rust path \p Path as a demangler prints it.
std::string rustPathStem(llvm::StringRef Path) {
  std::vector<std::string> Words;
  const auto Segments = splitTopLevel(Path, "::");
  for (size_t I = 0; I < Segments.size(); ++I)
    Words.push_back(segmentWord(Segments[I], I == 0));
  return joinStemWords(Words);
}

} // namespace

bool symbol_spelling::claimsRustLegacyName(llvm::StringRef Name) {
  return legacyComponents(Name).has_value();
}

std::string symbol_spelling::readableRustLegacyName(llvm::StringRef Name) {
  const auto Components = legacyComponents(Name);
  if (!Components)
    return {};
  std::string Path;
  for (llvm::StringRef Component : llvm::ArrayRef(*Components).drop_back()) {
    const auto Identifier = readLegacyIdentifier(Component);
    if (!Identifier)
      return {};
    if (!Path.empty())
      Path += "::";
    Path += *Identifier;
  }
  return Path;
}

std::string symbol_spelling::stemOfRustLegacyName(llvm::StringRef Name) {
  return rustPathStem(readableRustLegacyName(Name));
}

bool symbol_spelling::claimsRustV0Name(llvm::StringRef Name) {
  // `_R`, an optional encoding version, then the tag a path starts with:
  // crate root, inherent impl, trait impl, trait definition, nested path,
  // generic arguments or back reference.  `_RTC_CheckEsp` is MSVC's.
  llvm::StringRef Rest = Name.drop_front(2);
  Rest = Rest.drop_while(llvm::isDigit);
  return !Rest.empty() && llvm::StringRef("CMXYNIB").contains(Rest.front());
}

std::string symbol_spelling::readableRustV0Name(llvm::StringRef Name) {
  return takeDemangled(llvm::rustDemangle(Name));
}

std::string symbol_spelling::stemOfRustV0Name(llvm::StringRef Name) {
  return rustPathStem(readableRustV0Name(Name));
}
