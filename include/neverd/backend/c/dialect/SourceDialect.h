//===- SourceDialect.h - Pseudocode in its source language ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The C the HighC emitter writes, spelled in the language a function was
/// written in: Rust for Rust code, Go for Go code.  The C stays the semantic
/// authority.  A dialect spells exactly what the C says, with C's implicit
/// conversions written out and the names of the source language
/// (`core::fmt::write`, `internal/cpu.Initialize`), and shows a declaration
/// as C, with the reason, where it cannot.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_DIALECT_SOURCEDIALECT_H
#define NEVERD_BACKEND_C_DIALECT_SOURCEDIALECT_H

#include "neverd/Common.h"
#include "neverd/loader/ExceptionCommon.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <optional>
#include <string>
#include <vector>

namespace neverd {

struct CSourceName;

enum class SourceDialect : uint8_t {
#define NEVERD_SOURCE_DIALECT(Id, Key, Display) Id,
#include "neverd/backend/c/dialect/SourceDialects.def"
};

/// `c`, `rust` or `go`.
llvm::StringRef sourceDialectKey(SourceDialect Dialect);
/// `C`, `Rust` or `Go`.
llvm::StringRef sourceDialectDisplayName(SourceDialect Dialect);
std::optional<SourceDialect> sourceDialectFromKey(llvm::StringRef Key);

/// The dialect a runtime's code reads in other than C: Rust for Rust, Go for
/// Go.
std::optional<SourceDialect> dialectOfRuntime(SourceLanguageRuntime Runtime);

/// The dialects an image's pseudocode is offered in: C, and Rust or Go where
/// a runtime of the image is that language.  A C or C++ program reads in C
/// alone.
std::vector<SourceDialect>
offeredSourceDialects(const LanguageRuntimeInfo &Language);

/// The dialect a function of an image reads best in, among those the image
/// is offered in: Rust for a Rust symbol, Go for a Go symbol, C for any
/// other name, and the image's own language for a function the image does
/// not name (\p Named false).
SourceDialect sourceDialectOfFunction(llvm::StringRef Symbol, bool Named,
                                      const LanguageRuntimeInfo &Language);

struct SourceDialectOptions {
  SourceDialect Dialect = SourceDialect::C;
  Arch TheArch = Arch::X64;
  BinaryFormat Format = BinaryFormat::ELF;
  /// The names the C text spells, from CSourceMap::Names.
  llvm::ArrayRef<CSourceName> Names;
};

/// A statement or declaration of the dialect text and the C bytes it spells.
struct SourceDialectPiece {
  size_t CBegin = 0;
  size_t CEnd = 0;
  size_t Begin = 0;
  size_t End = 0;
};

/// A source language's name in the dialect text, which splitting the text
/// into identifiers would not find whole: `core::fmt::write`.
struct SourceDialectName {
  size_t Begin = 0;
  size_t End = 0;
  /// The C identifier the C view spells for it.
  std::string Identifier;
  std::string Symbol;
  std::optional<va_t> Address;
};

struct SourceDialectText {
  std::string Text;
  /// In text order.
  std::vector<SourceDialectPiece> Pieces;
  /// Why each declaration shown as C could not be spelled, in text order.
  std::vector<std::string> Unread;
  /// In text order.
  std::vector<SourceDialectName> Names;

  /// The text that spells the C bytes [CBegin, CEnd): the pieces within
  /// them, or the smallest piece around them.
  std::optional<std::pair<size_t, size_t>> map(size_t CBegin,
                                               size_t CEnd) const;
  /// Where the text spelling the C from \p CBegin on starts: the first
  /// piece that starts there or after.
  std::optional<size_t> mapOffset(size_t CBegin) const;
};

/// \p C, a complete HighC emission, spelled in \p Options.Dialect.
SourceDialectText spellInDialect(llvm::StringRef C,
                                 const SourceDialectOptions &Options);

} // namespace neverd

#endif // NEVERD_BACKEND_C_DIALECT_SOURCEDIALECT_H
