//===- X86NativeMemory.h - Native memory address admission ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check whether a normalized offset retains a DS-based native carrier's
/// faults.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LIFT_X86_X86NATIVEMEMORY_H
#define NEVERD_LIFT_X86_X86NATIVEMEMORY_H

#include "neverd/lift/X86/X86Lifter.h"
#include "neverd/support/X86/X86Addressing.h"

namespace neverd {
inline bool x86NativeDsAddressIsValid(Arch A, const cs_x86 &X86,
                                      const cs_x86_op &Memory) {
  const auto Space = X86Lifter::LiftState::memoryAddressSpace(Memory);
  if (Space == NdMemoryAddressSpace::X86FS ||
      Space == NdMemoryAddressSpace::X86GS)
    return true;
  const auto Base = Memory.mem.base;
  const bool Stack = isStackDefaultBase(Base);
  const auto Segment = Memory.mem.segment;
  // A normalized address loses SS-default noncanonical/limit fault identity.
  // A long-mode SS override on a DS-default base cannot restore that identity.
  return !((A == Arch::X64 && X86.addr_size == 8 && Stack) ||
           (A == Arch::X86 &&
            ((Stack && Segment == X86_REG_INVALID) ||
             (Segment != X86_REG_INVALID && Segment != X86_REG_DS))));
}
} // namespace neverd
#endif // NEVERD_LIFT_X86_X86NATIVEMEMORY_H
