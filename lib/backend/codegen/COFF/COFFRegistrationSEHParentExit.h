//===- COFFRegistrationSEHParentExit.h - Parent transfer proof -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Declares independent proofs for native SEH parent continuations.

#ifndef NEVERD_COFFREGISTRATIONSEHPARENTEXIT_H
#define NEVERD_COFFREGISTRATIONSEHPARENTEXIT_H

#include "COFFRegistrationIRProof.h"

namespace neverd::coff_registration {

struct RegistrationSEHParentExitContract {
  struct Transfer {
    const llvm::BasicBlock *Block = nullptr;
    va_t Callback = 0;
    va_t Address = 0;
  };
  std::map<int, Transfer> Transfers;
  std::map<int, int> Targets;
  std::map<const llvm::BasicBlock *, std::vector<const llvm::BasicBlock *>>
      ResumptionPredecessors;
  const llvm::BasicBlock *NormalEntry = nullptr;
  const llvm::SwitchInst *Dispatch = nullptr;
  std::set<const llvm::Instruction *> Setup;
  std::set<const llvm::Instruction *> ChainReads;
  std::set<const llvm::Instruction *> ChainWrites;
  std::set<std::pair<va_t, int>> SourceChainRemovals;
  std::set<const llvm::CallBase *> Calls;
};

/// Authenticate the complete private machine context and every alias. This
/// checks native structure without granting it a source control-flow identity.
llvm::Expected<RegistrationSEHParentExitContract> bindSEHParentExitContext(
    const llvm::Function &Parent, va_t Owner,
    const std::map<CallbackKey, const llvm::Function *> &Callbacks,
    const std::set<const llvm::LoadInst *> &LocalUnwindChainLoads = {});

/// Bind the checked transfers to independently replayed source instructions.
llvm::Error
bindSEHParentExitSources(RegistrationSEHParentExitContract &Contract,
                         const MedFunc &Source);

llvm::Error validateSEHParentExitTargets(
    const RegistrationSEHParentExitContract &Contract,
    const std::map<int, const llvm::BasicBlock *> &Entries);

} // namespace neverd::coff_registration

#endif // NEVERD_COFFREGISTRATIONSEHPARENTEXIT_H
