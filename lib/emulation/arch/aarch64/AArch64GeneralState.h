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
/// Capture all native integer state before publishing any register. The ISA
/// owns NZCV normalization; transport code supplies raw register values.
/// Privilege, vectors and other untransferred state remain unchanged.
llvm::Error captureAArch64GeneralState(AArch64MachineState &State,
                                       AArch64RegisterReader Read);
} // namespace neverd::emulation
#endif
