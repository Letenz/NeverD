//===- ExceptionEdge.h - IR-level exceptional edges ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The exceptional control-flow transfer NeverD's IR keeps separate from an
/// ordinary CFG edge, and the taxonomy of what each one is for.  These are the
/// only exception records a CFG consumer has to understand; everything else in
/// this family describes the tables they were recovered from.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_EXCEPTIONEDGE_H
#define NEVERD_LOADER_EXCEPTIONEDGE_H

#include "neverd/Common.h"

#include <algorithm>
#include <array>
#include <compare>
#include <cstdint>
#include <memory>
#include <span>
#include <tuple>
#include <vector>

namespace neverd {

enum class ExceptionalEdgeKind : uint8_t {
  SEHFilter,
  SEHHandler,
  SEHFinally,
  CxxCleanup,
  CxxCatch,
  /// Itanium landing pad that only runs destructors and resumes unwinding.
  ItaniumCleanupPad,
  /// Itanium landing pad that may stop the exception for a typed catch.
  ItaniumCatchPad,
  /// Itanium landing pad reached because an exception specification was
  /// violated, which calls `std::unexpected`/`std::terminate`.
  ItaniumSpecPad,
  /// Go `deferreturn` re-entry: the runtime resumes the frame here after
  /// running its deferred calls during a panic.
  GoDeferReturn,
  /// Go `recover` continuation.
  GoRecover,
  /// Delphi `try..finally` cleanup body.
  DelphiFinally,
  /// Delphi `try..except` body that catches anything, which is also what a
  /// `safecall` wrapper's automatic handler reaches.
  DelphiExcept,
  /// One `except on <class> do` arm.
  DelphiOnException,
  Unknown,
};

inline const char *getExceptionalEdgeKindName(ExceptionalEdgeKind Kind) {
  switch (Kind) {
  case ExceptionalEdgeKind::SEHFilter:
    return "seh-filter";
  case ExceptionalEdgeKind::SEHHandler:
    return "seh-handler";
  case ExceptionalEdgeKind::SEHFinally:
    return "seh-finally";
  case ExceptionalEdgeKind::CxxCleanup:
    return "cxx-cleanup";
  case ExceptionalEdgeKind::CxxCatch:
    return "cxx-catch";
  case ExceptionalEdgeKind::ItaniumCleanupPad:
    return "itanium-cleanup";
  case ExceptionalEdgeKind::ItaniumCatchPad:
    return "itanium-catch";
  case ExceptionalEdgeKind::ItaniumSpecPad:
    return "itanium-spec";
  case ExceptionalEdgeKind::GoDeferReturn:
    return "go-deferreturn";
  case ExceptionalEdgeKind::GoRecover:
    return "go-recover";
  case ExceptionalEdgeKind::DelphiFinally:
    return "delphi-finally";
  case ExceptionalEdgeKind::DelphiExcept:
    return "delphi-except";
  case ExceptionalEdgeKind::DelphiOnException:
    return "delphi-on";
  case ExceptionalEdgeKind::Unknown:
    return "unknown";
  }
  return "unknown";
}

/// The table region and originating state that permit one exceptional transfer.
/// These values are correlated: separate sets of regions and states would also
/// admit pairs that the original state graph never contained.
struct ExceptionalEdgeContext {
  uint32_t RegionIndex = 0;
  int32_t State = -1;
  auto operator<=>(const ExceptionalEdgeContext &) const = default;
};

/// An owning, immutable set of exact contexts shared by edges with the same
/// destination. Its digest binds every pair once for snapshot consumers; it is
/// not an admission proof. Construction rejects an empty set and canonicalizes
/// order and duplicates. Producers must budget the construction work.
class ExceptionalEdgeContextDomain {
public:
  explicit ExceptionalEdgeContextDomain(
      std::vector<ExceptionalEdgeContext> Contexts);
  ExceptionalEdgeContextDomain(const ExceptionalEdgeContextDomain &) = default;
  ExceptionalEdgeContextDomain &
  operator=(const ExceptionalEdgeContextDomain &) = delete;
  std::span<const ExceptionalEdgeContext> values() const { return Values; }
  const std::array<uint8_t, 32> &digest() const { return Digest; }

private:
  std::vector<ExceptionalEdgeContext> Values;
  std::array<uint8_t, 32> Digest;
};

/// IR-level exceptional transfer kept separate from ordinary CFG edges.  In a
/// successor list BlockId is the target block; in a predecessor list it is the
/// source block.  A value of -1 denotes a valid target outside the current
/// function/funclet while TargetVA retains the exact destination.
struct ExceptionalEdge {
  int BlockId = -1;
  va_t TargetVA = 0;
  ExceptionalEdgeKind Kind = ExceptionalEdgeKind::Unknown;
  /// Inline storage for a single context. Read contexts() instead: a shared
  /// domain, when present, is the complete authority for this edge.
  ExceptionalEdgeContext ExplicitContext;
  /// Exact throwing instruction for synchronous edges. InvalidVA denotes a
  /// protected range, whose register state cannot be sampled at one call.
  va_t SourceVA = InvalidVA;
  std::shared_ptr<const ExceptionalEdgeContextDomain> SharedContexts;

  ExceptionalEdge() = default;
  ExceptionalEdge(int Block, va_t Target,
                  ExceptionalEdgeKind EdgeKind = ExceptionalEdgeKind::Unknown,
                  uint32_t Region = 0, int32_t State = -1,
                  va_t Source = InvalidVA)
      : BlockId(Block), TargetVA(Target), Kind(EdgeKind),
        ExplicitContext{Region, State}, SourceVA(Source) {}

  std::span<const ExceptionalEdgeContext> contexts() const {
    return SharedContexts ? SharedContexts->values()
                          : std::span(&ExplicitContext, 1);
  }

  /// Exact singleton consumers must refuse a domain with several contexts.
  const ExceptionalEdgeContext *singleContext() const {
    const auto Values = contexts();
    return Values.size() == 1 ? Values.data() : nullptr;
  }

  bool operator==(const ExceptionalEdge &Other) const {
    return BlockId == Other.BlockId && TargetVA == Other.TargetVA &&
           Kind == Other.Kind && SourceVA == Other.SourceVA &&
           ((SharedContexts && SharedContexts == Other.SharedContexts) ||
            std::ranges::equal(contexts(), Other.contexts()));
  }

  bool operator<(const ExceptionalEdge &Other) const {
    const auto Key = std::tie(BlockId, TargetVA, Kind, SourceVA);
    const auto OtherKey =
        std::tie(Other.BlockId, Other.TargetVA, Other.Kind, Other.SourceVA);
    if (Key != OtherKey)
      return Key < OtherKey;
    if (SharedContexts && SharedContexts == Other.SharedContexts)
      return false;
    return std::ranges::lexicographical_compare(contexts(), Other.contexts());
  }
};

} // namespace neverd

#endif // NEVERD_LOADER_EXCEPTIONEDGE_H
