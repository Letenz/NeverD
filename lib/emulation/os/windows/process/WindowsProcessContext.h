//===- WindowsProcessContext.h - Windows user CONTEXT codec ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_WINDOWS_PROCESS_CONTEXT_H
#define NEVERD_EMULATION_WINDOWS_PROCESS_CONTEXT_H
#include "neverd/emulation/CPU.h"

#include <vector>

namespace neverd::emulation::windows_process {
llvm::Expected<std::vector<uint8_t>> captureUserContext(ExecutionBackend &CPU);
/// Validate the complete guest record before restoring any state. CPU state
/// outside the Windows CONTEXT remains owned by the original backend snapshot.
llvm::Error restoreUserContext(ExecutionBackend &CPU,
                               const BackendContext &Snapshot,
                               llvm::ArrayRef<uint8_t> Original,
                               llvm::ArrayRef<uint8_t> Changed,
                               uint64_t StackBase, uint64_t StackTop);
} // namespace neverd::emulation::windows_process
#endif
