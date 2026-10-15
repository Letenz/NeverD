//===- MedLLVMRegistrationSEHRuntimeCallback.cpp - Callback imports ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Rebind checked runtime arguments to the outlined callback's private stack.

#include "MedLLVMRegistrationSEHJump.h"

#include "neverd/backend/llvm/WindowsEHMetadata.h"

#include "llvm/IR/Function.h"
#include "llvm/Transforms/Utils/Local.h"

namespace neverd::x86_registration {

void bindSEHRuntimeCallbackArguments(
    const SEHJumpPlan &Plan,
    const std::map<va_t, llvm::Function *> &Callbacks) {
  for (const auto &[Original, Bound] : Plan.BoundCalls) {
    const auto &Source = *Bound.Source;
    if (!Source.EntryVA)
      continue;
    auto *Callback = Callbacks.at(Source.EntryVA);
    const auto Stack = getX86RegistrationSEHJumpStack(*Callback);
    assert(Stack && "outlined callback must retain its checked private stack");
    llvm::CallInst *Call = nullptr;
    for (auto &Block : *Callback)
      for (auto &I : Block)
        if (auto *Candidate = llvm::dyn_cast<llvm::CallInst>(&I);
            Candidate &&
            Candidate->getMetadata(
                windows_eh_md::RegistrationOperationAttachment) ==
                Original->getMetadata(
                    windows_eh_md::RegistrationOperationAttachment)) {
          assert(!Call && "runtime source occurrence has one callback clone");
          Call = Candidate;
        }
    assert(Call && "outlining must retain the checked runtime call");
    for (size_t I = 0; I != Source.Arguments.size(); ++I) {
      const auto &Argument = Source.Arguments[I];
      if (Argument.ArgumentKind != RegistrationSEHBoundArgument::Kind::Scalar)
        continue;
      auto *Load = llvm::cast<llvm::LoadInst>(Call->getArgOperand(I));
      auto *OldPointer =
          llvm::dyn_cast<llvm::Instruction>(Load->getPointerOperand());
      llvm::IRBuilder<> Builder(Load);
      auto *Pointer = Builder.CreateInBoundsGEP(
          Builder.getInt8Ty(), const_cast<llvm::AllocaInst *>(Stack->Slot),
          Builder.getInt32(int64_t(Stack->EntrySP) + Argument.Offset));
      Load->setOperand(0, Pointer);
      if (OldPointer)
        llvm::RecursivelyDeleteTriviallyDeadInstructions(OldPointer);
    }
  }
}

} // namespace neverd::x86_registration
