//===- LLVMScalarResultProjection.h - Scalar observations -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_ANALYSIS_LLVMSCALARRESULTPROJECTION_H
#define NEVERD_ANALYSIS_LLVMSCALARRESULTPROJECTION_H

#include "neverd/analysis/LLVMInterpreterModel.h"

#include "llvm/IR/Module.h"

#include <string>
#include <vector>

namespace neverd::analysis {

struct LLVMScalarResultObservation {
  /// Struct/array indices leading to an integer leaf; empty for an integer
  /// return. Bit numbers describe the integer value, independent of byte order.
  std::vector<unsigned> Indices;
  unsigned LowBit = 0;
  unsigned Bits = 32;
};

struct LLVMScalarResultProjectionLimits {
  uint64_t MaxConstructionWork = 1048576;
  LLVMInterpreterModelLimits Model;
};

struct LLVMScalarResultProjectionResult {
  enum Kind { Projected, Unsupported, BudgetExceeded } Status = Unsupported;
  std::string Diagnostic;
  /// Only Projected publishes a module. It borrows the input LLVMContext and
  /// contains one definition with the original name, plus needed intrinsics.
  std::unique_ptr<llvm::Module> Module;
  uint64_t ConstructionWork = 0;
  uint64_t RetainedInstructions = 0, RemovedPackaging = 0;
};

/// Clone one declared integer observation of a scalar function. Every input,
/// block and non-packaging instruction is retained, including dead arithmetic,
/// flags and assume calls. Only return selection and dead insertvalue packaging
/// change. The resulting function must pass the shared scalar model; no DCE,
/// input restriction, parameter deletion or architecture inference occurs.
/// Return leaves must resolve through insertvalue chains or defined constants.
/// Aggregate PHIs/selects, unknown contracts and unsupported scalar effects are
/// refused. Return attributes other than noundef are currently unsupported.
///
/// Input traversal and construction share a budget; model limits are separate.
/// LLVM verification/cloning remain trusted, without hard CPU/stack bounds.
/// Neither the original function nor its module changes, including on refusal.
/// Projection proves neither defined termination nor equality to another
/// function. Run complete scalar proofs for every required status/value/state
/// observation; selecting one leaf alone establishes no whole-state/native ABI
/// certificate or memory/entry contract.
LLVMScalarResultProjectionResult
projectLLVMScalarResult(const llvm::Function &Function,
                        const LLVMScalarResultObservation &Observation,
                        const LLVMScalarResultProjectionLimits &Limits = {});

} // namespace neverd::analysis
#endif
