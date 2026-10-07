//===- CompletedQueryCache.h - Context-local SAT answers --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_COMPLETEDQUERYCACHE_H
#define NEVERD_ANALYSIS_COMPLETEDQUERYCACHE_H

#include "neverd/solver/SatTypes.h"
#include "neverd/symbolic/SymExpr.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace neverd::analysis::detail {

/// Completed model-free answers in one immutable, append-only context. The
/// context must outlive this owner and may not be moved or replaced. Callers
/// must prove the entire predicate with fixed solver options before storing.
/// This cache grants no assumptions and never holds a model or search state.
///
/// Two bits per node encode absent/SAT/UNSAT. Lazy geometric growth retains at
/// most ceil(MaxNodes / 32) words; a growth temporarily keeps the old
/// allocation beside the replacement. Lookup and store never traverse or mutate
/// the DAG. Logical query and node budgets remain the caller's responsibility
/// on hits.
class CompletedQueryCache {
public:
  CompletedQueryCache(const symbolic::SymContext &Ctx, uint64_t MaxNodes)
      : Context(Ctx), MaxNodes(std::min(MaxNodes, uint64_t(UINT32_MAX) + 1)) {}
  CompletedQueryCache(const CompletedQueryCache &) = delete;
  CompletedQueryCache &operator=(const CompletedQueryCache &) = delete;
  CompletedQueryCache(CompletedQueryCache &&) = delete;
  CompletedQueryCache &operator=(CompletedQueryCache &&) = delete;

  std::optional<solver::SatResult> lookup(const symbolic::SymContext &Ctx,
                                          symbolic::SymRef Predicate) const {
    if (!valid(Ctx, Predicate) || Predicate.index() / 32 >= Words)
      return std::nullopt;
    const unsigned Value =
        (Answers[Predicate.index() / 32] >> (2 * (Predicate.index() % 32))) & 3;
    if (Value == 1)
      return solver::SatResult::Sat;
    if (Value == 2)
      return solver::SatResult::Unsat;
    return std::nullopt;
  }

  bool store(const symbolic::SymContext &Ctx, symbolic::SymRef Predicate,
             solver::SatResult Answer) {
    if (!valid(Ctx, Predicate) || (Answer != solver::SatResult::Sat &&
                                   Answer != solver::SatResult::Unsat))
      return false;
    const uint64_t Index = Predicate.index() / 32;
    if (Index >= Words) {
      const uint64_t Maximum = MaxNodes / 32 + (MaxNodes % 32 != 0);
      const uint64_t Next = std::max(Index + 1, std::min(Maximum, 2 * Words));
      auto NewAnswers = std::make_unique<uint64_t[]>(Next);
      if (Words)
        std::copy_n(Answers.get(), Words, NewAnswers.get());
      Answers = std::move(NewAnswers);
      Words = Next;
    }
    const unsigned Shift = 2 * (Predicate.index() % 32);
    const uint64_t Value = Answer == solver::SatResult::Sat ? 1 : 2;
    const uint64_t Old = (Answers[Index] >> Shift) & 3;
    if (Old && Old != Value)
      return false;
    Answers[Index] |= Value << Shift;
    return true;
  }

  uint64_t allocatedWords() const { return Words; }

private:
  bool valid(const symbolic::SymContext &Ctx,
             symbolic::SymRef Predicate) const {
    return &Ctx == &Context && Predicate && Predicate.index() < MaxNodes &&
           Predicate.index() < Ctx.numNodes() && Ctx.width(Predicate) == 1;
  }

  const symbolic::SymContext &Context;
  uint64_t MaxNodes;
  uint64_t Words = 0;
  std::unique_ptr<uint64_t[]> Answers;
};

} // namespace neverd::analysis::detail

#endif // NEVERD_ANALYSIS_COMPLETEDQUERYCACHE_H
