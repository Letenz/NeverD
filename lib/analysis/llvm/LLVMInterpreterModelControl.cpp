//===- LLVMInterpreterModelControl.cpp - Scalar LLVM model ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "LLVMInterpreterModelInternal.h"

namespace neverd::analysis::llvm_model {
bool Builder::emitControl(LowBlock &Out, const llvm::Instruction &I) {
  if (auto *Switch = llvm::dyn_cast<llvm::SwitchInst>(&I)) {
    auto &Dispatch = SwitchBlocks.at(Switch);
    int Default = target(I.getParent(), Switch->getDefaultDest());
    unsigned Index = 0;
    for (auto Case : Switch->cases()) {
      auto &D = Result.Function.Blocks[Dispatch[Index]];
      int Yes = target(I.getParent(), Case.getCaseSuccessor());
      int No =
          Index + 1 < Switch->getNumCases() ? Dispatch[Index + 1] : Default;
      if (Yes == No) {
        D.Succs = {Yes};
        emit(D, op(NdOp::BRANCH, {}, {num(address(Yes))}));
      } else {
        auto Condition = local(1);
        emit(D,
             op(NdOp::INT_EQUAL, Condition,
                {value(Switch->getCondition()), value(Case.getCaseValue())}));
        D.Succs = {Yes, No};
        emit(D, op(NdOp::COND_BR, {}, {num(address(Yes)), Condition}));
      }
      ++Index;
    }
    if (!Index) {
      Out.Succs = {Default};
      emit(Out, op(NdOp::BRANCH, {}, {num(address(Default))}));
    }
  } else if (llvm::isa<llvm::UncondBrInst, llvm::CondBrInst>(I)) {
    int T = target(I.getParent(), I.getSuccessor(0));
    Out.Succs = {T};
    if (auto *Br = llvm::dyn_cast<llvm::CondBrInst>(&I)) {
      int E = target(I.getParent(), I.getSuccessor(1));
      if (E == T) {
        emit(Out, op(NdOp::BRANCH, {}, {num(address(T))}));
        return true;
      }
      Out.Succs.push_back(E);
      emit(Out,
           op(NdOp::COND_BR, {}, {num(address(T)), value(Br->getCondition())}));
    } else
      emit(Out, op(NdOp::BRANCH, {}, {num(address(T))}));
  } else if (auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(&I)) {
    auto *V = Ret->getReturnValue();
    if (!V || (ScalarArguments ? !V->getType()->isIntegerTy()
                               : !V->getType()->isIntegerTy(64)))
      fail("invalid machine-state status return");
    auto Attr =
        F.getAttributes().getRetAttrs().getAttribute(llvm::Attribute::Range);
    if (Attr.isValid())
      requireRange(Out, value(V), Attr.getRange());
    emit(Out, op(NdOp::RETURN, {}, {value(V)}));
  } else
    return false;
  return true;
}

} // namespace neverd::analysis::llvm_model
