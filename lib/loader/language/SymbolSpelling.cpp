//===- SymbolSpelling.cpp - How a symbol name reads -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Finds the scheme that spells a symbol name (SymbolSchemes.def) and asks
/// that scheme's own file how the name reads.
///
//===----------------------------------------------------------------------===//

#include "neverd/loader/SymbolSpelling.h"

#include "SymbolSpellingDetail.h"

#include <cstdlib>
#include <utility>

using namespace neverd;
using namespace neverd::symbol_spelling;

namespace {

/// Whether \p Name starts with a prefix of \p Scheme.
bool hasSchemePrefix(SymbolScheme Scheme, llvm::StringRef Name) {
#define NEVERD_SYMBOL_SCHEME_PREFIX(Id, Prefix)                                \
  if (Scheme == SymbolScheme::Id && Name.starts_with(Prefix))                  \
    return true;
#include "neverd/loader/SymbolSchemes.def"
  return false;
}

/// The scheme that spells \p Name and the name without the underscore Mach-O
/// adds, when it has one.
std::pair<SymbolScheme, llvm::StringRef> classify(llvm::StringRef Name) {
  for (llvm::StringRef Candidate :
       {Name, Name.starts_with("_") ? Name.drop_front() : llvm::StringRef()}) {
    if (Candidate.empty())
      continue;
#define NEVERD_SYMBOL_SCHEME(Id, Language)                                     \
  if (hasSchemePrefix(SymbolScheme::Id, Candidate) &&                          \
      claims##Id##Name(Candidate))                                             \
    return {SymbolScheme::Id, Candidate};
#include "neverd/loader/SymbolSchemes.def"
  }
  return {SymbolScheme::None, Name};
}

} // namespace

std::string symbol_spelling::joinStemWords(llvm::ArrayRef<std::string> Words) {
  std::string Stem;
  for (const std::string &Word : Words) {
    if (Word.empty())
      continue;
    if (!Stem.empty())
      Stem += '_';
    Stem += Word;
  }
  return Stem;
}

std::string symbol_spelling::takeDemangled(char *Buffer) {
  if (!Buffer)
    return {};
  std::string Text(Buffer);
  std::free(Buffer);
  return Text;
}

SymbolScheme neverd::symbolScheme(llvm::StringRef Name) {
  return classify(Name).first;
}

llvm::StringRef neverd::symbolSchemeLanguage(SymbolScheme Scheme) {
#define NEVERD_SYMBOL_SCHEME(Id, Language)                                     \
  if (Scheme == SymbolScheme::Id)                                              \
    return Language;
#include "neverd/loader/SymbolSchemes.def"
  return {};
}

std::string neverd::readableSymbolName(llvm::StringRef Name) {
  const auto [Scheme, Mangled] = classify(Name);
  switch (Scheme) {
  case SymbolScheme::None:
    return {};
#define NEVERD_SYMBOL_SCHEME(Id, Language)                                     \
  case SymbolScheme::Id:                                                       \
    return readable##Id##Name(Mangled);
#include "neverd/loader/SymbolSchemes.def"
  }
  return {};
}

std::string neverd::displaySymbolName(llvm::StringRef Name) {
  std::string Readable = readableSymbolName(Name);
  return Readable.empty() ? Name.str() : Readable;
}

std::string neverd::symbolIdentifierStem(llvm::StringRef Name) {
  const auto [Scheme, Mangled] = classify(Name);
  switch (Scheme) {
  case SymbolScheme::None:
    return {};
#define NEVERD_SYMBOL_SCHEME(Id, Language)                                     \
  case SymbolScheme::Id:                                                       \
    return stemOf##Id##Name(Mangled);
#include "neverd/loader/SymbolSchemes.def"
  }
  return {};
}
