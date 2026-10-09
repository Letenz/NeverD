//===- DataSymbolBinding.h - Symbol-bound data pointer storage --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LOADER_DATASYMBOLBINDING_H
#define NEVERD_LOADER_DATASYMBOLBINDING_H

#include "neverd/Common.h"

#include <map>
#include <optional>
#include <string>

namespace neverd {
struct BinaryImage;

/// A dynamic relocation binds this pointer slot to a symbol's address.
/// Unknown (STT_NOTYPE) symbols remain untyped; TLS and callable resolvers
/// cannot be treated as ordinary object addresses.
struct DataSymbolBinding {
  std::string Name;
  std::optional<va_t> Definition;
  int64_t Addend = 0;
  bool Weak = false;
  bool Immutable = false;
  bool operator==(const DataSymbolBinding &) const = default;
};

/// Builds a snapshot from exact relocation symbol records, independent of
/// section names and symbol-name searches. Conflicting bindings fail clearly.
std::map<va_t, DataSymbolBinding>
collectDataSymbolBindings(const BinaryImage &Image);
} // namespace neverd
#endif
