//===- AArch64GeneralState.cpp - Atomic architectural register capture ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AArch64GeneralState.h"

#include "../../core/ExecutionDiagnostics.h"

namespace neverd::emulation {
llvm::Error captureAArch64GeneralState(AArch64MachineState &State,
                                       AArch64RegisterReader Read) {
  if (!Read)
    return diagnostic::error(diagnostic::Register);
  auto Next = State;
  constexpr AArch64Register Registers[] = {
#define NEVERD_AARCH64_GENERAL_REGISTER(Name) AArch64Register::Name,
#include "AArch64GeneralState.def"
#undef NEVERD_AARCH64_GENERAL_REGISTER
  };
  for (auto Register : Registers) {
    auto Value = Read(Register);
    if (!Value)
      return Value.takeError();
    Next.reg(Register) = *Value;
  }
  Next.reg(AArch64Register::NZCV) &= aarch64::NZCVMask;
  State = Next;
  return llvm::Error::success();
}
} // namespace neverd::emulation
