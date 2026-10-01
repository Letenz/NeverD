//===- X64MachineProbe.h - Private native startup probe -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_X64_MACHINEPROBE_H
#define NEVERD_EMULATION_X64_MACHINEPROBE_H

#include "X64ExceptionMonitor.h"

namespace neverd::emulation {
namespace x64::probe {
#define NEVERD_X64_PROBE_VALUE(Name, Value)                                    \
  inline constexpr uint64_t Name = Value;
#define NEVERD_X64_PROBE_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "X64MachineProbe.def"
#undef NEVERD_X64_PROBE_TEXT
#undef NEVERD_X64_PROBE_VALUE
enum class Step {
#define NEVERD_X64_PROBE_INSTRUCTION(Name, ...) Name,
#include "X64MachineProbe.def"
#undef NEVERD_X64_PROBE_INSTRUCTION
};
struct Instruction {
  Step Kind;
  llvm::ArrayRef<uint8_t> Code;
  const char *Name;
};
#define NEVERD_X64_PROBE_INSTRUCTION(Name, ...)                                \
  inline constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64MachineProbe.def"
#undef NEVERD_X64_PROBE_INSTRUCTION
inline constexpr Instruction Program[] = {
#define NEVERD_X64_PROBE_INSTRUCTION(Name, ...) {Step::Name, Name, #Name},
#include "X64MachineProbe.def"
#undef NEVERD_X64_PROBE_INSTRUCTION
};
constexpr size_t programBytes() {
  return 0
#define NEVERD_X64_PROBE_INSTRUCTION(Name, ...) +sizeof(Name)
#include "X64MachineProbe.def"
#undef NEVERD_X64_PROBE_INSTRUCTION
      ;
}
} // namespace x64::probe

/// Verify bounded native initialization using private supervisor code/data.
/// This does not certify arbitrary workloads or another execution profile.
llvm::Error verifyX64Machine(X64Machine &Machine, MemoryProjection &Memory);
} // namespace neverd::emulation
#endif
