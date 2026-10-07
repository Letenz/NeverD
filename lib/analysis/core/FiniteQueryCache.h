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
#include <memory>
#include <utility>

namespace neverd::analysis::detail {

/// Run-local reuse of completed finite-domain proofs. By default keys describe
/// the exact ordered query DAG modulo a consistent renaming of free variables;
/// names and machine-input provenance are not solver semantics. A bound cache
/// instead uses exact references within one immutable context. Neither mode
/// retains models, incomplete proofs, or solver resource failures.
///
/// MaxWords bounds both one key construction and the total retained keys and
/// results, including structural and recency overhead. A valid result that
/// fits alone may evict least recently used proofs. Rejected keys or results
/// neither evict nor refresh existing entries.
class FiniteQueryCache {
public:
  explicit FiniteQueryCache(uint64_t MaxWords) : MaxWords(MaxWords) {}
  /// Bind compact query identities to Ctx. Existing nodes must remain
  /// immutable; Ctx must outlive this cache and must not be moved or replaced.
  /// Appending nodes is allowed. Keys and prepared tokens never cross this
  /// cache owner.
  FiniteQueryCache(uint64_t MaxWords, const symbolic::SymContext &Ctx)
      : MaxWords(MaxWords), BoundContext(&Ctx),
        Scope(std::make_shared<const int>(0)) {}

  // Recency nodes refer to this cache's stable map keys and vice versa.
  FiniteQueryCache(const FiniteQueryCache &) = delete;
  FiniteQueryCache &operator=(const FiniteQueryCache &) = delete;
  FiniteQueryCache(FiniteQueryCache &&) = delete;
  FiniteQueryCache &operator=(FiniteQueryCache &&) = delete;

  /// A complete query key. Structural tokens own their serialization and are
  /// independent of their source context and cache. Bound tokens are valid only
  /// for their original cache owner and do not extend the context's lifetime.
  /// Each token owns at most MaxWords key words plus projection widths; keeping
  /// it alive during a proof adds temporary storage beside retained cache
  /// entries. Default and moved-from tokens are invalid and always miss.
  class PreparedQuery {
  public:
    PreparedQuery() = default;
    PreparedQuery(PreparedQuery &&Other) noexcept
        : Words(std::move(Other.Words)), Widths(std::move(Other.Widths)),
          Limit(std::exchange(Other.Limit, 0)), Scope(std::move(Other.Scope)) {}
    PreparedQuery &operator=(PreparedQuery &&Other) noexcept {
      if (this != &Other) {
        Words = std::move(Other.Words);
        Widths = std::move(Other.Widths);
        Limit = std::exchange(Other.Limit, 0);
        Scope = std::move(Other.Scope);
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
    std::shared_ptr<const int> Scope;
  };

  /// Prepare once for lookup followed by a possible store of the same query.
  /// Structural keys validate the full DAG; bound keys validate root references
  /// and widths in their fixed context. Preparation does not prove a query.
  /// Invalid roots, a foreign bound context or excess storage yield no token.
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
  const symbolic::SymContext *BoundContext = nullptr;
  // A separately owned identity prevents an old token from aliasing a new
  // owner at the same address. Unbound structural tokens have no scope.
  std::shared_ptr<const int> Scope;
  uint64_t StoredWords = 0;
  std::map<Key, Entry> Entries;
  mutable RecencyList Recency;
};

} // namespace neverd::analysis::detail

#endif // NEVERD_ANALYSIS_INTERPRETER_FINITEQUERYCACHE_H
