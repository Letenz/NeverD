//===- COFFRegistrationSEHState.h - Callback state proof -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate physical callback state writes and otherwise unused scope rows.

#ifndef NEVERD_COFFREGISTRATIONSEHSTATE_H
#define NEVERD_COFFREGISTRATIONSEHSTATE_H

#include "COFFRegistrationIRProof.h"

namespace neverd::coff_registration {

struct SEHCallbackStateWrites {
  std::set<const llvm::CallBase *> Writes;
  std::set<const llvm::StoreInst *> Stores;
};

/// Source segments must already bind each store's owner and occurrence order.
llvm::Expected<SEHCallbackStateWrites>
bindSEHCallbackStateWrites(const MedFunc &Source, const llvm::Function &Parent,
                           const std::map<int, SourceSegment> &Segments,
                           llvm::ArrayRef<const llvm::Function *> Functions);

/// Verify the exact entry chain retaining callback-selected scopes. Each
/// donothing edge keeps a table row reachable without activating its state.
llvm::Expected<std::set<const llvm::InvokeInst *>> validateSEHRetainedScopes(
    const MedFunc &Source, const llvm::Function &Parent,
    const llvm::BasicBlock &SourceEntry,
    const std::map<uint32_t, const llvm::BasicBlock *> &Unwinds,
    const llvm::BasicBlock *CapturedEntry = nullptr);

} // namespace neverd::coff_registration

#endif
