//===- SymbolDecoration.h - C names and the symbols they link as -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Converts between a C name and the symbol an object format links it as
/// (SymbolDecorations.def).  A symbol's own leading underscores beyond the
/// format's decoration are part of the name: ELF `__libc_start_main` and
/// `_ZdlPv` keep them.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_SYMBOLDECORATION_H
#define NEVERD_LOADER_SYMBOLDECORATION_H

#include "neverd/Common.h"

#include "llvm/ADT/StringRef.h"

#include <string>

namespace neverd {

/// Whether \p Format on \p Target links a C name with one added leading
/// underscore.
inline bool hasABIUnderscore(BinaryFormat Format, Arch Target) {
#define NEVERD_ABI_UNDERSCORE(FormatId)                                        \
  if (Format == BinaryFormat::FormatId)                                        \
    return true;
#define NEVERD_ABI_UNDERSCORE_ON(FormatId, ArchId)                             \
  if (Format == BinaryFormat::FormatId && Target == Arch::ArchId)              \
    return true;
#include "neverd/loader/SymbolDecorations.def"
  return false;
}

/// The C name symbol \p Symbol spells: without the format's one underscore.
inline llvm::StringRef cNameOfSymbol(llvm::StringRef Symbol,
                                     BinaryFormat Format, Arch Target) {
  if (hasABIUnderscore(Format, Target))
    Symbol.consume_front("_");
  return Symbol;
}

/// Whether \p Name is already the symbol on \p Format and \p Target, which a
/// format's underscore does not decorate (SymbolDecorations.def).
inline bool isUndecoratedSymbol(llvm::StringRef Name, BinaryFormat Format,
                                Arch Target) {
#define NEVERD_ABI_UNDECORATED_PREFIX(FormatId, ArchId, Prefix)                \
  if (Format == BinaryFormat::FormatId && Target == Arch::ArchId &&            \
      Name.starts_with(Prefix))                                                \
    return true;
#include "neverd/loader/SymbolDecorations.def"
  return false;
}

/// The symbol C name \p Name links as, which an assembler label must spell.
inline std::string symbolOfCName(llvm::StringRef Name, BinaryFormat Format,
                                 Arch Target) {
  const bool Underscore = hasABIUnderscore(Format, Target) &&
                          !isUndecoratedSymbol(Name, Format, Target);
  return (llvm::StringRef(Underscore ? "_" : "") + Name).str();
}

/// The prefix the linker of \p Format gives the symbol of an import's
/// address slot (SymbolDecorations.def), or empty when it names none.
inline llvm::StringRef importSlotPrefix(BinaryFormat Format) {
#define NEVERD_IMPORT_SLOT_PREFIX(FormatId, Prefix)                            \
  if (Format == BinaryFormat::FormatId)                                        \
    return Prefix;
#include "neverd/loader/SymbolDecorations.def"
  return {};
}

/// Whether an import entry of \p Format names its function by the C name
/// rather than by its symbol (SymbolDecorations.def).
inline bool importNamesAreCNames(BinaryFormat Format) {
#define NEVERD_IMPORT_C_NAMES(FormatId)                                        \
  if (Format == BinaryFormat::FormatId)                                        \
    return true;
#include "neverd/loader/SymbolDecorations.def"
  return false;
}

/// The symbol the import its import entry names \p Name links as, so that
/// cNameOfSymbol gives that name back: 32-bit Windows' `_initterm` is the
/// symbol `__initterm`, not the C name `initterm`.
inline std::string symbolOfImportName(llvm::StringRef Name, BinaryFormat Format,
                                      Arch Target) {
  return importNamesAreCNames(Format) ? symbolOfCName(Name, Format, Target)
                                      : Name.str();
}

} // namespace neverd

#endif // NEVERD_LOADER_SYMBOLDECORATION_H
