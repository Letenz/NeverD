//===- FiniteQueryCache.h - Bounded finite-proof reuse ------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_INTERPRETER_FINITEQUERYCACHE_H
#define NEVERD_ANALYSIS_INTERPRETER_FINITEQUERYCACHE_H

#include "FiniteValues.h"

#include <list>
#include <map>

namespace neverd::analysis::detail {

/// Run-local reuse of completed finite-domain proofs. Keys describe the exact
/// ordered query DAG modulo a consistent renaming of its free variables.
/// Names and machine-input provenance are not solver semantics. No context,
/// symbolic reference, partial model, or solver resource failure is retained.
///
/// MaxWords bounds both one key construction and the total retained keys and
/// results, including structural and recency overhead. A valid result that
/// fits alone may evict least recently used proofs. Unsupported, malformed,
/// or oversized candidates neither evict nor refresh existing entries.
class FiniteQueryCache {
public:
  explicit FiniteQueryCache(uint64_t MaxWords) : MaxWords(MaxWords) {}

  // Recency nodes refer to this cache's stable map keys and vice versa.
  FiniteQueryCache(const FiniteQueryCache &) = delete;
  FiniteQueryCache &operator=(const FiniteQueryCache &) = delete;
  FiniteQueryCache(FiniteQueryCache &&) = delete;
  FiniteQueryCache &operator=(FiniteQueryCache &&) = delete;

  std::optional<FiniteValues> lookup(const symbolic::SymContext &Ctx,
                                     symbolic::SymRef Predicate,
                                     llvm::ArrayRef<symbolic::SymRef> Values,
                                     uint32_t Limit) const;

  /// Result must come from a proof of this exact query. Only Complete and
  /// TooManyValues are accepted; both are independent of solver budgets.
  void store(const symbolic::SymContext &Ctx, symbolic::SymRef Predicate,
             llvm::ArrayRef<symbolic::SymRef> Values, uint32_t Limit,
             const FiniteValues &Result);

private:
  using Key = std::vector<uint64_t>;
  using RecencyList = std::list<const Key *>;
  struct Entry {
    FiniteValues Result;
    uint64_t Words;
    RecencyList::iterator Recent;
  };
  uint64_t MaxWords;
  uint64_t StoredWords = 0;
  std::map<Key, Entry> Entries;
  mutable RecencyList Recency;
};

} // namespace neverd::analysis::detail

#endif // NEVERD_ANALYSIS_INTERPRETER_FINITEQUERYCACHE_H
