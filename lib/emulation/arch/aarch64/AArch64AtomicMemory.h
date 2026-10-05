//===- AArch64AtomicMemory.h - Shared atomic memory access policy ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_AARCH64_ATOMICMEMORY_H
#define NEVERD_EMULATION_AARCH64_ATOMICMEMORY_H
#include "AArch64Machine.h"

#include "neverd/emulation/CPU.h"

namespace neverd::emulation {
enum class AArch64AtomicAlignment { Natural, LSE2 };
/// FEAT_LSE2 admits an atomic operand within one aligned 16-byte quantity.
/// Size is the complete decoded operand, including both members of a pair.
bool isAArch64AtomicAligned(uint64_t Address, uint64_t Size,
                            AArch64AtomicAlignment);
struct AArch64AtomicAccess {
  const BackendHooks &Hooks;
  llvm::function_ref<bool()> Stopped;
  llvm::function_ref<llvm::Error(BackendFault)> RaiseFault;
  llvm::function_ref<llvm::Error(uint64_t, uint64_t, unsigned)> CheckAccess;
  unsigned WritePermissions;
  AArch64AtomicAlignment Alignment;
};
/// Validate the complete typed alignment outcome before guest OS translation.
bool isAArch64AtomicAlignmentFault(const BackendFault &);
} // namespace neverd::emulation
#endif
