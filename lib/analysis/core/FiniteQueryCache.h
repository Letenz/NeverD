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
#include <utility>

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

  /// A complete serialized query, independent of its source context and cache.
  /// Each token owns at most MaxWords key words plus projection widths; keeping
  /// it alive during a proof adds temporary storage beside retained cache
  /// entries. Default and moved-from tokens are invalid and always miss.
  class PreparedQuery {
  public:
    PreparedQuery() = default;
    PreparedQuery(PreparedQuery &&Other) noexcept
        : Words(std::move(Other.Words)), Widths(std::move(Other.Widths)),
          Limit(std::exchange(Other.Limit, 0)) {}
    PreparedQuery &operator=(PreparedQuery &&Other) noexcept {
      if (this != &Other) {
        Words = std::move(Other.Words);
        Widths = std::move(Other.Widths);
        Limit = std::exchange(Other.Limit, 0);
      }
      return *this;
    }
    PreparedQuery(const PreparedQuery &) = delete;
    PreparedQuery &operator=(const PreparedQuery &) = delete;

  private:
    friend class FiniteQueryCache;
    std::vector<uint64_t> Words;
    std::vector<uint32_t> Widths;
    uint32_t Limit = 0;
  };

  /// Serialize once for a lookup followed by a possible store of the same
  /// query. Unsupported, malformed or oversized queries return an invalid
  /// token.
  PreparedQuery prepare(const symbolic::SymContext &Ctx,
                        symbolic::SymRef Predicate,
                        llvm::ArrayRef<symbolic::SymRef> Values,
                        uint32_t Limit) const;
  std::optional<FiniteValues> lookup(const PreparedQuery &Query) const;
  /// Consume Query; Result must prove exactly that query. Only Complete and
  /// TooManyValues can enter the cache, with the usual result and storage
  /// checks.
  void store(PreparedQuery Query, const FiniteValues &Result);

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
