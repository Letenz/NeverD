//===- FrameEntryConstraints.h - Explicit nonwrapping entry domain --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_INTERPRETER_FRAMEENTRYCONSTRAINTS_H
#define NEVERD_ANALYSIS_INTERPRETER_FRAMEENTRYCONSTRAINTS_H

#include "neverd/symbolic/SymExpr.h"

#include <cassert>

namespace neverd::analysis::detail {

/// Caller validates a 64-bit Root and Begin < End. Bound the first and last
/// bytes without signed negation or constructing a wrapping exclusive end.
/// This does not establish memory accessibility, provenance or disjointness.
inline symbolic::SymRef nonwrappingFramePredicate(symbolic::SymContext &Ctx,
                                                  symbolic::SymRef Root,
                                                  int64_t Begin, int64_t End) {
  assert(Root && Ctx.width(Root) == 64 && Begin < End);
  auto Predicate = Ctx.mkTrue();
  if (Begin < 0)
    Predicate = Ctx.mkAnd(
        Predicate,
        Ctx.mkUle(Ctx.mkConst(64, uint64_t{0} - static_cast<uint64_t>(Begin)),
                  Root));
  if (End > 0)
    Predicate = Ctx.mkAnd(
        Predicate,
        Ctx.mkUle(Root, Ctx.mkConst(64, UINT64_MAX -
                                            static_cast<uint64_t>(End - 1))));
  return Predicate;
}

} // namespace neverd::analysis::detail

#endif
