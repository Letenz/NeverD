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

/// Nonwrapping frame bounds must already hold. Compare first and last bytes
/// so adjacency remains valid, including a frame whose last byte is UINT64_MAX.
inline symbolic::SymRef excludedFrameRangePredicate(symbolic::SymContext &Ctx,
                                                    symbolic::SymRef Root,
                                                    int64_t Begin, int64_t End,
                                                    uint64_t ExcludedBegin,
                                                    uint64_t ExcludedEnd) {
  assert(Root && Ctx.width(Root) == 64 && Begin < End &&
         ExcludedBegin < ExcludedEnd);
  const auto First =
      Ctx.mkAdd(Root, Ctx.mkConst(64, static_cast<uint64_t>(Begin)));
  const auto Last =
      Ctx.mkAdd(Root, Ctx.mkConst(64, static_cast<uint64_t>(End) - 1));
  return Ctx.mkOr(Ctx.mkUlt(Last, Ctx.mkConst(64, ExcludedBegin)),
                  Ctx.mkUle(Ctx.mkConst(64, ExcludedEnd), First));
}

/// Both complete relative ranges must already be nonwrapping. Distinct root
/// identities do not imply different values or disjoint memory. Compare their
/// first/last bytes so adjacency and a final byte at UINT64_MAX remain valid.
inline symbolic::SymRef disjointFrameRangesPredicate(symbolic::SymContext &Ctx,
                                                     symbolic::SymRef Root,
                                                     int64_t Begin, int64_t End,
                                                     symbolic::SymRef OtherRoot,
                                                     int64_t OtherBegin,
                                                     int64_t OtherEnd) {
  assert(Root && OtherRoot && Ctx.width(Root) == 64 &&
         Ctx.width(OtherRoot) == 64 && Begin < End && OtherBegin < OtherEnd);
  const auto First =
      Ctx.mkAdd(Root, Ctx.mkConst(64, static_cast<uint64_t>(Begin)));
  const auto Last =
      Ctx.mkAdd(Root, Ctx.mkConst(64, static_cast<uint64_t>(End) - 1));
  const auto OtherFirst =
      Ctx.mkAdd(OtherRoot, Ctx.mkConst(64, static_cast<uint64_t>(OtherBegin)));
  const auto OtherLast = Ctx.mkAdd(
      OtherRoot, Ctx.mkConst(64, static_cast<uint64_t>(OtherEnd) - 1));
  return Ctx.mkOr(Ctx.mkUlt(Last, OtherFirst), Ctx.mkUlt(OtherLast, First));
}

} // namespace neverd::analysis::detail

#endif
