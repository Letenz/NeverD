//===- AArch64MachineProbe.h - Private ARM64 probe -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_AARCH64_MACHINEPROBE_H
#define NEVERD_EMULATION_AARCH64_MACHINEPROBE_H
#include <cstdint>

namespace neverd::emulation::aarch64::probe {
enum class Step {
#define NEVERD_AARCH64_PROBE_INSTRUCTION(Name, Word) Name,
#include "AArch64MachineProbe.def"
#undef NEVERD_AARCH64_PROBE_INSTRUCTION
};
struct Instruction {
  Step Kind;
  uint32_t Word;
};
inline constexpr Instruction Program[] = {
#define NEVERD_AARCH64_PROBE_INSTRUCTION(Name, Word) {Step::Name, Word},
#include "AArch64MachineProbe.def"
#undef NEVERD_AARCH64_PROBE_INSTRUCTION
};
} // namespace neverd::emulation::aarch64::probe
#endif
