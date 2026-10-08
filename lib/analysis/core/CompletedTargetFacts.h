//===- CompletedTargetFacts.h - Facts from complete target enumeration
//===========//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_COMPLETEDTARGETFACTS_H
#define NEVERD_ANALYSIS_COMPLETEDTARGETFACTS_H
#include "FiniteValues.h"

#include <memory>
namespace neverd::analysis::detail {
// Context-local singleton facts can be entered only by a complete enumeration.
// A partial tuple list, candidate model or caller-constructed result is not
// evidence.
class CompletedTargetFacts {
  class Impl;
  std::unique_ptr<Impl> State;

public:
  enum class Answer { Proved, Unavailable, BudgetExceeded };
  CompletedTargetFacts(symbolic::SymContext &Ctx, uint64_t MaxWords);
  ~CompletedTargetFacts();
  CompletedTargetFacts(const CompletedTargetFacts &) = delete;
  CompletedTargetFacts &operator=(const CompletedTargetFacts &) = delete;

  // Preserve the original complete enumeration and its limits. Only encoding
  // gate exhaustion can try a completed additive fact, and that reduced target
  // must again be completely enumerated under the entire original predicate.
  FiniteValues enumerate(FiniteDomainEncoding &Encoding,
                         symbolic::SymRef Predicate, symbolic::SymRef Target,
                         uint32_t Limit, uint64_t MaxQueries,
                         uint64_t MaxSymbolicNodes, uint64_t &Queries);
  Answer proves(const symbolic::SymContext &Ctx, symbolic::SymRef Domain,
                symbolic::SymRef Equality, uint64_t &Remaining) const;
  uint64_t size() const;
  uint64_t allocatedWords() const;
};
} // namespace neverd::analysis::detail
#endif
