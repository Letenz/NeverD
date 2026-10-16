//===- COFFRegistrationCopyEdges.cpp - Private scalar edge copies ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationCopyEdges.h"

#include "COFFRegistrationCopySlots.h"

#include "neverd/Limits.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"

namespace neverd::coff_registration {
namespace {
// Undefined handler registers can feed unused PHIs. Jump snapshots keep those
// slots syntactically live, so local dead-instruction removal cannot discard
// their copy cycles. Admit a frozen poison only when its entire scalar
// transport graph has no observer, including through private snapshot slots.
bool isUnobservedPoisonCopy(const llvm::FreezeInst &Freeze, size_t &Work,
                            bool AllowVolatileSlots,
                            const RegistrationCopySlots &Slots) {
  if (!Freeze.getType()->isIntegerTy() ||
      Freeze.getType()->getIntegerBitWidth() > 64 ||
      !llvm::isa<llvm::PoisonValue>(Freeze.getOperand(0)))
    return false;
  llvm::SmallVector<const llvm::Value *, 16> Pending{&Freeze};
  llvm::SmallPtrSet<const llvm::Value *, 32> Seen;
  while (!Pending.empty()) {
    const auto *Value = Pending.pop_back_val();
    if (++Work > limits::kMaxRegistrationEHStateWork)
      return false;
    if (!Seen.insert(Value).second)
      continue;
    for (const auto *User : Value->users()) {
      const auto *I = llvm::dyn_cast<llvm::Instruction>(User);
      if (++Work > limits::kMaxRegistrationEHStateWork || !I ||
          I->getModule() != Freeze.getModule())
        return false;
      if (Slots.isStorage(Value) && Slots.isEscape(I))
        continue;
      if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(I)) {
        if (Store->getPointerOperand() == Value && Slots.isStorage(Value))
          continue;
        if (Store->getValueOperand() != Value ||
            !Slots.isPrivate(Store->getPointerOperand(), Value->getType(), Work,
                             AllowVolatileSlots))
          return false;
        for (const auto *Alias : Slots.aliases(Store->getPointerOperand()))
          Pending.push_back(Alias);
      } else if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(I)) {
        if (Load->getPointerOperand() != Value || !Slots.isStorage(Value))
          return false;
        Pending.push_back(Load);
      } else if (llvm::isa<llvm::TruncInst, llvm::ZExtInst, llvm::SExtInst,
                           llvm::FreezeInst>(I)) {
        if (!I->getType()->isIntegerTy() ||
            I->getType()->getIntegerBitWidth() > 64)
          return false;
        Pending.push_back(I);
      } else {
        return false;
      }
    }
  }
  return true;
}

bool hasPrivateScalarCopies(const llvm::BasicBlock &Block, size_t &Work,
                            bool AllowVolatileSlots,
                            const RegistrationCopySlots &Slots) {
  for (const auto &I : Block) {
    if (++Work > limits::kMaxRegistrationEHStateWork)
      return false;
    if (I.isTerminator())
      continue;
    if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I)) {
      if (!Slots.isPrivate(Load->getPointerOperand(), Load->getType(), Work,
                           AllowVolatileSlots))
        return false;
    } else if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I)) {
      if (!Slots.isPrivate(Store->getPointerOperand(),
                           Store->getValueOperand()->getType(), Work,
                           AllowVolatileSlots))
        return false;
    } else if (const auto *Freeze = llvm::dyn_cast<llvm::FreezeInst>(&I)) {
      if (!isUnobservedPoisonCopy(*Freeze, Work, AllowVolatileSlots, Slots))
        return false;
    } else if (!llvm::isa<llvm::TruncInst, llvm::ZExtInst, llvm::SExtInst>(I))
      return false;
  }
  return true;
}
} // namespace

const llvm::BasicBlock *registrationCopyDestination(
    const llvm::BasicBlock &From, const llvm::BasicBlock &Copy,
    const std::set<const llvm::BasicBlock *> &Expected, size_t &Work,
    bool AllowVolatileSlots, const RegistrationCopySlots *RecoveredSlots) {
  const RegistrationCopySlots LocalSlots;
  const auto &Slots = RecoveredSlots ? *RecoveredSlots : LocalSlots;
  const auto *Branch = llvm::dyn_cast<llvm::UncondBrInst>(Copy.getTerminator());
  if (!Branch || Copy.hasAddressTaken() ||
      Copy.getParent() != From.getParent() ||
      Copy.getUniquePredecessor() != &From ||
      !Expected.count(Branch->getSuccessor(0)) ||
      !hasPrivateScalarCopies(Copy, Work, AllowVolatileSlots, Slots))
    return nullptr;
  return Branch->getSuccessor(0);
}
} // namespace neverd::coff_registration
