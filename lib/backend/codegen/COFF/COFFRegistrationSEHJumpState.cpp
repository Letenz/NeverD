//===- COFFRegistrationSEHJumpState.cpp - Resumed state replay -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independently checks logical state projection after a native capture.

#include "COFFRegistrationSEHJump.h"

#include "neverd/ir/med/X86RegistrationSEHJump.h"

#include "llvm/IR/Operator.h"

namespace neverd::coff_registration {

llvm::Error
validateSEHJumpState(const X86RegistrationSEHJumpCall &Expected,
                     const llvm::CallBase &Call, const llvm::StoreInst &State,
                     const RegistrationFrame &Frame,
                     std::set<const llvm::Instruction *> &Restores) {
  const auto *GEP =
      llvm::dyn_cast<llvm::GetElementPtrInst>(State.getPointerOperand());
  llvm::APInt Offset(32, 0);
  const auto ResumeLevel = Expected.ResumeLevel;
  const llvm::Value *Initial = State.getValueOperand();
  if (!ResumeLevel || *ResumeLevel != Expected.Capture->Level) {
    const auto *Select = llvm::dyn_cast<llvm::SelectInst>(Initial);
    const auto *Compare =
        Select ? llvm::dyn_cast<llvm::ICmpInst>(Select->getCondition())
               : nullptr;
    if (!Select || !Compare || Compare->hasPoisonGeneratingFlags() ||
        !Select->hasOneUse() || !Compare->hasOneUse() ||
        Compare->getPredicate() != llvm::ICmpInst::ICMP_EQ ||
        Compare->getOperand(0) != &Call ||
        integer(Compare->getOperand(1), 32) != 0)
      return rejectIR("native jump changed its resumed source try level");
    if (ResumeLevel) {
      if (integer(Select->getFalseValue(), 32) != uint32_t(*ResumeLevel))
        return rejectIR("native jump changed its resumed source try level");
    } else {
      const llvm::Value *Projected = Select->getFalseValue();
      std::vector<const llvm::ICmpInst *> Tests;
      for (auto It = Expected.ResumeStateMap.rbegin();
           It != Expected.ResumeStateMap.rend(); ++It) {
        const auto *Step = llvm::dyn_cast<llvm::SelectInst>(Projected);
        const auto *Test =
            Step ? llvm::dyn_cast<llvm::ICmpInst>(Step->getCondition())
                 : nullptr;
        if (!Step || !Test || !Step->hasOneUse() || !Test->hasOneUse() ||
            Test->hasPoisonGeneratingFlags() ||
            Test->getPredicate() != llvm::ICmpInst::ICMP_EQ ||
            integer(Test->getOperand(1), 32) != uint32_t(It->first) ||
            integer(Step->getTrueValue(), 32) != uint32_t(It->second))
          return rejectIR("native jump changed its unwind state projection");
        Tests.push_back(Test);
        Restores.insert({Step, Test});
        Projected = Step->getFalseValue();
      }
      const auto *Current = llvm::dyn_cast<llvm::LoadInst>(Projected);
      if (!Current || Current->getNumUses() != Tests.size() + 1 ||
          !Current->isVolatile() || Current->isAtomic() ||
          Current->getAlign() != llvm::Align(1) ||
          Current->getPointerOperand() != State.getPointerOperand())
        return rejectIR("native jump changed its current source state read");
      for (const auto *Test : Tests)
        if (Test->getOperand(0) != Current)
          return rejectIR("native jump projection changed its observed state");
      Restores.insert(Current);
    }
    Initial = Select->getTrueValue();
    Restores.insert({Compare, Select});
  }
  if (!GEP || GEP->getPointerOperand() != Frame.Slot || Frame.EntrySP < 8 ||
      !GEP->accumulateConstantOffset(Call.getModule()->getDataLayout(),
                                     Offset) ||
      Offset.getZExtValue() != Frame.EntrySP - 8 ||
      integer(Initial, 32) != uint32_t(Expected.Capture->Level) ||
      State.isAtomic() || !State.isVolatile() ||
      State.getAlign() != llvm::Align(1))
    return rejectIR("native jump changed its restored source try level");
  Restores.insert({GEP, &State});
  return llvm::Error::success();
}

} // namespace neverd::coff_registration
