//===- AArch64Machine.h - ARM64 native machine boundary ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_AARCH64_MACHINE_H
#define NEVERD_EMULATION_AARCH64_MACHINE_H
#include "../../core/MemoryLayout.h"
#include "../../core/PhysicalMemory.h"

#include <chrono>
namespace neverd::emulation {
namespace aarch64 {
#define NEVERD_AARCH64_VALUE(Name, Value)                                      \
  inline constexpr uint64_t Name = Value;
#include "AArch64Machine.def"
#undef NEVERD_AARCH64_VALUE
inline constexpr uint32_t Maintenance[] = {
#define NEVERD_AARCH64_MAINTENANCE(Name, Encoding) Encoding,
#include "AArch64Machine.def"
#undef NEVERD_AARCH64_MAINTENANCE
};
inline bool canonical(uint64_t A) { return A <= UserMax || A >= KernelMin; }
} // namespace aarch64
struct AArch64MachineState {
  std::array<uint64_t, unsigned(AArch64Register::FPSR) + 1> Registers{};
  std::array<RegisterValue, aarch64::VectorCount> Vectors{};
  uint64_t &reg(AArch64Register R) { return Registers[unsigned(R)]; }
  uint64_t reg(AArch64Register R) const { return Registers[unsigned(R)]; }
};
/// One admitted instruction; architectural transport never decides OS behavior.
class AArch64Machine {
public:
  virtual ~AArch64Machine() = default;
  virtual llvm::Error step(AArch64MachineState &State,
                           std::chrono::steady_clock::time_point Deadline) = 0;
};
llvm::Error buildAArch64PageTables(PhysicalMemory &Memory);
llvm::Error verifyAArch64Machine(AArch64Machine &Machine,
                                 PhysicalMemory &Memory);
} // namespace neverd::emulation
#endif
