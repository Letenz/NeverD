//===- StaticSymbolTable.h - An image's static symbol table -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The static symbol table of a linked image, found before the image loads:
/// the table a load reads unless the caller declines it
/// (BinaryImage::ReadsStaticSymbols) and strip removes.  Anyone can write
/// names into it, so a frontend can ask before they load.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_STATICSYMBOLTABLE_H
#define NEVERD_LOADER_STATICSYMBOLTABLE_H

#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace neverd {

struct StaticSymbolTable {
  /// The table, as StaticSymbolTables.def spells it for the format.
  std::string Table;
  /// The defined names a load reads from it.
  uint64_t Count = 0;
  /// Its first function names in table order, the global ones first.
  std::vector<std::string> Names;
};

/// The static symbol table of the image at \p Path, with up to \p MaxNames
/// of its function names.  Empty for an image without one, for an object
/// file, which is never read without its table, and for a format that has
/// none.  Fails only when the file cannot be read.
llvm::Expected<std::optional<StaticSymbolTable>>
findStaticSymbolTable(const std::filesystem::path &Path, size_t MaxNames);

} // namespace neverd

#endif // NEVERD_LOADER_STATICSYMBOLTABLE_H
