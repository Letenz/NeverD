//===- PEImportShadow.h - Functions named like a PE import ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// PE binds an import to the DLL that exports it, so a local function whose
/// symbol spells an import's name is another function: MinGW's own atexit
/// calls _crt_atexit, while msvcrt.dll's atexit is the import.  The LLVM
/// route and its C printer give such a function its name with its address as
/// a suffix, so neither a call nor a reader takes it for the import.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_PEIMPORTSHADOW_H
#define NEVERD_BACKEND_LLVM_PEIMPORTSHADOW_H

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/SymbolDecoration.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"

#include <optional>
#include <set>
#include <string>

namespace neverd {

/// The C names the imports of \p Img link by when it is a PE image; none for
/// another format, where an import may bind to the image's own definition.
inline std::set<std::string> peImportCNames(const BinaryImage &Img) {
  std::set<std::string> Names;
  if (Img.Format == BinaryFormat::COFF)
    for (const Import &Imp : Img.Imports)
      if (!Imp.Name.empty())
        Names.insert(Imp.Name);
  return Names;
}

/// The name of the function at \p Entry of \p Img whose symbol spells
/// \p Name, when that is the C name of one of \p ImportCNames: \p Name with
/// the address as a suffix.  An import's stub keeps \p Name, since what it
/// calls is the import.
inline std::optional<std::string>
peImportShadowName(const std::set<std::string> &ImportCNames,
                   const BinaryImage &Img, va_t Entry, llvm::StringRef Name) {
  if (ImportCNames.empty() || Img.findImportAt(Entry) ||
      !ImportCNames.count(cNameOfSymbol(Name, Img.Format, Img.Arch).str()))
    return std::nullopt;
  return (Name + "_" + llvm::utohexstr(Entry)).str();
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_PEIMPORTSHADOW_H
