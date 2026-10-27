//===- COFFRegistrationSEHDynamicScope.cpp - Dynamic scope replay --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationSEHDynamicScope.h"

#include "COFFRegistrationSEHCall.h"

#include "llvm/IR/CFG.h"

namespace neverd::coff_registration {

llvm::Error
validateDynamicSEHScope(const RegistrationBlockState &Source,
                        const RegistrationFrame &Frame,
                        const SourceSegment &Segment,
                        const llvm::CallInst &Enter, const llvm::CallInst &Exit,
                        const std::map<int32_t, DynamicSEHScopeCase> &Cases,
                        std::set<const llvm::CallBase *> &Calls) {
  const auto *Entry = Enter.getParent();
  const auto *Body = Segment.Enter->getParent();
  const auto *After = Exit.getParent();
  const auto *SelectEntry =
      llvm::dyn_cast<llvm::SwitchInst>(Entry->getTerminator());
  const auto *SelectExit =
      llvm::dyn_cast<llvm::SwitchInst>(Body->getTerminator());
  const auto *Level =
      SelectEntry ? llvm::dyn_cast<llvm::LoadInst>(SelectEntry->getCondition())
                  : nullptr;
  const auto *Address =
      Level
          ? llvm::dyn_cast<llvm::GetElementPtrInst>(Level->getPointerOperand())
          : nullptr;
  llvm::APInt Offset(32, 0);
  if (!SelectEntry || !SelectExit || !Level || !Address ||
      &*Entry->getFirstNonPHIIt() != &Enter ||
      next(Enter)->getNextNode() != Address || next(*Address) != Level ||
      next(*Level) != SelectEntry || !Level->hasNUses(2) ||
      !Address->hasOneUse() || Level->isAtomic() || !Level->isVolatile() ||
      Level->getAlign() != llvm::Align(1) ||
      !Level->getType()->isIntegerTy(32) ||
      Address->getPointerOperand() != Frame.Slot || Frame.EntrySP < 8 ||
      !Address->accumulateConstantOffset(Entry->getModule()->getDataLayout(),
                                         Offset) ||
      Offset.getZExtValue() != Frame.EntrySP - 8 ||
      SelectExit->getCondition() != Level ||
      Segment.Exit->getParent() != Body || next(*Segment.Exit) != SelectExit ||
      After->size() != 2 || &After->front() != &Exit)
    return rejectIR("dynamic SEH scope changed its private state selector");
  const bool Inactive =
      llvm::any_of(Source.levels(), [](int32_t L) { return L < 0; });
  if (SelectEntry->getNumCases() != Cases.size() ||
      SelectExit->getNumCases() != Cases.size() ||
      Cases.size() + unsigned(Inactive) != Source.levels().size())
    return rejectIR("dynamic SEH scope has an incomplete state choice");
  if (Inactive) {
    if (SelectEntry->getDefaultDest() != Body ||
        SelectExit->getDefaultDest() != After)
      return rejectIR("dynamic SEH scope changed its inactive path");
  } else {
    const auto *Invalid = SelectEntry->getDefaultDest();
    if (SelectExit->getDefaultDest() != Invalid || Invalid->size() != 1 ||
        !llvm::isa<llvm::UnreachableInst>(Invalid->getTerminator()))
      return rejectIR("dynamic SEH scope admits an unowned state");
  }
  std::set<const llvm::BasicBlock *> BodyPreds, ExitPreds;
  if (Inactive) {
    BodyPreds.insert(Entry);
    ExitPreds.insert(Body);
  }
  for (const auto &[State, Choice] : Cases) {
    const auto *Begin = Choice.Enter;
    const auto *End = Choice.Exit;
    if (State < 0 || !llvm::is_contained(Source.levels(), State) || !Begin ||
        !End || Begin->getParent()->size() != 2 ||
        Begin->getParent()->getSinglePredecessor() != Entry ||
        Begin->getNormalDest() != Body ||
        Begin->getUnwindDest() != Choice.Unwind ||
        End->getParent()->size() != 2 ||
        End->getParent()->getSinglePredecessor() != Body ||
        End->getNormalDest() != After || End->getUnwindDest() != Choice.Unwind)
      return rejectIR("dynamic SEH scope bypasses a checked state boundary");
    auto *Value = llvm::ConstantInt::get(
        llvm::Type::getInt32Ty(Enter.getContext()), State);
    const auto EntryCase = SelectEntry->findCaseValue(Value);
    const auto ExitCase = SelectExit->findCaseValue(Value);
    if (EntryCase == SelectEntry->case_default() ||
        ExitCase == SelectExit->case_default() ||
        EntryCase->getCaseSuccessor() != Begin->getParent() ||
        ExitCase->getCaseSuccessor() != End->getParent())
      return rejectIR("dynamic SEH scope routes a source state incorrectly");
    BodyPreds.insert(Begin->getParent());
    ExitPreds.insert(End->getParent());
  }
  for (const auto *Pred : llvm::predecessors(Body))
    if (!BodyPreds.erase(Pred))
      return rejectIR("dynamic SEH body has an independent incoming edge");
  for (const auto *Pred : llvm::predecessors(After))
    if (!ExitPreds.erase(Pred))
      return rejectIR("dynamic SEH exit has an independent incoming edge");
  if (!BodyPreds.empty() || !ExitPreds.empty())
    return rejectIR("dynamic SEH scope has an incomplete execution boundary");
  for (const auto &I : *Body) {
    if (llvm::isa<llvm::PHINode>(I))
      return rejectIR("dynamic SEH body selects a source predecessor too late");
    const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
    if (!Call || llvm::isa<llvm::IntrinsicInst>(Call))
      continue;
#ifndef LLVM_NEVERD_X86_REGISTRATION_SEH_DYNAMIC_CALLS
    return rejectIR("LLVM cannot preserve a dynamic SEH call state");
#else
    const auto *CI = llvm::dyn_cast<llvm::CallInst>(Call);
    if (!isDynamicSEHCall(*Call) || !Segment.Enter->comesBefore(CI) ||
        !CI->comesBefore(Segment.Exit) || !Calls.insert(CI).second)
      return rejectIR("dynamic SEH body contains an unbound throwing call");
#endif
  }
  return llvm::Error::success();
}

llvm::Error
validateDynamicSEHCallMarkers(llvm::ArrayRef<const llvm::Function *> Functions,
                              const std::set<const llvm::CallBase *> &Calls) {
#ifdef LLVM_NEVERD_X86_REGISTRATION_SEH_DYNAMIC_CALLS
  const auto Attribute =
      llvm::mc_rewrite::RewriteWinX86SEHInheritStateAttribute;
  for (const auto *Function : Functions) {
    if (Function->hasFnAttribute(Attribute))
      return rejectIR("dynamic SEH call marker belongs to a function");
    for (const auto &Block : *Function)
      for (const auto &I : Block) {
        const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
        if (!Call)
          continue;
        if ((Call->getAttributes().hasFnAttr(Attribute) &&
             !Calls.count(Call)) ||
            (Call->getCalledFunction() &&
             Call->getCalledFunction()->hasFnAttribute(Attribute)))
          return rejectIR("dynamic SEH call marker has no checked scope body");
      }
  }
#endif
  return llvm::Error::success();
}

} // namespace neverd::coff_registration
