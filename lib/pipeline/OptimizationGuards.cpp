//===- OptimizationGuards.cpp - Preserve checked optimization boundaries --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keeps interprocedural transforms from invalidating optnone contracts.

#include "OptimizationGuards.h"

#include "llvm/ADT/Any.h"
#include "llvm/Analysis/LazyCallGraph.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassInstrumentation.h"

namespace neverd {

void registerOptNoneInterproceduralGuard(
    llvm::PassInstrumentationCallbacks &Callbacks) {
  Callbacks.registerShouldRunOptionalPassCallback(
      [](llvm::StringRef, llvm::Any IR) {
        const llvm::Module *M = nullptr;
        if (const auto *Module = llvm::any_cast<const llvm::Module *>(&IR))
          M = *Module;
        else if (const auto *SCC =
                     llvm::any_cast<const llvm::LazyCallGraph::SCC *>(&IR)) {
          if ((*SCC)->begin() != (*SCC)->end())
            M = (*SCC)->begin()->getFunction().getParent();
        }
        if (!M)
          return true;

        // LLVM's optnone instrumentation only guards function/loop passes.
        // GlobalOpt can still rewrite a protected CFG, and dead-argument
        // elimination can replace checked callback arguments with poison.
        // A call-graph pass on an ordinary caller can also affect a protected
        // callee's ABI, so use the same module-wide boundary for both. Required
        // adaptors still run and optimize ordinary functions independently.
        for (const llvm::Function &F : *M)
          if (!F.isDeclaration() && F.hasOptNone())
            return false;
        return true;
      });
}

} // namespace neverd
