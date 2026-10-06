//===- X64ExceptionMonitor.h - Private IDT and IST projection -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_ARCH_X64EXCEPTIONMONITOR_H
#define NEVERD_EMULATION_ARCH_X64EXCEPTIONMONITOR_H

#include "X64Exception.h"
#include "X64Machine.h"

namespace neverd::emulation {
namespace x64::gateway {
#define NEVERD_X64_GATEWAY_VALUE(Name, Value)                                  \
  inline constexpr uint64_t Name = Value;
#include "X64Exceptions.def"
#undef NEVERD_X64_GATEWAY_VALUE
} // namespace x64::gateway

/// Choose unmapped supervisor VAs without reserving caller address-space RAM.
/// The three native pages are private per CPU and never enter AddressSpace.
llvm::Expected<uint64_t> initializeX64ExceptionMonitor(MemoryProjection &);
uint64_t x64ExceptionMonitorBase(const MemoryProjection &);

/// Authenticate the completed HLT entry and hardware-pushed IST frame. Restore
/// architectural PC/SP/flags, retaining processor exception status and GPRs.
/// A forged gateway entry or malformed frame remains a transport failure.
/// A single step is also checked against the state it started from. A free
/// entry (\p Stepped false) ran any number of instructions first, so its
/// frame alone states where the exception occurred; the guest owns its trap
/// flag there, and only the resume flag is withheld.
llvm::Expected<X64Exception>
consumeX64ExceptionMonitor(const MemoryProjection &, X64MachineState &,
                           const X64MachineState &Before, uint64_t CR2,
                           bool Stepped = true);
} // namespace neverd::emulation
#endif
