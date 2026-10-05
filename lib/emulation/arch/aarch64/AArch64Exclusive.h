//===- AArch64Exclusive.h - Architectural exclusive memory operations ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_AARCH64_EXCLUSIVE_H
#define NEVERD_EMULATION_AARCH64_EXCLUSIVE_H
#include "AArch64Machine.h"

#include <capstone/capstone.h>

namespace neverd::emulation {
struct AArch64ExclusiveInstruction {
  enum class Operation { Load, Store, Clear } Kind;
  unsigned Width, Count, First, Second, Base, Status;
  uint64_t Address;
  unsigned size() const { return Width * Count; }
};
/// Decode the original baseline word, rejecting constrained-unpredictable
/// register overlaps before any memory access or local monitor transition.
llvm::Expected<std::optional<AArch64ExclusiveInstruction>>
decodeAArch64Exclusive(const cs_insn &, const AArch64MachineState &);
/// Validate the complete typed alignment outcome before guest OS translation.
bool isAArch64ExclusiveAlignmentFault(const BackendFault &);
} // namespace neverd::emulation
#endif
