//===- DriverContextCapture.h - Checked caller-context CPU bridge -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Read-only collection for the shared checked kernel API model.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_DRIVER_CONTEXT_CAPTURE_H
#define NEVERD_EMULATION_DRIVER_CONTEXT_CAPTURE_H

#include "../kernel/KernelContextCapture.h"

#include "neverd/emulation/CPU.h"

namespace neverd::emulation {
llvm::Expected<KernelCapturedContext>
collectDriverContext(ExecutionBackend &CPU);
} // namespace neverd::emulation
#endif
