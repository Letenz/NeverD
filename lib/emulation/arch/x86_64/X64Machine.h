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
inline constexpr unsigned ExecutionStopCount = 4;
inline uint64_t executionStopControl(unsigned Count) {
  uint64_t Control = 0x400; // DR7's architecturally fixed bit.
  for (unsigned I = 0; I < Count; ++I)
    Control |= uint64_t(1) << (I * 2);
  return Control;
}
inline bool executionStopHit(llvm::ArrayRef<uint64_t> Stops, uint64_t PC,
                             uint64_t Status) {
  // A guest trap or debug-register access is not our execution breakpoint.
  if (Status &
      ((uint64_t(1) << 13) | (uint64_t(1) << 14) | (uint64_t(1) << 15)))
    return false;
  for (unsigned I = 0; I < Stops.size(); ++I)
    if ((Status & (uint64_t(1) << I)) && Stops[I] == PC)
      return true;
  return false;
}
} // namespace x64
class MemoryProjection;
struct X64PageTableCache;
/// Build the guest page tables and return the transport-physical root. When
/// \p NoExecutePages is non-empty, those guest pages are additionally marked
/// non-executable regardless of their permissions, so a direct run's first
/// fetch into one faults; \p WatchEpoch distinguishes successive overlays so a
/// cached projection is not reused across a change to the set. \p WatchWrites
/// makes every alias of a watched RAM page read-only for direct execution.
/// \p WriteGuard additionally protects one physical code page while its
/// decoded control-flow plan is in use; changes require a new watch epoch.
llvm::Expected<uint64_t>
buildX64PageTables(MemoryProjection &Memory, bool UserMode = false,
                   bool ExceptionMonitor = false,
                   llvm::ArrayRef<ExecutionWatch> NoExecutePages = {},
                   uint64_t WatchEpoch = 0, bool WatchWrites = false,
                   std::optional<uint64_t> WriteGuard = std::nullopt,
                   X64PageTableCache *Cache = nullptr);
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
/// Complete a hardware-authenticated MOV from CR8 virtualization exit. The
/// transport supplies the architectural GPR number and hardware instruction
/// length. This does not admit control-register instructions to checked code.
llvm::Error completeX64CR8Read(X64MachineState &State, unsigned GPR,
                               uint64_t InstructionBytes);
/// Native execution of one already admitted instruction. No OS models,
/// instruction decoding, memory ownership or lifecycle decisions belong here.
/// The v1 contract admits scalar integer and bounded SSE/SSE2 data operations.
/// Admitted legacy SSE arithmetic and conversions retain MXCSR and the
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
  virtual uint32_t mxcsrMask() const { return x64::fp::BaselineMXCSRMask; }
  virtual X64BranchModel branchModel() const { return X64BranchModel::Intel; }
  virtual llvm::Error step(X64MachineState &State, uint64_t PageTableRoot,
                           MachineRunControl Control) = 0;
  /// Native execution of user code that nothing admitted, until the processor
  /// raises a synchronous exception or \p Control interrupts it. A system
  /// call instruction is such an exception: this profile leaves the system
  /// call extension disabled, so the instruction is undefined and its address
  /// is the reported program counter. An exception returns X64ExceptionError
  /// with the architectural state at the faulting instruction; an interrupted
  /// run publishes the state at the instruction boundary it stopped on.
  /// Transports without this entry return an unsupported-contract error.
  virtual llvm::Error run(X64MachineState &State, uint64_t PageTableRoot,
                          MachineRunControl Control);
  /// Run with private execution stops, returning success before a matching
  /// instruction. Real exceptions and interruption retain their ordinary
  /// outcomes. No instruction bytes or architectural trap flags are patched.
  virtual bool supportsExecutionStops() const { return false; }
  virtual llvm::Error runTo(X64MachineState &State, uint64_t PageTableRoot,
                            MachineRunControl Control,
                            llvm::ArrayRef<uint64_t> Stops);
};
} // namespace neverd::emulation
#endif
