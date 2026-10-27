//===- FrameOffsets.h - Proved entry-relative addresses ----------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_INTERPRETER_FRAMEOFFSETS_H
#define NEVERD_ANALYSIS_INTERPRETER_FRAMEOFFSETS_H

#include "FiniteQueryCache.h"

namespace neverd::symbolic {
class MBARewriteCache;
} // namespace neverd::symbolic

namespace neverd::analysis::detail {

enum class FrameOffsetStatus {
  Exact,
  NonUnique,
  Infeasible,
  Invalid,
  BudgetExceeded,
};

struct FrameOffset {
  FrameOffsetStatus Status = FrameOffsetStatus::Invalid;
  /// Modular 64-bit displacement; meaningful only for Exact.
  uint64_t Offset = 0;
};

/// Optional per-call strategy accounting, reset on entry. Charges uncached
/// readability-prefix admission (nodes, operand edges and constant limbs) plus
/// MBA measurement units; not every internal traversal, sample or CPU
/// operation. A changed exact expression may retry the ordinary proof once,
/// retaining cumulative solver limits. Only completed finite-domain evidence
/// from that retry can populate the original query's cache; algebraic offset
/// theorems alone cannot.
struct FrameOffsetNormalization {
  uint64_t MaxWork = 0;
  uint64_t WorkUsed = 0;
  /// Optional exact derivations owned by this proof's symbolic context. The
  /// caller keeps the context and cache alive across calls; no predicate or
  /// finite-domain evidence is retained here. Hits still consume MBA work.
  symbolic::MBARewriteCache *Rewrites = nullptr;
};

/// Prove Value == Root + Offset under Predicate, preserving Root's identity.
/// Exact does not establish reachability: predicate-independent identities
/// need no satisfiability query. Infeasible is an explicit empty-domain proof.
/// No frame bounds, accessibility, provenance or nonalias fact is implied.
/// Callers must canonicalize their actual symbolic memory accesses as well as
/// retained pointer facts. No input, state, or entry domain is changed here.
/// All solver checks debit Queries, including a final uniqueness check; a
/// partial model or a failed resource limit never supplies an offset.
FrameOffset proveFrameOffset(symbolic::SymContext &Ctx,
                             symbolic::SymRef Predicate, symbolic::SymRef Value,
                             symbolic::SymRef Root,
                             solver::SolverOptions Settings,
                             uint64_t MaxQueries, uint64_t MaxSymbolicNodes,
                             uint64_t &Queries,
                             FiniteQueryCache *Cache = nullptr,
                             FrameOffsetNormalization *Normalization = nullptr);

FrameOffset proveFrameOffset(symbolic::SymContext &Ctx,
                             symbolic::SymRef Predicate, symbolic::SymRef Value,
                             symbolic::SymRef Root,
                             const SpecializationOptions &Options,
                             uint64_t &Queries,
                             FiniteQueryCache *Cache = nullptr,
                             FrameOffsetNormalization *Normalization = nullptr);

/// Uses the encoding owner's fixed context and settings. Every cold finite
/// proof still runs independent search and charges all of its query requests.
FrameOffset proveFrameOffset(FiniteDomainEncoding &Encoding,
                             symbolic::SymRef Predicate, symbolic::SymRef Value,
                             symbolic::SymRef Root, uint64_t MaxQueries,
                             uint64_t MaxSymbolicNodes, uint64_t &Queries,
                             FiniteQueryCache *Cache = nullptr,
                             FrameOffsetNormalization *Normalization = nullptr);

} // namespace neverd::analysis::detail

#endif // NEVERD_ANALYSIS_INTERPRETER_FRAMEOFFSETS_H
