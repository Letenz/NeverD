//===- AArch64GeneralState.cpp - Atomic architectural register capture ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AArch64GeneralState.h"

#include "../../core/ExecutionDiagnostics.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/MathExtras.h"

namespace neverd::emulation {
namespace {
llvm::Error captureRegisters(AArch64MachineState &State,
                             AArch64RegisterReader Read,
                             llvm::ArrayRef<AArch64Register> Registers) {
  if (!Read)
    return diagnostic::error(diagnostic::Register);
  auto Next = State;
  for (auto Register : Registers) {
    auto Value = Read(Register);
    if (!Value)
      return Value.takeError();
    Next.reg(Register) = *Value & llvm::maskTrailingOnes<uint64_t>(
                                      registerWidth(cpuRegister(Register)));
  }
  Next.reg(AArch64Register::NZCV) &= aarch64::NZCVMask;
  State = Next;
  return llvm::Error::success();
}
} // namespace
llvm::Error captureAArch64GeneralState(AArch64MachineState &State,
                                       AArch64RegisterReader Read) {
  constexpr AArch64Register Registers[] = {
#define NEVERD_AARCH64_GENERAL_REGISTER(Name) AArch64Register::Name,
#include "AArch64GeneralState.def"
#undef NEVERD_AARCH64_GENERAL_REGISTER
  };
  return captureRegisters(State, Read, Registers);
}
llvm::Error captureAArch64ScalarState(AArch64MachineState &State,
                                      AArch64RegisterReader Read) {
  constexpr AArch64Register Registers[] = {
#define NEVERD_SCALAR_REGISTER(Arch, Name, Width, Backend)                     \
  NEVERD_REGISTER_##Arch(Name)
#define NEVERD_REGISTER_X64(Name)
#define NEVERD_REGISTER_AArch64(Name) AArch64Register::Name,
#include "neverd/emulation/Registers.def"
#undef NEVERD_REGISTER_AArch64
#undef NEVERD_REGISTER_X64
#undef NEVERD_SCALAR_REGISTER
  };
  return captureRegisters(State, Read, Registers);
}
} // namespace neverd::emulation
