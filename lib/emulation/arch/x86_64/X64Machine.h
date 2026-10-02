//===- X64Machine.h - Host-independent checked machine state -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_ARCH_X64MACHINE_H
#define NEVERD_EMULATION_ARCH_X64MACHINE_H
#include "../../core/MachineRunControl.h"
#include "../../core/MemoryLayout.h"
#include "X64FPState.h"

#include "neverd/emulation/CPU.h"

#include <chrono>

namespace neverd::emulation {
namespace x64 {
#define NEVERD_X64_MACHINE_VALUE(Name, Value)                                  \
  inline constexpr uint64_t Name = Value;
#include "X64Machine.def"
#undef NEVERD_X64_MACHINE_VALUE
inline bool canonical(uint64_t Address) {
  return Address <= UserMax || Address >= KernelMin;
}
inline bool canonicalRange(uint64_t Address, uint64_t Size) {
  return Size && Size - 1 <= UINT64_MAX - Address && canonical(Address) &&
         canonical(Address + Size - 1) &&
         ((Address <= UserMax) == (Address + Size - 1 <= UserMax));
}
} // namespace x64
class MemoryProjection;
llvm::Expected<uint64_t> buildX64PageTables(MemoryProjection &Memory,
                                            bool UserMode = false,
                                            bool ExceptionMonitor = false);
struct X64MachineState {
  bool UserMode = false;
  std::array<uint64_t, unsigned(X64Register::SS) + 1> Registers{};
  uint64_t GSBase = 0;
  uint64_t FSBase = 0;
  uint32_t MXCSR = x64::InitialMXCSR;
  X64FPState FP;
  std::array<ExecutionBackend::XmmValue, x64::XmmCount> Xmm{};
  bool operator==(const X64MachineState &) const = default;
  uint64_t &reg(X64Register R) { return Registers[unsigned(R)]; }
  uint64_t reg(X64Register R) const { return Registers[unsigned(R)]; }
};
/// Native execution of one already admitted instruction. No OS models,
/// instruction decoding, memory ownership or lifecycle decisions belong here.
/// The v1 contract admits scalar integer and bounded SSE/SSE2 data operations.
/// Admitted masked legacy SSE arithmetic and conversions retain MXCSR and the
/// complete legacy FP/SSE state in the CPU context.
/// Synchronous processor faults return X64ExceptionError with their original
/// architectural context. Transport failures do not publish partial CPU state.
/// The architecture excludes unlisted floating-point/vector families,
/// privileged instructions, debug/control flags and unbounded execution.
/// CLD/STD may change the direction flag used by checked string transfers.
class X64Machine {
public:
  virtual ~X64Machine() = default;
  virtual bool requiresExceptionMonitor() const { return false; }
  virtual llvm::Error step(X64MachineState &State, uint64_t PageTableRoot,
                           MachineRunControl Control) = 0;
};
} // namespace neverd::emulation
#endif
