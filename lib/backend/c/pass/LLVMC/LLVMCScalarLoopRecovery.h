//===- LLVMCScalarLoopRecovery.h - Proved source loops -----------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LLVMCSCALARLOOPRECOVERY_H
#define NEVERD_LLVMCSCALARLOOPRECOVERY_H

#include "neverd/analysis/LLVMScalarSourceRecovery.h"

namespace neverd::llvmc {

/// Normalize only proved scalar functions in the emitter's private clone.
/// Search and final cleanup proofs share one module-wide budget. Refused or
/// exhausted functions retain their complete original bodies.
unsigned
recoverScalarLoops(llvm::Module &Module, const llvm::Function *Only = nullptr,
                   const analysis::LLVMScalarSourceRecoveryLimits &Limits = {});

} // namespace neverd::llvmc
#endif
