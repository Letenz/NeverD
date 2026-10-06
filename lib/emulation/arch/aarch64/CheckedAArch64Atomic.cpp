//===- CheckedAArch64Atomic.cpp - Checked atomic access policy ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AArch64Atomic.h"
#include "AArch64Exclusive.h"
#include "CheckedAArch64Backend.h"

namespace neverd::emulation {
llvm::Expected<bool> CheckedAArch64Backend::executeAtomic(const cs_insn &I) {
  auto Atomic = decodeAArch64Atomic(I, CPU);
  if (!Atomic)
    return Atomic.takeError();
  auto Local = decodeAArch64Exclusive(I, CPU);
  if (!Local)
    return Local.takeError();
  if (!*Atomic && !*Local)
    return false;
  auto Stopped = [&] { return StopRequested || FirstFault; };
  auto Raise = [&](BackendFault Fault) { return raiseFault(Fault, true); };
  auto Check = [&](uint64_t Address, uint64_t Size, unsigned Permissions) {
    return access(Address, Size, Permissions, true, true);
  };
  const AArch64AtomicAccess Access{Hooks,
                                   Stopped,
                                   Raise,
                                   Check,
                                   executionPermissions(Write),
                                   AArch64AtomicAlignment::LSE2};
  if (auto E = *Atomic ? executeAArch64Atomic(**Atomic, CPU, *Memory, Access)
                       : executeAArch64Exclusive(**Local, CPU, Exclusive,
                                                 *Memory, Access))
    return std::move(E);
  return true;
}
} // namespace neverd::emulation
