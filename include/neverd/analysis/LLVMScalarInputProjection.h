//===- LLVMScalarInputProjection.h - Explicit scalar inputs ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_ANALYSIS_LLVMSCALARINPUTPROJECTION_H
#define NEVERD_ANALYSIS_LLVMSCALARINPUTPROJECTION_H

#include "neverd/analysis/LLVMScalarResultProjection.h"

namespace neverd::analysis {

using LLVMScalarInputProjectionLimits = LLVMScalarResultProjectionLimits;

struct LLVMScalarInputProjectionResult {
  enum Kind { Projected, Unsupported, BudgetExceeded } Status = Unsupported;
  std::string Diagnostic;
  /// Only Projected publishes one definition and its intrinsic declarations.
  /// The module borrows the original LLVMContext.
  std::unique_ptr<llvm::Module> Module;
  /// Increasing original argument indices, one per projected parameter.
  /// Re-embed a candidate with this mapping before proving it against the
  /// complete original signature. Omitted inputs are never fixed to constants.
  std::vector<unsigned> Arguments;
  uint64_t ConstructionWork = 0;
  uint64_t RetainedInstructions = 0;
  unsigned RemovedArguments = 0;
};

/// Clone a scalar integer function and omit only unused SSA parameters.
/// Reuse result projection's bounded clone and scalar-model admission; refuse
/// packaging removal and debug contracts. Every block, non-return instruction,
/// annotation, assume and used parameter remains, including dead arithmetic
/// that may carry definedness obligations. No optimization or proof is run.
///
/// The mapping describes an explicit scalar interface, not a native ABI or a
/// definedness/termination certificate. Establish the complete original source
/// proof before composing this with source preparation, and prove recompiled
/// candidates through the original signature and its entry contract.
///
/// Input/result projection share one construction budget; model limits remain
/// separate. LLVM verification, cloning and body movement are trusted bulk
/// operations, not hard CPU/stack limits. Refusal publishes no module or
/// mapping and never changes the original function or its parent.
LLVMScalarInputProjectionResult
projectLLVMScalarInputs(const llvm::Function &Function,
                        const LLVMScalarInputProjectionLimits &Limits = {});

} // namespace neverd::analysis
#endif
