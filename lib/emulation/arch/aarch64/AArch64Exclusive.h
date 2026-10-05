//===- AArch64Exclusive.h - Architectural exclusive memory operations ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_AARCH64_EXCLUSIVE_H
#define NEVERD_EMULATION_AARCH64_EXCLUSIVE_H
#include "AArch64AtomicMemory.h"

#include "neverd/emulation/CPU.h"

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
llvm::Expected<std::optional<AArch64ExclusiveInstruction>>
decodeAArch64Exclusive(uint32_t Word, const AArch64MachineState &);
bool isAArch64Exclusive(uint32_t Word);
/// Complete one exclusive instruction under the physical execution lease.
/// Both software execution and checked transports use this monitor authority.
llvm::Error executeAArch64Exclusive(const AArch64ExclusiveInstruction &,
                                    AArch64MachineState &,
                                    std::shared_ptr<RAMReservation> &,
                                    MemoryProjection &,
                                    const AArch64AtomicAccess &);
} // namespace neverd::emulation
#endif
