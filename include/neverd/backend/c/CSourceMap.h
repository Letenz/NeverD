//===- CSourceMap.h - Sidecar mapping of emitted C to library evidence ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_CSOURCEMAP_H
#define NEVERD_BACKEND_C_CSOURCEMAP_H

#include "neverd/ir/high/HighSourceMap.h"

#include <optional>
#include <string>
#include <vector>

namespace neverd {

class LLVMSourceMap;

struct CSourceSpan {
  /// Half-open UTF-8 byte offsets in the exact, complete emitted C text.
  size_t Begin = 0;
  size_t End = 0;
  bool operator==(const CSourceSpan &) const = default;
};

/// Where a top-level function definition begins in the emitted C text, its
/// leading comments included. A funclet body printed inside its parent's
/// definition is not a definition of its own.
struct CSourceDefinition {
  /// The function's entry, when the emitter knows it.
  std::optional<va_t> Entry;
  /// UTF-8 byte offset in the exact, complete emitted C text.
  size_t Begin = 0;
  bool operator==(const CSourceDefinition &) const = default;
};

/// A function or object the emitted C names, and the symbol its identifier
/// stands for, so that a reader of the text can name it as its source
/// language does.
struct CSourceName {
  enum class Kind : uint8_t { Function, Object, Type };
  Kind TheKind = Kind::Function;
  /// The C identifier the text spells.
  std::string Identifier;
  /// The symbol as the image spells it (the debug type name for Type); empty
  /// for a name the emitter made
  /// itself (`off_4010`) and for one several symbols share (an MSVC stem).
  std::string Symbol;
  /// The function's entry or the object's address, when the emitter knows it.
  std::optional<va_t> Address;
  bool operator==(const CSourceName &) const = default;
};

struct CSourceRegion {
  /// Index into CSourceMap::Recognitions, shared by both C routes.
  size_t Recognition = 0;
  std::vector<CSourceSpan> Spans;
  bool Mapped = false;
};

/// A surviving emission event's original instruction occurrences. This is
/// navigation evidence, not a complete dependency/provenance claim.
struct CSourceAnchor {
  va_t Function = 0;
  CSourceSpan Span;
  std::vector<sigs::LibraryOccurrence> Occurrences;
};

/// Per-emission inputs and output, separate from lifted IR and from C text.
/// Input snapshots must outlive emission. Regions is replaced on every emit;
/// absence of a surviving, complete mapping leaves a region unfolded.
struct CSourceMap {
  const std::vector<sigs::LibraryRecognition> *Recognitions = nullptr;
  const HighSourceMap *HighSources = nullptr;
  const LLVMSourceMap *LLVMSources = nullptr;
  std::vector<CSourceRegion> Regions;
  /// Replaced on every emit; published only with byte-identical source text.
  std::vector<CSourceAnchor> Anchors;
  /// The top-level definitions in text order, replaced on every emit. Text
  /// before the first is the translation unit's prelude: includes, support
  /// types and declarations.
  std::vector<CSourceDefinition> Definitions;
  /// The functions and objects the text names, in identifier order, replaced
  /// on every emit.
  std::vector<CSourceName> Names;
};

} // namespace neverd

#endif
