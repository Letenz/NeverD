//===- LinuxUserMemory.h - Bounded Linux user copies ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_OS_LINUX_LINUXUSERMEMORY_H
#define NEVERD_EMULATION_OS_LINUX_LINUXUSERMEMORY_H

#include "LinuxKernel.h"

namespace neverd::emulation::linux_model {
enum class UserWriteResult { Stored, BadAddress, MixedAccess };

/// Complete fixed-size stores and wholly inaccessible buffers are modeled.
/// Mixed access needs an architecture-specific partial-copy contract.
llvm::Expected<UserWriteResult> writeUserMemory(ExecutionBackend &CPU,
                                                const MemoryLayout &Layout,
                                                uint64_t Address,
                                                llvm::ArrayRef<uint8_t> Bytes);
} // namespace neverd::emulation::linux_model
#endif
