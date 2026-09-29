//===- ISAEncoding.h - ISA instruction encoding constants -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Instruction encoding constants for x86, AArch64, and ARM.  Used by both
/// loader (IAT thunk scanning, prologue detection) and codegen (trampoline
/// writing, relocation patching) layers.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_ISAENCODING_H
#define NEVERD_SUPPORT_ISAENCODING_H

#include "neverd/Common.h"
#include "neverd/support/BranchEncoding.h"

#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Win64EH.h"

#include <cstddef>
#include <cstdint>

namespace neverd {

// The constants are in ISAEncoding.def, one macro per namespace.

namespace x86 {
#define NEVERD_X86_ENCODING(Type, Name, Value) constexpr Type Name = Value;
#include "neverd/support/ISAEncoding.def"
} // namespace x86

namespace aarch64 {
#define NEVERD_AARCH64_ENCODING(Type, Name, Value) constexpr Type Name = Value;
#include "neverd/support/ISAEncoding.def"
} // namespace aarch64

namespace elf {
#define NEVERD_ELF_ENCODING(Type, Name, Value) constexpr Type Name = Value;
#include "neverd/support/ISAEncoding.def"

inline uint32_t getPLTEntrySize(Arch A) {
  switch (A) {
  case Arch::ARM:
    return kARMPLTEntrySize;
  case Arch::AArch64:
    return kAArch64PLTEntrySize;
  default:
    return kDefaultPLTEntrySize;
  }
}
} // namespace elf

namespace macho {
#define NEVERD_MACHO_ENCODING(Type, Name, Value) constexpr Type Name = Value;
#include "neverd/support/ISAEncoding.def"

inline uint32_t getStubSize(Arch A) {
  switch (A) {
  case Arch::AArch64:
    return kAArch64StubSize;
  case Arch::ARM:
    return kARMStubSize;
  default:
    return kDefaultStubSize;
  }
}
} // namespace macho

namespace arm {
#define NEVERD_ARM_ENCODING(Type, Name, Value) constexpr Type Name = Value;
#include "neverd/support/ISAEncoding.def"
} // namespace arm

// PE x64 unwind info layout (cf. llvm/Support/Win64EH.h).
namespace unwind {
#define NEVERD_UNWIND_ENCODING(Type, Name, Value) constexpr Type Name = Value;
#include "neverd/support/ISAEncoding.def"
using llvm::Win64EH::UNW_ChainInfo;
} // namespace unwind

} // namespace neverd

#endif // NEVERD_SUPPORT_ISAENCODING_H
