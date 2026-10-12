//===- FreshChoiceProjection.h - Reuse fresh-choice projections -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Memoizes zero substitution for newly created leaves in one immutable DAG.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_FRESHCHOICEPROJECTION_H
#define NEVERD_ANALYSIS_FRESHCHOICEPROJECTION_H

#include "neverd/symbolic/SymExpr.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>

namespace neverd::analysis::detail {

/// A context-local zero projection, not a replacement for either execution's
/// fresh values. The context must outlive this owner and cannot be moved or
/// replaced. SymRef retains its usual same-context programming precondition.
///
/// New keys must have been created after the preceding projection completed.
/// An immutable older node cannot contain such a leaf, so completed results
/// remain valid when a key is added. Arbitrary map edits are not supported.
///
/// Retain at most MaxCachedNodes results, growing geometrically. Growth keeps
/// the old allocation until the replacement is filled. Roots outside that
/// prefix use ordinary substitution. Retention grants no proof or extra node
/// budget; callers still charge their existing limits after every projection.
class FreshChoiceProjection {
public:
  struct Statistics {
    uint64_t Queries = 0;
    uint64_t WorkItems = 0;
    uint64_t OperandScans = 0;
    uint64_t Rebuilds = 0;
    uint64_t CacheHits = 0;
    uint64_t Fallbacks = 0;
  };

  FreshChoiceProjection(symbolic::SymContext &Ctx, uint64_t MaxCachedNodes)
      : Context(Ctx), Maximum(std::min(MaxCachedNodes, uint64_t(UINT32_MAX))) {}
  FreshChoiceProjection(const FreshChoiceProjection &) = delete;
  FreshChoiceProjection &operator=(const FreshChoiceProjection &) = delete;
  FreshChoiceProjection(FreshChoiceProjection &&) = delete;
  FreshChoiceProjection &operator=(FreshChoiceProjection &&) = delete;

  bool addFreshZero(symbolic::SymContext &Ctx, symbolic::SymRef Fresh) {
    if (!valid(Ctx, Fresh) || Fresh.index() < ProjectedPrefix ||
        Context.op(Fresh) != symbolic::SymOp::Var ||
        !Context.varInfo(Context.varId(Fresh)).Fresh ||
        Choices.count(Fresh.index()))
      return false;
    Choices.emplace(Fresh.index(), Context.mkZero(Context.width(Fresh)));
    return true;
  }

  symbolic::SymRef project(symbolic::SymContext &Ctx, symbolic::SymRef Root) {
    if (!valid(Ctx, Root))
      return {};
    ++Stats.Queries;
    if (Choices.empty())
      return finish(Root);
    if (Root.index() >= Maximum) {
      ++Stats.Fallbacks;
      return finish(Context.substitute(Root, Choices));
    }
    grow(uint64_t(Root.index()) + 1);
    if (Results[Root.index()]) {
      ++Stats.CacheHits;
      return finish(Results[Root.index()]);
    }

    llvm::SmallVector<std::pair<symbolic::SymRef, bool>, 32> Work{
        {Root, false}};
    while (!Work.empty()) {
      ++Stats.WorkItems;
      auto [Current, Expanded] = Work.pop_back_val();
      if (Results[Current.index()]) {
        ++Stats.CacheHits;
        continue;
      }
      if (!Expanded) {
        if (const auto I = Choices.find(Current.index()); I != Choices.end()) {
          Results[Current.index()] = I->second;
          continue;
        }
        Work.emplace_back(Current, true);
        for (auto Child : Context.operands(Current)) {
          ++Stats.OperandScans;
          if (Results[Child.index()])
            ++Stats.CacheHits;
          else
            Work.emplace_back(Child, false);
        }
        continue;
      }

      llvm::SmallVector<symbolic::SymRef, 8> Operands;
      bool Changed = false;
      for (auto Child : Context.operands(Current)) {
        ++Stats.OperandScans;
        const auto Image = Results[Child.index()];
        assert(Image && "substitution child was not completed");
        Changed |= Image != Child;
        Operands.push_back(Image);
      }
      if (Changed) {
        ++Stats.Rebuilds;
        // No reference into the context's operand pool survives rebuilding.
        Results[Current.index()] = Context.rebuild(Current, Operands);
      } else {
        Results[Current.index()] = Current;
      }
    }
    return finish(Results[Root.index()]);
  }

  uint64_t allocatedNodes() const { return Allocated; }
  const Statistics &statistics() const { return Stats; }

private:
  bool valid(const symbolic::SymContext &Ctx, symbolic::SymRef R) const {
    return &Ctx == &Context && R && R.index() < Context.numNodes();
  }

  symbolic::SymRef finish(symbolic::SymRef Result) {
    // Include nodes created by rebuilding, even on the uncached fallback.
    ProjectedPrefix = Context.numNodes();
    return Result;
  }

  void grow(uint64_t Needed) {
    if (Needed <= Allocated)
      return;
    const uint64_t Next = std::max(Needed, std::min(Maximum, 2 * Allocated));
    auto NewResults = std::make_unique<symbolic::SymRef[]>(Next);
    if (Allocated)
      std::copy_n(Results.get(), Allocated, NewResults.get());
    Results = std::move(NewResults);
    Allocated = Next;
  }

  symbolic::SymContext &Context;
  uint64_t Maximum;
  uint64_t ProjectedPrefix = 0;
  uint64_t Allocated = 0;
  std::unique_ptr<symbolic::SymRef[]> Results;
  std::unordered_map<uint32_t, symbolic::SymRef> Choices;
  Statistics Stats;
};

} // namespace neverd::analysis::detail

#endif // NEVERD_ANALYSIS_FRESHCHOICEPROJECTION_H
