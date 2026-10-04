//===- LLVMCCommonBranches.h - Source control normalization -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LIB_BACKEND_C_PASS_LLVMC_COMMONBRANCHES_H
#define NEVERD_LIB_BACKEND_C_PASS_LLVMC_COMMONBRANCHES_H

namespace llvm {
class Function;
}

namespace neverd::llvmc {
// Operates on the source-only clone, before any emission analyses are built.
bool factorCommonBranchTests(llvm::Function &Function);
} // namespace neverd::llvmc

#endif
