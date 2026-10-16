//===- MedLLVMRegistrationSEHRegisters.h - Finally registers --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Binds private ordinary-call inputs after runtime callback outlining.

#ifndef NEVERD_BACKEND_LLVM_X86_REGISTRATIONSEHREGISTERS_H
#define NEVERD_BACKEND_LLVM_X86_REGISTRATIONSEHREGISTERS_H

#include "llvm/ADT/ArrayRef.h"

namespace llvm {
class Function;
}

namespace neverd::x86_registration {
void bindSEHRegisterCallbackInputs(llvm::ArrayRef<llvm::Function *> Callbacks);
} // namespace neverd::x86_registration

#endif // NEVERD_BACKEND_LLVM_X86_REGISTRATIONSEHREGISTERS_H
