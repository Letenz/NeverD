//===- DomainCoverage.h - Complete terminal coverage proofs ===========//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_DOMAINCOVERAGE_H
#define NEVERD_ANALYSIS_DOMAINCOVERAGE_H
#include "neverd/solver/SatTypes.h"
#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/STLFunctionalExtras.h"
namespace neverd::analysis::detail {
struct DomainCoverageProof {
  bool Proved = false;
  uint64_t Guards = 0, FreshQueries = 0, Removed = 0, Words = 0;
};

struct PartitionedCoverageResult {
  bool Proved = false;
  uint64_t Words = 0, Splits = 0, Leaves = 0, Obligations = 0;
  unsigned MaximumDepth = 0;
};

// Check solves the complete supplied predicate. Prove solves the complete
// Domain AND NOT Goal. Both callbacks charge the caller's cumulative query
// allowance, keep all solver settings, and expose the actual SatResult.
// ValidateNodes enforces the cumulative allocation ceiling, including after
// every callback. One failed obligation leaves the entire result unproved.
DomainCoverageProof proveCoverageFromDomainFacts(
    symbolic::SymContext &Ctx, symbolic::SymRef Predicate,
    symbolic::SymRef Domain, symbolic::SymRef Coverage, uint64_t MaxWork,
    llvm::function_ref<solver::SatResult(symbolic::SymRef)> Check,
    llvm::function_ref<void()> ValidateNodes);
PartitionedCoverageResult provePartitionedCoverage(
    symbolic::SymContext &Ctx, symbolic::SymRef Domain,
    symbolic::SymRef Coverage, uint64_t MaxWork,
    llvm::function_ref<solver::SatResult(symbolic::SymRef, symbolic::SymRef)>
        Prove,
    llvm::function_ref<void()> ValidateNodes);
} // namespace neverd::analysis::detail
#endif
