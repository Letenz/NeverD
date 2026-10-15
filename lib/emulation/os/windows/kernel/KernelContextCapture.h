//===- KernelContextCapture.h - Borrowed caller-context CPU services -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Per-call CPU services; kernel memory and publication policy stay in the
/// kernel model. Services are never retained beyond the synchronous API call.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_KERNEL_CONTEXT_CAPTURE_H
#define NEVERD_EMULATION_KERNEL_CONTEXT_CAPTURE_H

#include "../exception/X64SEH.h"

#include "llvm/ADT/STLFunctionalExtras.h"

namespace neverd::emulation {
struct KernelCapturedContext {
  std::array<uint8_t, seh::ContextSize> Bytes{};
};

/// Both services borrow the model's same stopped CPU and memory. Read is
/// read-only; SetFlags changes only logical FLAGS and leaves state unchanged
/// on failure. Neither service may reenter, schedule or change memory mappings.
struct KernelContextCapture {
  llvm::function_ref<llvm::Expected<KernelCapturedContext>()> Read;
  llvm::function_ref<llvm::Error(uint64_t)> SetFlags;
};
} // namespace neverd::emulation
#endif
