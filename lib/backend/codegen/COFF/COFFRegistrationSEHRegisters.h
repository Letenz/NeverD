//===- COFFRegistrationSEHRegisters.h - Register exchange proof -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent private-storage and source-order checks for finally registers.

#ifndef NEVERD_COFFREGISTRATIONSEHREGISTERS_H
#define NEVERD_COFFREGISTRATIONSEHREGISTERS_H

#include "COFFRegistrationIRProof.h"

namespace neverd::coff_registration {
bool matchesSEHRegisterExchange(const llvm::Instruction &Instruction,
                                const MedOp &Source, va_t Owner);
llvm::Error validateSEHRegisterStorage(
    const llvm::Function &Parent, const MedFunc &Source,
    const std::map<CallbackKey, const llvm::Function *> &Callbacks);
} // namespace neverd::coff_registration

#endif // NEVERD_COFFREGISTRATIONSEHREGISTERS_H
