//===- MedLLVMRegistrationCxxBlocks.cpp - PE32 copy block owners ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MedLLVMRegistrationCxxBlocks.h"

#include "MedLLVMRegistrationCopyBlocks.h"

namespace neverd {
std::optional<std::map<llvm::BasicBlock *, X86RegistrationCatchIdentity>>
registrationCxxPHICopyOwners(
    const std::map<int, X86RegistrationCatchIdentity> &Owners,
    const std::map<int, llvm::BasicBlock *> &OriginalBlocks,
    const std::map<llvm::BasicBlock *, std::pair<int, int>> &CopyEdges) {
  return registrationPHICopyOwners(Owners, OriginalBlocks, CopyEdges);
}
} // namespace neverd
