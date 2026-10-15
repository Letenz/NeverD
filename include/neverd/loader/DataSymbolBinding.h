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

/// The pointer-width data words the dynamic loader starts at a function
/// another module defines, by that function's name: an absolute symbol
/// relocation with no addend against a non-weak function symbol, as a pointer
/// variable's initializer (`void (*sink)(int) = exit;`).  The program may
/// store another function there, so the word holds a callable value without
/// being the function's import slot.  A word two such relocations name
/// differently is left out.
std::map<va_t, std::string>
collectExternalFunctionPointers(const BinaryImage &Image);
} // namespace neverd
#endif
