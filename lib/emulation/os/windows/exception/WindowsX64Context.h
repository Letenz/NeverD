//===- WindowsX64Context.h - Shared Windows x64 context collection -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared register layout and logical context publication, independent of
/// user/kernel buffer policy and concrete execution transports.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_WINDOWS_X64_CONTEXT_H
#define NEVERD_EMULATION_WINDOWS_X64_CONTEXT_H

#include "X64SEH.h"

#include "neverd/emulation/CPU.h"

namespace neverd::emulation::windows_context {
#define NEVERD_WINDOWS_X64_CAPTURE_FLAGS(Value)                                \
  inline constexpr uint32_t CapturedFlags = Value;
#include "WindowsX64Context.def"
#undef NEVERD_WINDOWS_X64_CAPTURE_FLAGS

/// Collect the shared integer/control fields without changing guest state.
/// HardwareFault exposes the exception delivery RF bit only in the record.
llvm::Error collectX64IntegerContext(ExecutionBackend &CPU,
                                     llvm::MutableArrayRef<uint8_t> Bytes,
                                     bool HardwareFault = false);

/// The checked logical context uses its flat data selector. Raw transport
/// selectors and host operating-system selector values are not inputs.
void writeFlatDataSelectors(llvm::MutableArrayRef<uint8_t> Bytes,
                            uint16_t Selector);
} // namespace neverd::emulation::windows_context
#endif
