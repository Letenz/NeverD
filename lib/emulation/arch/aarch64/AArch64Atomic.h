//===- AArch64Atomic.h - Architectural LSE atomic instructions ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_AARCH64_ATOMIC_H
#define NEVERD_EMULATION_AARCH64_ATOMIC_H
#include "AArch64AtomicMemory.h"

#include <capstone/capstone.h>

namespace neverd::emulation {
struct AArch64AtomicInstruction {
  enum class Operation {
#define NEVERD_AARCH64_ATOMIC_RMW(Name, Kind, Mask, Value) Kind,
#define NEVERD_AARCH64_ATOMIC_COMPARE NEVERD_AARCH64_ATOMIC_RMW
#define NEVERD_AARCH64_ATOMIC_PAIR NEVERD_AARCH64_ATOMIC_RMW
#include "AArch64AtomicInstructions.def"
#undef NEVERD_AARCH64_ATOMIC_RMW
#undef NEVERD_AARCH64_ATOMIC_COMPARE
#undef NEVERD_AARCH64_ATOMIC_PAIR
  } Kind;
  unsigned Width, Count, Source, Target;
  uint64_t Address;
  unsigned size() const { return Width * Count; }
  bool compare() const {
    return Kind == Operation::Compare || Kind == Operation::ComparePair;
  }
};
/// Reject undefined pair encodings before callbacks, RAM or CPU mutation.
llvm::Expected<std::optional<AArch64AtomicInstruction>>
decodeAArch64Atomic(const cs_insn &, const AArch64MachineState &);
llvm::Expected<std::optional<AArch64AtomicInstruction>>
decodeAArch64Atomic(uint32_t Word, const AArch64MachineState &);
bool isAArch64Atomic(uint32_t Word);
/// The caller holds the shared physical execution lease. Acquire/release
/// ordering is provided by that lease in this cooperative execution profile.
llvm::Error executeAArch64Atomic(const AArch64AtomicInstruction &,
                                 AArch64MachineState &, MemoryProjection &,
                                 const AArch64AtomicAccess &);
} // namespace neverd::emulation
#endif
