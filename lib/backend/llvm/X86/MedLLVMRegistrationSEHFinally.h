//===- MedLLVMRegistrationSEHFinally.h - Nested finally ABI ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Projects checked ordinary calls between outlined finally invocations.

#ifndef NEVERD_BACKEND_LLVM_X86_MEDLLVMREGISTRATIONSEHFINALLY_H
#define NEVERD_BACKEND_LLVM_X86_MEDLLVMREGISTRATIONSEHFINALLY_H

#include "neverd/Common.h"
#include "neverd/backend/llvm/WindowsRegistrationFrame.h"

#include "llvm/ADT/ArrayRef.h"

namespace llvm {
class CallInst;
class Function;
} // namespace llvm

namespace neverd::x86_registration {
struct SEHNestedFinallyCall {
  const llvm::CallInst *Original;
  va_t Address;
  va_t Target;
  size_t CallerIndex;
  size_t CalleeIndex;
  bool NoReturn;
  std::vector<llvm::Instruction *> DiscardedResults;
};

/// Commit preflighted calls using the exact batch outliner's clone identities.
void emitSEHNestedFinallyCalls(
    llvm::ArrayRef<SEHNestedFinallyCall> Calls,
    llvm::ArrayRef<llvm::Function *> Callbacks,
    llvm::ArrayRef<X86RegistrationCallbackInstructionMap> InstructionMaps,
    llvm::Function &Parent, va_t ParentVA);
} // namespace neverd::x86_registration

#endif
