//===- LLVMScalarLoopRecovery.h - Loop recovery -----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_ANALYSIS_LLVMSCALARLOOPRECOVERY_H
#define NEVERD_ANALYSIS_LLVMSCALARLOOPRECOVERY_H

#include "neverd/analysis/LLVMScalarEquivalence.h"

#include "llvm/IR/Module.h"

namespace neverd::analysis {

struct LLVMScalarLoopRecoveryLimits {
  LLVMScalarEquivalenceLimits Proof;
  uint64_t MaxConstructionWork = 1048576;
  uint64_t MaxProofWork = 134217728;
  unsigned MaxCandidates = 128;
  unsigned MaxTransforms = 16;
};

enum class LLVMScalarLoopRecoveryStatus {
  Recovered,
  Unchanged,
  Unsupported,
  BudgetExceeded,
};

struct LLVMScalarLoopRecoveryResult {
  LLVMScalarLoopRecoveryStatus Status = LLVMScalarLoopRecoveryStatus::Unchanged;
  std::string Diagnostic;
  /// Non-null only for Recovered. Owns one definition with the original name
  /// and needed intrinsic declarations. Borrows the original LLVMContext.
  std::unique_ptr<llvm::Module> Module;
  uint64_t ConstructionWork = 0;
  uint64_t ProofWork = 0;
  unsigned Candidates = 0;
  unsigned ProvedTransforms = 0;
};

/// Reconstruct peeled scalar loops by proposing changes to SSA carriers,
/// zero-trip regions, loop tests and internal integer widths. Search preserves
/// the complete proved source control domain. A zero-data screen may only
/// reject proposals; it never replaces the required proof with all other
/// input bits symbolic.
/// Boolean header carriers may propose comparisons with an integer carrier
/// when every external entry matches its seed and dominating invariant bound.
/// Entry agreement is discovery only; all actual backedges still need proof.
/// Every accepted change must pass complete scalar equivalence against the
/// original function, using the shared model and SymExec. No original-source
/// template, native address or external solver participates. Unsupported or
/// exhausted searches publish no partial result. Neither the source function
/// nor its module is changed. This opt-in C++ operation does not infer memory
/// privacy, a native ABI or compiler validity.
LLVMScalarLoopRecoveryResult
recoverLLVMScalarLoops(const llvm::Function &Function,
                       const LLVMScalarLoopRecoveryLimits &Limits = {});

} // namespace neverd::analysis
#endif
