//===- CheckedAArch64Exclusive.cpp - Checked exclusive access policy ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AArch64Exclusive.h"
#include "CheckedAArch64Backend.h"

namespace neverd::emulation {
llvm::Error
CheckedAArch64Backend::executeExclusive(const AArch64ExclusiveInstruction &I) {
  return executeAArch64Exclusive(
      I, CPU, Exclusive, *Memory,
      {Hooks, [&] { return StopRequested || FirstFault; },
       [&](BackendFault Fault) { return raiseFault(Fault, true); },
       [&](uint64_t Address, uint64_t Size, unsigned Permissions) {
         return access(Address, Size, Permissions, true, true);
       },
       executionPermissions(Write)});
}
} // namespace neverd::emulation
