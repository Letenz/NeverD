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
// Select return-only PHIs on their original incoming edges so error paths can
// print direct returns. Never duplicates a load, call or cleanup operation.
bool distributeReturnEdges(llvm::Function &Function);
} // namespace neverd::llvmc

#endif
