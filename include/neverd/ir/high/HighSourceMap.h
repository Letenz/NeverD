//===- HighSourceMap.h - Non-owning source expression observations --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_HIGHSOURCEMAP_H
#define NEVERD_IR_HIGH_HIGHSOURCEMAP_H

#include "neverd/sigs/LibraryRecognition.h"

#include <memory>
#include <vector>

namespace neverd {

struct HighExpr;

/// An expression produced from one concrete MedIR occurrence. Weak ownership
/// deliberately loses a mapping when a transformation replaces the expression.
/// Surviving expressions are checked again at emission; a pointer by itself
/// does not establish a source span or permission to fold a region.
struct HighSourceObservation {
  va_t Function = 0;
  sigs::LibraryOccurrence Occurrence;
  std::weak_ptr<const HighExpr> Expression;
};

using HighSourceMap = std::vector<HighSourceObservation>;

} // namespace neverd

#endif
