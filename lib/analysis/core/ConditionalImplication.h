//===- ConditionalImplication.h - Bounded whole-domain implication ===========//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_CONDITIONALIMPLICATION_H
#define NEVERD_ANALYSIS_CONDITIONALIMPLICATION_H
#include "neverd/solver/BitVectorSolver.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <memory>
#include <vector>
namespace neverd::analysis::conditional_implication {
using solver::SatResult;
struct Limits {
  uint64_t MaxNodes = 4194304;
  // Maximum inspection work in each preparation, rewrite or encoding phase.
  // A partial closure is usable only with all reconstruction definitions.
  uint64_t MaxWork = 4194304;
  uint32_t MaxReachableNodes = 262144;
  uint32_t MaxWidth = 256;
  uint32_t MaxQueries = 16384;
  bool operator==(const Limits &) const = default;
};

struct Batch {
  unsigned Begin, Count;
  SatResult Answer;
  uint64_t Gates;
};
struct Result {
  bool Proved = false, ReusedPreparation = false;
  uint64_t NormalizationWork = 0, CollectionWork = 0, ClosureWork = 0,
           RebuildWork = 0;
  uint64_t Queries = 0, ProvedTerms = 0, TotalTerms = 0, DomainGates = 0;
  std::vector<Batch> Batches;
};

// Only the implementation can create prepared conditional identities. A cache
// retains pristine encoding, never a model or a result for an earlier goal.
// A fresh complete proof is required for every goal, including cache reuse.
// The append-only source context must outlive this cache.
class Cache {
  class Impl;
  std::unique_ptr<Impl> State;

public:
  Cache();
  ~Cache();
  Cache(const Cache &) = delete;
  Cache &operator=(const Cache &) = delete;
  Result proveCached(symbolic::SymContext &Ctx, symbolic::SymRef Domain,
                     symbolic::SymRef Goal,
                     const solver::SolverOptions &Options, const Limits &Bound,
                     llvm::function_ref<bool()> ChargeQuery);
  Result proveFresh(symbolic::SymContext &Ctx, symbolic::SymRef Domain,
                    symbolic::SymRef Goal, const solver::SolverOptions &Options,
                    const Limits &Bound, uint64_t RemainingQueries,
                    llvm::function_ref<bool()> ChargeQuery);
};
} // namespace neverd::analysis::conditional_implication
#endif
