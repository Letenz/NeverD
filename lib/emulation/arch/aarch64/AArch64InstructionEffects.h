//===- AArch64InstructionEffects.h - ARM64 admission and RAM effects -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_AARCH64_INSTRUCTION_EFFECTS_H
#define NEVERD_EMULATION_AARCH64_INSTRUCTION_EFFECTS_H
#include "AArch64Machine.h"

#include <capstone/capstone.h>
#include <vector>

namespace neverd::emulation {
struct AArch64MemoryAccess {
  uint64_t Address;
  RegisterValue Value;
  unsigned Size, Permission;
};
/// Validate one checked instruction and describe its complete RAM footprint.
/// The ISA owns encodings and register views; this query has no guest effects.
llvm::Expected<std::vector<AArch64MemoryAccess>>
getAArch64InstructionEffects(const cs_insn &Instruction,
                             const AArch64MachineState &State);
} // namespace neverd::emulation
#endif
