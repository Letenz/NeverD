//===- LLVMScalarSourceRecovery.h - Proved scalar cleanup -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_ANALYSIS_LLVMSCALARSOURCERECOVERY_H
#define NEVERD_ANALYSIS_LLVMSCALARSOURCERECOVERY_H

#include "neverd/analysis/LLVMScalarLoopRecovery.h"

namespace neverd::analysis {

struct LLVMScalarSourceRecoveryLimits {
  LLVMScalarLoopRecoveryLimits Search;
  /// Preparation/final queries retain Search.Proof's semantic limits, but
  /// have this work ceiling. All queries also debit Search.MaxProofWork.
  uint64_t MaxBoundaryProofWork = 67108864;
  unsigned MaxCleanupRounds = 8;
};

struct LLVMScalarSourceRecoveryResult : LLVMScalarLoopRecoveryResult {
  unsigned CleanupRounds = 0;
};

/// Prepare scalar SSA with LLVM/predicate cleanup before proposing recovered
/// loops. Prove preparation and the exact final body against the complete
/// original, including dead operations, poison, assumes and input contracts.
/// Every input stays symbolic outside the proved source control partitions.
/// Cleanup is a proposal, never an assumption of compiler correctness. It
/// includes LLVM common-expression hoisting and charged removal of instruction
/// poison flags in the private clone. Original annotations remain mandatory
/// proof obligations; argument/call contracts and assumes are not relaxed.
///
/// Construction, cleanup rounds, candidate, transformation and proof budgets
/// are cumulative. LLVM passes/cloning/verifiers are trusted bulk operations;
/// the work limits are not hard CPU/stack bounds. InstCombine proposals have
/// an explicit iteration ceiling and need not reach a fixpoint. Unsupported
/// or exhausted work returns no module. Source and parent remain unchanged.
/// Recovered also includes cleanup without a successful loop transformation;
/// the result retains the function's signature and establishes no native ABI.
LLVMScalarSourceRecoveryResult
recoverLLVMScalarSource(const llvm::Function &Function,
                        const LLVMScalarSourceRecoveryLimits &Limits = {});

} // namespace neverd::analysis
#endif
