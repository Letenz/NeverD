//===- LLVMScalarStateProjection.h - x64 entry domain -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_ANALYSIS_X64_LLVMSCALARSTATEPROJECTION_H
#define NEVERD_ANALYSIS_X64_LLVMSCALARSTATEPROJECTION_H

#include "neverd/analysis/InterpreterEntryAlignment.h"
#include "neverd/analysis/InterpreterMachineStateProfile.h"
#include "neverd/analysis/LLVMScalarStateProjection.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <optional>

namespace neverd::analysis {
/// Parameterize exactly the declared profile's valid entry flags using the
/// machine-state semantic owner, with optional caller-declared RSP congruence.
/// Every machine-state input remains a 32-bit cell. Observations are explicit;
/// the factory adds none and certifies neither observer sufficiency nor ABI.
/// Apply only to the InterpreterMachineStateX64V1 source layout. Projection
/// checks observation bounds and source admission separately.
llvm::Expected<LLVMScalarStateContract> llvmScalarStateContractX64(
    InterpreterMachineStateProfile Profile,
    llvm::ArrayRef<LLVMScalarStateObservation> Observations,
    std::optional<InterpreterEntryAlignment> Alignment = std::nullopt);
} // namespace neverd::analysis
#endif
