//===- SymbolSpellingDetail.h - One scheme's symbol spellings --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// What each scheme of SymbolSchemes.def provides SymbolSpelling.cpp, which
/// tries the schemes in their table order.  Every scheme defines, in its own
/// file:
///
///   bool claims<Id>Name(StringRef Name)
///       whether a name that starts with one of the scheme's prefixes is
///       spelled in the scheme
///   std::string readable<Id>Name(StringRef Name)
///       the name as its language spells it, or empty
///   std::string stemOf<Id>Name(StringRef Name)
///       the C identifier stem, or empty
///
/// Name starts with one of the scheme's prefixes: SymbolSpelling.cpp has
/// already removed the underscore Mach-O adds.
///
/// This header is an implementation detail of the loader library and should
/// NOT be included by code outside lib/loader/language/.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_LANGUAGE_SYMBOLSPELLINGDETAIL_H
#define NEVERD_LOADER_LANGUAGE_SYMBOLSPELLINGDETAIL_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <string>

namespace neverd {
namespace symbol_spelling {

#define NEVERD_SYMBOL_SCHEME(Id, Language)                                     \
  bool claims##Id##Name(llvm::StringRef Name);                                 \
  std::string readable##Id##Name(llvm::StringRef Name);                        \
  std::string stemOf##Id##Name(llvm::StringRef Name);
#include "neverd/loader/SymbolSchemes.def"

/// \p Words joined with underscores, skipping empty ones.
std::string joinStemWords(llvm::ArrayRef<std::string> Words);

/// The text a demangler returned in \p Buffer, which it allocated with
/// malloc, or empty for none.
std::string takeDemangled(char *Buffer);

} // namespace symbol_spelling
} // namespace neverd

#endif // NEVERD_LOADER_LANGUAGE_SYMBOLSPELLINGDETAIL_H
