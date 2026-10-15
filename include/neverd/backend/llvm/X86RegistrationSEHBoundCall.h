//===- X86RegistrationSEHBoundCall.h - Preserved SEH calls -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

/// \file
/// Bind checked preserved x86 SEH helpers to their MedIR invocations.

#ifndef NEVERD_BACKEND_LLVM_X86REGISTRATIONSEHBOUNDCALL_H
#define NEVERD_BACKEND_LLVM_X86REGISTRATIONSEHBOUNDCALL_H

#include "neverd/ir/RegistrationState.h"

#include <map>
#include <optional>

namespace llvm {
class FunctionType;
class LLVMContext;
} // namespace llvm

namespace neverd {
struct MedFunc;
struct MedOp;

struct X86RegistrationSEHBoundCall {
  const RegistrationSEHBoundCall *Source = nullptr;
  const MedOp *Operation = nullptr;
  int Block = -1;
};

/// Bind invocation-specific argument recipes to their exact source operations.
std::optional<std::map<va_t, X86RegistrationSEHBoundCall>>
getX86RegistrationSEHBoundCalls(const MedFunc &Function);

llvm::FunctionType *
getX86RegistrationSEHBoundType(const RegistrationSEHBoundCall &Call,
                               llvm::LLVMContext &Context);

} // namespace neverd

#endif
