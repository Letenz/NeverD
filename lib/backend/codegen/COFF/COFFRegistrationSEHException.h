//===- COFFRegistrationSEHException.h - SEH runtime objects -------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_COFFREGISTRATIONSEHEXCEPTION_H
#define NEVERD_COFFREGISTRATIONSEHEXCEPTION_H

#include "COFFRegistrationIRProof.h"

namespace neverd::coff_registration {

/// Independently replayed source reads and the already authenticated runtime
/// bridge for each filter. Pointer roots are distinct for EXCEPTION_POINTERS
/// and EXCEPTION_RECORD and cannot be shared by different filter invocations.
struct RegistrationSEHExceptionContract {
  struct Access {
    RegistrationSEHExceptionAccess::Kind Field;
    const llvm::StoreInst *Bridge;
  };
  const llvm::AllocaInst *Frame = nullptr;
  int64_t PointersOffset = 0;
  std::map<const llvm::LoadInst *, Access> Reads;
  std::map<const llvm::Function *, const llvm::StoreInst *> Bridges;
  std::set<const llvm::Value *> Roots;
  /// Literal null reads retain their source memory effects, including faults.
  /// Their exact occurrence, pointer, width and ordering are checked at bind.
  std::set<const llvm::LoadInst *> NullReads;

  const llvm::Value *pointerResult(const llvm::LoadInst &Load) const;
  bool validRead(const llvm::LoadInst &Load, const llvm::Value *Root,
                 int64_t Offset) const;
  bool clobbersPointers(const llvm::StoreInst &Store, const llvm::Value *Root,
                        int64_t Offset, uint64_t Size) const;
};

llvm::Expected<std::set<const llvm::LoadInst *>>
bindSEHNullReads(const MedFunc &Source,
                 llvm::ArrayRef<const llvm::Function *> Functions);

llvm::Expected<RegistrationSEHExceptionContract> bindSEHExceptionReads(
    const MedFunc &Source, const RegistrationFrame &Frame,
    const std::map<CallbackKey, const llvm::Function *> &Callbacks,
    const std::map<const llvm::Function *, const llvm::StoreInst *> &Bridges,
    llvm::ArrayRef<const llvm::Function *> Functions);

} // namespace neverd::coff_registration

#endif // NEVERD_COFFREGISTRATIONSEHEXCEPTION_H
