//===- UnicornArchitecture.cpp - Explicit software CPU reset state -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "UnicornArchitecture.h"

#include "../../arch/aarch64/AArch64Machine.h"
#include "../../arch/x86_64/X64Machine.h"
#include "../../core/ExecutionDiagnostics.h"

#include <unicorn/arm64.h>
#include <unicorn/x86.h>
namespace neverd::emulation {
llvm::Error initializeUnicornArchitecture(uc_engine *CPU,
                                          GuestArchitecture Architecture) {
  if (Architecture == GuestArchitecture::X64) {
    uint64_t Flags = x64::InitialFlags;
    if (uc_reg_write(CPU, UC_X86_REG_RFLAGS, &Flags) != UC_ERR_OK)
      return diagnostic::error(diagnostic::UnicornFlags);
  } else if (Architecture == GuestArchitecture::AArch64) {
    uint32_t PState = aarch64::PStateEL1h | aarch64::PStateDAIF;
    if (uc_reg_write(CPU, UC_ARM64_REG_PSTATE, &PState) != UC_ERR_OK)
      return diagnostic::error(diagnostic::ArmState);
#define NEVERD_UNICORN_AARCH64_SYSTEM_REGISTER(Name, Op0, Op1, CRn, CRm, Op2,  \
                                               Value)                          \
  {                                                                            \
    uc_arm64_cp_reg R{CRn, CRm, Op0, Op1, Op2, Value};                         \
    if (uc_reg_write(CPU, UC_ARM64_REG_CP_REG, &R) != UC_ERR_OK)               \
      return diagnostic::error(diagnostic::ArmState);                          \
  }
#include "UnicornArchitecture.def"
#undef NEVERD_UNICORN_AARCH64_SYSTEM_REGISTER
  } else
    return diagnostic::error(diagnostic::Architecture);
  return llvm::Error::success();
}
} // namespace neverd::emulation
