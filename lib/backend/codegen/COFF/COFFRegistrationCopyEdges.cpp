//===- COFFRegistrationCopyEdges.cpp - Private scalar edge copies ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationCopyEdges.h"

#include "neverd/Limits.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/Instructions.h"

namespace neverd::coff_registration {
namespace {
bool isPrivateScalarSlot(const llvm::Value *Value, const llvm::Type *Type,
                         size_t &Work, bool AllowVolatileSlots) {
  const auto *Slot = llvm::dyn_cast<llvm::AllocaInst>(Value);
  if (!Slot || !Slot->isStaticAlloca() || !Type->isIntegerTy() ||
      Type->getIntegerBitWidth() > 64 || Slot->getAllocatedType() != Type ||
      Slot->getAddressSpace())
    return false;
  for (const auto *User : Slot->users()) {
    if (++Work > limits::kMaxRegistrationEHStateWork)
      return false;
    if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(User)) {
      if (Load->getPointerOperand() != Slot || Load->getType() != Type ||
          (Load->isVolatile() && !AllowVolatileSlots) || Load->isAtomic())
        return false;
    } else if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(User)) {
      if (Store->getPointerOperand() != Slot ||
          Store->getValueOperand()->getType() != Type ||
          (Store->isVolatile() && !AllowVolatileSlots) || Store->isAtomic())
        return false;
    } else
      return false;
  }
  return true;
}

bool hasPrivateScalarCopies(const llvm::BasicBlock &Block, size_t &Work,
                            bool AllowVolatileSlots) {
  for (const auto &I : Block) {
    if (++Work > limits::kMaxRegistrationEHStateWork)
      return false;
    if (I.isTerminator())
      continue;
    if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I)) {
      if (!isPrivateScalarSlot(Load->getPointerOperand(), Load->getType(), Work,
                               AllowVolatileSlots))
        return false;
    } else if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I)) {
      if (!isPrivateScalarSlot(Store->getPointerOperand(),
                               Store->getValueOperand()->getType(), Work,
                               AllowVolatileSlots))
        return false;
    } else if (!llvm::isa<llvm::TruncInst, llvm::ZExtInst, llvm::SExtInst>(I))
      return false;
  }
  return true;
}
} // namespace

const llvm::BasicBlock *
registrationCopyDestination(const llvm::BasicBlock &From,
                            const llvm::BasicBlock &Copy,
                            const std::set<const llvm::BasicBlock *> &Expected,
                            size_t &Work, bool AllowVolatileSlots) {
  const auto *Branch = llvm::dyn_cast<llvm::UncondBrInst>(Copy.getTerminator());
  if (!Branch || Copy.hasAddressTaken() ||
      Copy.getParent() != From.getParent() ||
      Copy.getUniquePredecessor() != &From ||
      !Expected.count(Branch->getSuccessor(0)) ||
      !hasPrivateScalarCopies(Copy, Work, AllowVolatileSlots))
    return nullptr;
  return Branch->getSuccessor(0);
}
} // namespace neverd::coff_registration
