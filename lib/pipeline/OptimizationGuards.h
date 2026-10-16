//===- OptimizationGuards.h - Preserve checked optimization boundaries ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Pass instrumentation for contracts that require unchanged function bodies.

#ifndef NEVERD_PIPELINE_OPTIMIZATIONGUARDS_H
#define NEVERD_PIPELINE_OPTIMIZATIONGUARDS_H

namespace llvm {
class PassInstrumentationCallbacks;
} // namespace llvm

namespace neverd {

/// Supplement LLVM's function/loop optnone instrumentation. Optional module
/// and call-graph passes may otherwise change an optnone body or its callers.
void registerOptNoneInterproceduralGuard(
    llvm::PassInstrumentationCallbacks &Callbacks);

} // namespace neverd

#endif // NEVERD_PIPELINE_OPTIMIZATIONGUARDS_H
