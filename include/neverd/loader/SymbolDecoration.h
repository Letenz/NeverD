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

/// The symbol C name \p Name links as, which an assembler label must spell.
inline std::string symbolOfCName(llvm::StringRef Name, BinaryFormat Format,
                                 Arch Target) {
  return (llvm::StringRef(hasABIUnderscore(Format, Target) ? "_" : "") + Name)
      .str();
}

} // namespace neverd

#endif // NEVERD_LOADER_SYMBOLDECORATION_H
