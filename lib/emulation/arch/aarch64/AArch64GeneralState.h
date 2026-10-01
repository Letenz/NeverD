//===- AArch64GeneralState.h - Atomic architectural register capture ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_AARCH64_GENERAL_STATE_H
#define NEVERD_EMULATION_AARCH64_GENERAL_STATE_H
#include "AArch64Machine.h"

#include "llvm/ADT/STLFunctionalExtras.h"

namespace neverd::emulation {
using AArch64RegisterReader =
    llvm::function_ref<llvm::Expected<uint64_t>(AArch64Register)>;
using AArch64VectorReader =
    llvm::function_ref<llvm::Expected<RegisterValue>(unsigned)>;
/// Capture the legacy integer subset before publishing any register. The ISA
/// owns NZCV normalization; transport code supplies raw register values.
/// Privilege, vectors and other untransferred state remain unchanged.
llvm::Error captureAArch64GeneralState(AArch64MachineState &State,
                                       AArch64RegisterReader Read);
/// Capture the complete public scalar inventory with its declared widths.
/// This is the scalar phase of complete machine capture, including thread and
/// FP control state. Vector payloads are committed by captureAArch64State.
llvm::Error captureAArch64ScalarState(AArch64MachineState &State,
                                      AArch64RegisterReader Read);
/// Publish the complete scalar and vector inventory as one transaction. A
/// failed transport read cannot expose an advanced PC or partial FP/SIMD state.
llvm::Error captureAArch64State(AArch64MachineState &State,
                                AArch64RegisterReader ReadScalar,
                                AArch64VectorReader ReadVector);
} // namespace neverd::emulation
#endif
