//===- X64Machine.cpp - Architectural native-exit completion ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "X64Machine.h"

#include "../../core/ExecutionDiagnostics.h"

namespace neverd::emulation {
llvm::Error completeX64CR8Read(X64MachineState &State, unsigned GPR,
                               uint64_t InstructionBytes) {
  // Intel SDM, MOV from CR and VM-exit qualification: the first sixteen
  // register identities follow the architectural ModRM/REX numbering.
  static_assert(unsigned(X64Register::AX) == 0 &&
                unsigned(X64Register::R15) == 15);
  if (State.UserMode || GPR > unsigned(X64Register::R15) ||
      InstructionBytes < 4 || InstructionBytes > x64::MaxInstructionBytes ||
      State.reg(X64Register::CR8) > x64::MaxCR8)
    return diagnostic::error("invalid x64 CR8 read exit");
  State.reg(X64Register(GPR)) = State.reg(X64Register::CR8);
  State.reg(X64Register::PC) += InstructionBytes;
  State.reg(X64Register::FLAGS) &= ~x64::ResumeFlag;
  return llvm::Error::success();
}
llvm::Error X64Machine::run(X64MachineState &, uint64_t, MachineRunControl) {
  return diagnostic::error(diagnostic::DirectExecutionUnsupported);
}
} // namespace neverd::emulation
