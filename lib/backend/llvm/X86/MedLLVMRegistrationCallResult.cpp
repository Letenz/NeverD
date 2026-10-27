//===- MedLLVMRegistrationCallResult.cpp - PE32 dead results
//---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind source-unused results to removable emitter-private LLVM transports.
//===----------------------------------------------------------------------===//

#include "MedLLVMRegistrationCallResult.h"

#include "neverd/Limits.h"
#include "neverd/backend/Limits.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/Instructions.h"

#include <functional>

namespace neverd::med_llvm_eh {
std::optional<llvm::SmallVector<llvm::Instruction *, 8>>
unusedRegistrationCallResultPlan(
    llvm::CallInst &Call, llvm::ArrayRef<llvm::AllocaInst *> PrivateSlots,
    size_t &Work) {
  llvm::SmallPtrSet<const llvm::AllocaInst *, 32> Slots(PrivateSlots.begin(),
                                                        PrivateSlots.end());
  llvm::SmallPtrSet<const llvm::Value *, 32> Seen;
  llvm::SmallVector<llvm::Instruction *, 8> Erase;
  auto Charge = [&](size_t Amount) {
    if (Work > limits::kMaxRegistrationEHStateWork ||
        Amount > limits::kMaxRegistrationEHStateWork - Work) {
      Work = limits::kMaxRegistrationEHStateWork;
      return false;
    }
    Work += Amount;
    return true;
  };
  std::function<bool(llvm::Value &, unsigned)> Visit = [&](llvm::Value &Value,
                                                           unsigned Depth) {
    if (!Charge(1 + Value.getNumUses()) ||
        Depth >
            ::neverd::limits::backend::
                kMedLLVMRegistrationCallResultUnusedRegistrationCallResultPlanVisitDepthLimit)
      return false;
    if (!Seen.insert(&Value).second)
      return true;
    for (auto *User : Value.users()) {
      auto *Instruction = llvm::dyn_cast<llvm::Instruction>(User);
      if (!Instruction || Instruction->getFunction() != Call.getFunction() ||
          Instruction->hasMetadata())
        return false;
      if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(Instruction)) {
        const auto *Slot =
            llvm::dyn_cast<llvm::AllocaInst>(Store->getPointerOperand());
        if (!Slot || !Slots.contains(Slot) || !Store->isSimple() ||
            Store->getValueOperand() != &Value || !Charge(Slot->getNumUses()))
          return false;
        for (const auto *Use : Slot->users()) {
          const auto *Write = llvm::dyn_cast<llvm::StoreInst>(Use);
          if (!Write || Write->getPointerOperand() != Slot ||
              Write->getValueOperand() == Slot || !Write->isSimple() ||
              Write->hasMetadata())
            return false;
        }
      } else {
        if ((!llvm::isa<llvm::TruncInst>(Instruction) &&
             !llvm::isa<llvm::ZExtInst>(Instruction) &&
             !llvm::isa<llvm::SExtInst>(Instruction)) ||
            !Visit(*Instruction, Depth + 1))
          return false;
      }
      if (!llvm::is_contained(Erase, Instruction))
        Erase.push_back(Instruction);
    }
    return true;
  };
  if (!Visit(Call, 0))
    return std::nullopt;
  return Erase;
}
} // namespace neverd::med_llvm_eh
