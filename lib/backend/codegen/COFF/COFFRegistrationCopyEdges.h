//===- COFFRegistrationCopyEdges.h - Private scalar edge copies -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_COFFREGISTRATIONCOPYEDGES_H
#define NEVERD_COFFREGISTRATIONCOPYEDGES_H

#include <cstddef>
#include <set>

namespace llvm {
class BasicBlock;
}
namespace neverd::coff_registration {
class RegistrationCopySlots;
/// Authenticate a single source edge split containing only private register
/// copies. SEH keeps source register spills volatile for asynchronous ordering.
const llvm::BasicBlock *registrationCopyDestination(
    const llvm::BasicBlock &From, const llvm::BasicBlock &Copy,
    const std::set<const llvm::BasicBlock *> &Expected, size_t &Work,
    bool AllowVolatileSlots = false,
    const RegistrationCopySlots *RecoveredSlots = nullptr);
} // namespace neverd::coff_registration

#endif // NEVERD_COFFREGISTRATIONCOPYEDGES_H
