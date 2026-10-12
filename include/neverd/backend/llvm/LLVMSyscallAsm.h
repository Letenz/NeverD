//===- LLVMSyscallAsm.h - Inline asm of a supervisor call -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The LLVM inline assembly a modeled supervisor call lowers to.  The LLVM
/// route emits it and the LLVM-C projection recognizes it, so both build it
/// here from the same convention.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_LLVMSYSCALLASM_H
#define NEVERD_BACKEND_LLVM_LLVMSYSCALLASM_H

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/med/MedSyscallConvention.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ErrorHandling.h"

#include <string>

namespace neverd {

/// The instruction of \p Convention, such as `svc #0`.
inline std::string syscallAsmString(const SyscallConvention &Convention) {
  const char *Mnemonic = intrinsicAsmMnemonic(Convention.Modeled);
  if (!Mnemonic)
    llvm::report_fatal_error("supervisor call convention has no mnemonic");
  return std::string(Mnemonic) + " #" + std::to_string(Convention.Immediate);
}

/// The constraints of \p Convention: the result register as the output, the
/// number and then each argument register as inputs (the argument register
/// the result reuses tied to it), and a memory clobber.
inline std::string syscallAsmConstraints(const SyscallConvention &Convention) {
  const RegNameFn Name = getTargetRegInfo(Convention.TheArch).GetRegName;
  auto Register = [&](uint64_t Offset) {
    const char *Text = Name ? Name(Offset, Convention.RegisterSize) : nullptr;
    if (!Text)
      llvm::report_fatal_error("supervisor call register has no name");
    return "{" + llvm::StringRef(Text).lower() + "}";
  };
  std::string Constraints = "=" + Register(Convention.ResultRegister) + "," +
                            Register(Convention.NumberRegister);
  for (unsigned I = 0; I < Convention.ArgumentCount; ++I)
    Constraints +=
        "," + (Convention.ArgumentRegisters[I] == Convention.ResultRegister
                   ? std::string("0")
                   : Register(Convention.ArgumentRegisters[I]));
  return Constraints + ",~{memory}";
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_LLVMSYSCALLASM_H
