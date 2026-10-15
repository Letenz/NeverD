//===- COFFRegistrationSEHDynamicScope.h - Scope replay --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_COFFREGISTRATIONSEHDYNAMICSCOPE_H
#define NEVERD_COFFREGISTRATIONSEHDYNAMICSCOPE_H

#include "COFFRegistrationIRProof.h"

namespace neverd::coff_registration {
struct RegistrationSEHScopeState {
  bool Enabled = false;
  int64_t Offset = 0;
  std::map<const llvm::StoreInst *, uint16_t> Writes;
  std::set<const llvm::StoreInst *> CallbackWrites;
  std::set<const llvm::CallBase *> PhysicalWrites;
};

llvm::Expected<RegistrationSEHScopeState>
bindDynamicSEHState(const MedFunc &Source, const RegistrationFrame &Frame,
                    llvm::ArrayRef<const llvm::Function *> Functions,
                    const std::set<const llvm::StoreInst *> &RuntimeWrites);

struct DynamicSEHScopeCase {
  const llvm::InvokeInst *Enter;
  const llvm::InvokeInst *Exit;
  const llvm::BasicBlock *Unwind;
};

llvm::Error
validateDynamicSEHScope(const RegistrationBlockState &Source,
                        const RegistrationFrame &Frame,
                        const SourceSegment &Segment,
                        const llvm::CallInst &Enter, const llvm::CallInst &Exit,
                        const std::map<int32_t, DynamicSEHScopeCase> &Cases,
                        std::set<const llvm::CallBase *> &Calls);

llvm::Error
validateDynamicSEHCallMarkers(llvm::ArrayRef<const llvm::Function *> Functions,
                              const std::set<const llvm::CallBase *> &Calls);
} // namespace neverd::coff_registration

#endif // NEVERD_COFFREGISTRATIONSEHDYNAMICSCOPE_H
