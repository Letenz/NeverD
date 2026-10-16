//===- RegistrationSEHFinallyIRMutations.cpp - Finally call edits ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Replay immutable source after changing parent and nested finally calls.

#include "RegistrationSEHFinallyIRMutations.h"

#include "gtest/gtest.h"

#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"

namespace neverd::registration_test {

void checkSEHFinallyEdits(const llvm::Function &Parent,
                          const ExceptionFunction &Source,
                          const BinaryImage &Image) {
#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
  struct Site {
    std::string Function;
    va_t Address;
  };
  std::vector<Site> Sites;
  for (const auto &Function : *Parent.getParent())
    for (const auto &Block : Function)
      for (const auto &I : Block)
        if (const auto *MD = I.getMetadata(
                windows_eh_md::RegistrationFinallyCallAttachment)) {
          const auto *Owner =
              llvm::mdconst::dyn_extract<llvm::ConstantInt>(MD->getOperand(0));
          const auto *Address =
              llvm::mdconst::dyn_extract<llvm::ConstantInt>(MD->getOperand(1));
          ASSERT_TRUE(Owner && Address);
          if (Owner->getZExtValue() == Source.CodeRange.Begin)
            Sites.push_back(
                {Function.getName().str(), Address->getZExtValue()});
        }
  for (const auto &Site : Sites)
    for (unsigned Mutation = 0; Mutation != 17; ++Mutation) {
      SCOPED_TRACE(Site.Function);
      SCOPED_TRACE(Site.Address);
      SCOPED_TRACE(Mutation);
      auto Module = llvm::CloneModule(*Parent.getParent());
      auto *Function = Module->getFunction(Parent.getName());
      auto *Caller = Module->getFunction(Site.Function);
      ASSERT_TRUE(Function && Caller);
      llvm::CallBase *Call = nullptr;
      for (auto &Block : *Caller)
        for (auto &I : Block)
          if (const auto *MD = I.getMetadata(
                  windows_eh_md::RegistrationFinallyCallAttachment)) {
            const auto *Address = llvm::mdconst::dyn_extract<llvm::ConstantInt>(
                MD->getOperand(1));
            if (Address && Address->getZExtValue() == Site.Address) {
              ASSERT_FALSE(Call);
              Call = llvm::dyn_cast<llvm::CallBase>(&I);
            }
          }
      ASSERT_TRUE(Call);
      llvm::IRBuilder<> B(Call);
      auto EditMarker = [&](llvm::StringRef Name, unsigned Field) {
        const auto *MD = Call->getMetadata(Name);
        llvm::SmallVector<llvm::Metadata *, 5> Values;
        for (const auto &Value : MD->operands())
          Values.push_back(Value.get());
        const auto *Old =
            llvm::mdconst::extract<llvm::ConstantInt>(Values[Field]);
        Values[Field] = llvm::ConstantAsMetadata::get(
            llvm::ConstantInt::get(Old->getType(), Old->getZExtValue() + 1));
        Call->setMetadata(Name,
                          llvm::MDNode::get(Module->getContext(), Values));
      };
      switch (Mutation) {
      case 0:
        Call->setMetadata(windows_eh_md::RegistrationFinallyCallAttachment,
                          nullptr);
        break;
      case 1:
        Call->setArgOperand(0, B.getInt8(1));
        break;
      case 2:
        Call->setArgOperand(1, llvm::ConstantPointerNull::get(B.getPtrTy()));
        break;
      case 3:
        Call->setCallingConv(llvm::CallingConv::X86_StdCall);
        break;
      case 4:
        Call->addFnAttr(llvm::Attribute::NoUnwind);
        break;
      case 5:
        Call->addFnAttr(llvm::Attribute::NoCallback);
        break;
      case 6:
        EditMarker(windows_eh_md::RegistrationFinallyCallAttachment, 2);
        break;
      case 7:
        EditMarker(windows_eh_md::RegistrationFinallyCallAttachment, 1);
        break;
      case 8:
        EditMarker(windows_eh_md::RegistrationOperationAttachment, 2);
        break;
      case 9:
        EditMarker(windows_eh_md::RegistrationOperationAttachment, 3);
        break;
      case 10: {
        auto *Duplicate =
            B.CreateCall(Call->getCalledFunction(),
                         {Call->getArgOperand(0), Call->getArgOperand(1)});
        Duplicate->copyMetadata(*Call);
        break;
      }
      case 11: {
        auto *Other = llvm::Function::Create(Call->getFunctionType(),
                                             llvm::GlobalValue::ExternalLinkage,
                                             "changed.finally", *Module);
        Call->setCalledFunction(Other);
        break;
      }
      case 12:
        Call->addParamAttr(0, llvm::Attribute::NoUndef);
        break;
      case 13:
        if (auto *Ordinary = llvm::dyn_cast<llvm::CallInst>(Call))
          Ordinary->setTailCallKind(llvm::CallInst::TCK_Tail);
        else
          Call->addFnAttr(llvm::Attribute::WillReturn);
        break;
      case 14:
        if (Call->doesNotReturn()) {
          if (Caller == Function)
            continue;
          Call->removeFnAttr(llvm::Attribute::NoReturn);
        } else
          Call->addFnAttr(llvm::Attribute::NoReturn);
        break;
      case 15: {
        if (Caller == Function)
          continue;
        // Keep a valid destination frame and ABI while moving the marked
        // operation out of its source callback. Reject its changed owner,
        // rather than an independently invalid null parent-frame argument.
        ASSERT_TRUE(llvm::isa<llvm::CallInst>(Call));
        auto *Destination = Function->getEntryBlock().getTerminator();
        llvm::IRBuilder<> DestinationBuilder(Destination);
        auto *Frame = DestinationBuilder.CreateCall(
            llvm::Intrinsic::getOrInsertDeclaration(
                Module.get(), llvm::Intrinsic::localaddress));
        Call->setArgOperand(1, Frame);
        Call->moveBefore(Destination->getIterator());
        break;
      }
      case 16: {
        if (!Call->doesNotReturn() || Caller == Function)
          continue;
        auto *Callee = Call->getCalledFunction();
        ASSERT_TRUE(Callee && Callee->getReturnType()->isVoidTy());
        auto *Terminator = Callee->back().getTerminator();
        ASSERT_TRUE(llvm::isa<llvm::UnreachableInst>(Terminator));
        llvm::ReturnInst::Create(Module->getContext(), nullptr, Terminator);
        Terminator->eraseFromParent();
        break;
      }
      }
      ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
      auto Error = validateCOFFRegistrationIR(*Function, Source, Image);
      ASSERT_TRUE(bool(Error));
      llvm::consumeError(std::move(Error));
    }
#endif
}

} // namespace neverd::registration_test
