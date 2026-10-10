//===- HighSourceMap.h - Non-owning source expression observations --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_HIGHSOURCEMAP_H
#define NEVERD_IR_HIGH_HIGHSOURCEMAP_H

#include "neverd/sigs/LibraryRecognition.h"

#include <memory>
#include <optional>
#include <vector>

namespace neverd {

struct HighExpr;
enum class StmtKind : uint8_t;
enum class HighSourceKind { Expression, Store, Statement };

/// An expression produced from one concrete MedIR occurrence. Weak ownership
/// deliberately loses a mapping when a transformation replaces the expression.
/// Surviving expressions are checked again at emission; a pointer by itself
/// does not establish a source span or permission to fold a region.
struct HighSourceObservation {
  va_t Function = 0;
  sigs::LibraryOccurrence Occurrence;
  std::weak_ptr<const HighExpr> Expression;
  /// Stores retain their original HighStmt address across structuring. More
  /// than one store occurrence at that address makes the mapping ambiguous.
  HighSourceKind Kind = HighSourceKind::Expression;
  /// The original lowered statement kind. A changed or synthetic statement
  /// must not inherit an instruction anchor merely by sharing an address.
  std::optional<StmtKind> StatementKind;
};

using HighSourceMap = std::vector<HighSourceObservation>;

} // namespace neverd

#endif
