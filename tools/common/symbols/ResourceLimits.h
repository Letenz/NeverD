//===- ResourceLimits.h - Symbol transfer limits and timeouts --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared C++ view of the symbol transport definition table.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_COMMON_SYMBOLS_RESOURCELIMITS_H
#define NEVERD_TOOLS_COMMON_SYMBOLS_RESOURCELIMITS_H

namespace neverd::limits::symbol_download {
#define NEVERD_SYMBOL_LIMIT(Name, Value) inline constexpr auto k##Name = Value;
#include "SymbolDownloadABI.def"
} // namespace neverd::limits::symbol_download

#endif // NEVERD_TOOLS_COMMON_SYMBOLS_RESOURCELIMITS_H
