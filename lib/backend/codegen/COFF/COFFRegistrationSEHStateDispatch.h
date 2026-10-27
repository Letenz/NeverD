//===- COFFRegistrationSEHStateDispatch.h - State selection ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticates the finite physical updates following a source state store.

#ifndef NEVERD_COFFREGISTRATIONSEHSTATEDISPATCH_H
#define NEVERD_COFFREGISTRATIONSEHSTATEDISPATCH_H

#include "COFFRegistrationIRProof.h"

namespace neverd {
struct X86RegistrationSEHStateStoreBinding;
namespace coff_registration {

struct SEHCallbackStateSequence {
  std::vector<const llvm::CallInst *> Writes;
  const llvm::SwitchInst *Dispatch = nullptr;
  const llvm::Instruction *Resume = nullptr;
};

/// A single result has an adjacent update. Several results require one exact
/// full-word source read, a closed switch and one update on each case edge.
llvm::Expected<SEHCallbackStateSequence>
bindSEHCallbackStateSequence(const llvm::StoreInst &Store,
                             const X86RegistrationSEHStateStoreBinding &Bound,
                             const MedFunc &Source,
                             const llvm::Function &Parent);

} // namespace coff_registration
} // namespace neverd

#endif
