//===- AArch64AtomicMemory.cpp - Atomic alignment and fault policy --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AArch64AtomicMemory.h"

#include "llvm/Support/MathExtras.h"

namespace neverd::emulation {
bool isAArch64AtomicAligned(uint64_t Address, uint64_t Size,
                            AArch64AtomicAlignment Alignment) {
  assert(llvm::isPowerOf2_64(Size) && Size <= aarch64::AtomicAlignmentGranule);
  if (Alignment == AArch64AtomicAlignment::Natural)
    return Address % Size == 0;
  return Address % aarch64::AtomicAlignmentGranule <=
         aarch64::AtomicAlignmentGranule - Size;
}
bool isAArch64AtomicAlignmentFault(const BackendFault &Fault) {
  return Fault.Kind == BackendFaultKind::Alignment &&
         Fault.Cause == BackendFaultCause::OperandAlignment && Fault.Address &&
         Fault.Size && llvm::isPowerOf2_64(*Fault.Size) &&
         *Fault.Size <= aarch64::AtomicAlignmentGranule &&
         *Fault.Address % *Fault.Size &&
         (Fault.Access == BackendAccessKind::Read ||
          Fault.Access == BackendAccessKind::Write) &&
         !Fault.Interrupt && !Fault.ErrorCode;
}
} // namespace neverd::emulation
