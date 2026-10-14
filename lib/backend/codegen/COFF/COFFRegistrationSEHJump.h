//===- COFFRegistrationSEHJump.h - Native jump buffer replay ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_COFFREGISTRATIONSEHJUMP_H
#define NEVERD_COFFREGISTRATIONSEHJUMP_H

#include "COFFRegistrationIRProof.h"

namespace neverd::coff_registration {

struct RegistrationSEHJumpContract {
  std::set<const llvm::CallBase *> Calls;
  std::vector<std::pair<int64_t, int64_t>> SourceBuffers;
  bool readsSourceBuffer(int64_t Offset, uint64_t Bytes) const;
};

/// Rebind source calls, private buffers, register snapshots and state restores
/// from actual post-edit IR. The returned calls alone may borrow these buffers.
/// IncomingSetup must come from successful validateIncomingCallerFrame replay;
/// its immutable runtime frame binding is not part of the logical registers.
llvm::Expected<RegistrationSEHJumpContract>
bindSEHJumps(const MedFunc &Source, const llvm::Function &Parent,
             const RegistrationFrame &Frame,
             const std::set<const llvm::Instruction *> &IncomingSetup);

llvm::Error validateSEHJumpUnwindBridge(const llvm::Function &Parent,
                                        va_t SourceUnwind);

} // namespace neverd::coff_registration

#endif // NEVERD_COFFREGISTRATIONSEHJUMP_H
