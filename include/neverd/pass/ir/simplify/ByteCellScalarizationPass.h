//===- ByteCellScalarizationPass.h - Promote overlapping cells ---*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_PASS_IR_SIMPLIFY_BYTECELLSCALARIZATIONPASS_H
#define NEVERD_PASS_IR_SIMPLIFY_BYTECELLSCALARIZATIONPASS_H

#include "llvm/IR/PassManager.h"

#include <cstdint>

namespace neverd {

/// Per-function work/output ceilings, including work on refused objects, and
/// a per-object size admission ceiling. Zero permits no work of that kind.
/// Work/output exhaustion leaves the whole function unchanged.
struct ByteCellScalarizationOptions {
  uint64_t MaxInstructions = 262144;
  uint64_t MaxObjectBytes = 65536;
  uint64_t MaxCells = 4096;
  uint64_t MaxNewInstructions = 131072;
  uint64_t MaxWork = 1048576;
};

struct ByteCellScalarizationResult {
  uint64_t Objects = 0;
  uint64_t Cells = 0;
  uint64_t Accesses = 0;
  /// Charged construction upper bound; constant GEP folding may emit less.
  uint64_t NewInstructions = 0;
  uint64_t Instructions = 0;
  uint64_t Work = 0;
  bool BudgetExhausted = false;
};

/// Split remaining nonescaping static byte arrays at every access boundary.
/// Each covered interval uses 8/16/32/64-bit cells; every admitted access
/// covers whole cells. Only in-object constant GEPs and ordinary byte-sized
/// integer loads/stores up to 128 bits are supported. At least one proper
/// overlapping boundary is required; exact disjoint scalar homes belong to
/// ordinary SROA.
///
/// A composite access uses a private typed memory copy, evaluating each stored
/// operand once and preserving the target's memory representation. No freeze,
/// initialization, entry-value or native-frame contract is introduced. SROA
/// can then promote the exact cells and per-access copies, including at joins
/// and backedges. Calls other than the shared frame-independent arithmetic
/// intrinsics, escaping/dynamic/ordered accesses, metadata and debug records
/// conservatively prevent the affected transformation. Requires verified IR.
struct ByteCellScalarizationPass
    : public llvm::PassInfoMixin<ByteCellScalarizationPass> {
  explicit ByteCellScalarizationPass(ByteCellScalarizationOptions Options = {})
      : Options(Options) {}

  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &FAM);
  static ByteCellScalarizationResult
  scalarize(llvm::Function &F, ByteCellScalarizationOptions Options = {});

private:
  ByteCellScalarizationOptions Options;
};

} // namespace neverd
#endif
