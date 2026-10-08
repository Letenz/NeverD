//===- SourceDialect.cpp - Pseudocode in its source language --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/c/dialect/SourceDialect.h"

#include "CSyntaxTree.h"
#include "DialectPrinter.h"

#include "neverd/loader/SymbolSpelling.h"

using namespace neverd;
using namespace neverd::csyntax;

llvm::StringRef neverd::sourceDialectKey(SourceDialect Dialect) {
  switch (Dialect) {
#define NEVERD_SOURCE_DIALECT(Id, Key, Display)                                \
  case SourceDialect::Id:                                                      \
    return Key;
#include "neverd/backend/c/dialect/SourceDialects.def"
  }
  return "c";
}

llvm::StringRef neverd::sourceDialectDisplayName(SourceDialect Dialect) {
  switch (Dialect) {
#define NEVERD_SOURCE_DIALECT(Id, Key, Display)                                \
  case SourceDialect::Id:                                                      \
    return Display;
#include "neverd/backend/c/dialect/SourceDialects.def"
  }
  return "C";
}

std::optional<SourceDialect> neverd::sourceDialectFromKey(llvm::StringRef Key) {
#define NEVERD_SOURCE_DIALECT(Id, DialectKey, Display)                         \
  if (Key == DialectKey)                                                       \
    return SourceDialect::Id;
#include "neverd/backend/c/dialect/SourceDialects.def"
  return std::nullopt;
}

SourceDialect neverd::sourceDialectOfSymbol(llvm::StringRef Symbol, bool Named,
                                            SourceDialect ImageDialect) {
  if (!Named)
    return ImageDialect;
  switch (symbolScheme(Symbol)) {
  case SymbolScheme::RustLegacy:
  case SymbolScheme::RustV0:
    return SourceDialect::Rust;
  case SymbolScheme::None:
    // Go names a function by its package path: `main.main`.
    if (ImageDialect == SourceDialect::Go && Symbol.contains('.'))
      return SourceDialect::Go;
    return SourceDialect::C;
  default:
    return SourceDialect::C;
  }
}

SourceDialectText neverd::spellInDialect(llvm::StringRef C,
                                         const SourceDialectOptions &Options) {
  Tree T(CDataModel::forTarget(Options.TheArch, Options.Format));
  T.Format = Options.Format;
  parse(C, T);
  check(T);
  std::unique_ptr<DialectPrinter> Printer;
  switch (Options.Dialect) {
  case SourceDialect::Rust:
    Printer = makeRustPrinter(T, Options);
    break;
  case SourceDialect::Go:
    Printer = makeGoPrinter(T, Options);
    break;
  case SourceDialect::C:
    Printer = makeCPrinter(T, Options);
    break;
  }
  return Printer->run();
}
