//===- LLVMCLoopPhases.h - Recombine loop expressions -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LIB_BACKEND_C_PASS_LLVMC_LOOPPHASES_H
#define NEVERD_LIB_BACKEND_C_PASS_LLVMC_LOOPPHASES_H

namespace llvm {
class Function;
}

namespace neverd::llvmc {
// Operates on the source-only clone, without changing control-flow edges.
bool factorLoopPhiExpressions(llvm::Function &Function);
} // namespace neverd::llvmc

#endif
