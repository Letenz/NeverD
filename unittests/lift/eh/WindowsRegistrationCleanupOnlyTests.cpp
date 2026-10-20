//===- WindowsRegistrationCleanupOnlyTests.cpp - PE32 unwind-only IR -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/backend/llvm/WindowsEHNativeSource.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/ir/med/X86RegistrationCallback.h"
#include "neverd/loader/COFF/COFFLoader.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <cstdlib>

using namespace neverd;

namespace {
#ifdef LLVM_NEVERD_X86_CXX_FUNCTION_RECEIPTS
TEST(WindowsRegistrationCleanupOnly, InputPE32PreservesEveryUnwindAction) {
  const auto *Path = std::getenv("NEVERD_REGISTRATION_CLEANUP_ONLY_PE32");
  if (!Path)
    GTEST_SKIP() << "requires the registration_cleanup_only.cpp PE32 fixture";
  auto Image = COFFLoader().load(Path);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const auto Export = llvm::find_if(Image->Exports, [](const auto &Entry) {
    return Entry.Name == "cleanup_only_parent";
  });
  ASSERT_NE(Export, Image->Exports.end());
  const auto *Source = Image->ExceptionMetadata.findFunction(Export->Addr);
  ASSERT_TRUE(Source && Source->Cxx && Source->Registration);
  ASSERT_TRUE(Source->Cxx->TryBlocks.empty());
  const auto Actions =
      llvm::count_if(Source->Cxx->UnwindMap,
                     [](const auto &Action) { return Action.ActionVA; });
  ASSERT_GE(Actions, 1);
  ASSERT_LE(Actions, 2);
  ASSERT_TRUE(
      classifyWindowsEHNativeSource(*Source, Arch::X86, BinaryFormat::COFF,
                                    WindowsEHNativeCapability::IRLowering)
          .canLowerNativeIR());
  EXPECT_FALSE(
      classifyWindowsEHNativeSource(*Source, Arch::X86, BinaryFormat::COFF)
          .canPatchOutput());
  Decoder Decode;
  ASSERT_TRUE(Decode.init(*Image));
  auto Low = CFGBuilder().build(*Image, Decode, Export->Addr, "cleanup_only");
  ASSERT_TRUE(Low.RegistrationStates);
  for (const auto &Diagnostic : Low.RegistrationStates->Diagnostics)
    llvm::errs() << Diagnostic << '\n';
  ASSERT_TRUE(Low.RegistrationStates->Complete);
  ASSERT_TRUE(Low.RegistrationStates->CleanupFrameEffectsComplete);
  ASSERT_EQ(Low.RegistrationStates->CleanupContracts.size(), Actions);
  size_t CleanupCalls = 0;
  for (const auto &Contract : Low.RegistrationStates->CleanupContracts)
    CleanupCalls += Contract.Calls.size();
  ASSERT_EQ(CleanupCalls, 2u);
  EXPECT_TRUE(Low.RegistrationStates->CxxContinuations.empty());
  LowToMedConverter Converter;
  Converter.setBinaryImage(&*Image);
  auto Med = Converter.convert(Low, Arch::X86, BinaryFormat::COFF);
  inferMedTypes(Med, Arch::X86);
  const auto Owners = registrationCatchBlocks(Med);
  const auto Parents = registrationCleanupParents(Med);
  ASSERT_TRUE(Owners && Parents);
  EXPECT_TRUE(Owners->empty());
  ASSERT_EQ(Parents->size(), Actions);
  for (const auto &[State, Owner] : *Parents)
    EXPECT_FALSE(Owner);
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = Med;
    auto &States = *Changed.RegistrationStates;
    if (Mutation == 0)
      States.CallbackStatesComplete = false;
    if (Mutation == 1)
      States.RegistrationLifetimeComplete = false;
    if (Mutation == 2) {
      auto State = llvm::find_if(
          States.Blocks, [](const auto &Block) { return Block.Reached; });
      ASSERT_NE(State, States.Blocks.end());
      State->CallbackOnly = true;
      State->CxxCatchStacks = {{{0, 0}}};
    }
    if (Mutation == 3)
      States.CxxContinuations.emplace_back();
    EXPECT_FALSE(registrationCatchBlocks(Changed));
    EXPECT_FALSE(registrationCleanupParents(Changed));
  }
  llvm::LLVMContext Context;
  MedLLVMEmitter Emitter;
  auto Module = Emitter.emit({Med}, Context, "cleanup-only", Arch::X86, {},
                             &*Image, BinaryFormat::COFF);
  ASSERT_TRUE(Module);
  auto *Parent = Module->getFunction(Med.Name);
  ASSERT_TRUE(Parent);
  ASSERT_TRUE(Parent->getMetadata(windows_eh_md::NativeAttachment));
  ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  auto Control = validateCOFFRegistrationCxxControlIR(*Parent, *Source, *Image);
  ASSERT_FALSE(bool(Control)) << llvm::toString(std::move(Control));
  auto Frame = validateCOFFRegistrationCxxIR(*Parent, *Source, *Image);
  ASSERT_FALSE(bool(Frame)) << llvm::toString(std::move(Frame));

  unsigned Invokes = 0, Cleanups = 0, ToCaller = 0;
  for (auto &Block : *Parent)
    for (auto &I : Block) {
      EXPECT_FALSE((llvm::isa<llvm::CatchPadInst, llvm::CatchSwitchInst,
                              llvm::CatchReturnInst>(I)));
      Invokes += llvm::isa<llvm::InvokeInst>(I);
      Cleanups += llvm::isa<llvm::CleanupPadInst>(I);
      if (const auto *Return = llvm::dyn_cast<llvm::CleanupReturnInst>(&I))
        ToCaller += !Return->getUnwindDest();
    }
  EXPECT_GE(Invokes, 1u);
  EXPECT_EQ(Cleanups, Actions);
  EXPECT_EQ(ToCaller, 1u);
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = llvm::CloneModule(*Module);
    auto *Function = Changed->getFunction(Parent->getName());
    llvm::CleanupReturnInst *Inner = nullptr;
    for (auto &Block : *Function)
      if (auto *Return =
              llvm::dyn_cast<llvm::CleanupReturnInst>(Block.getTerminator());
          Return && (!Inner || Return->getUnwindDest()))
        Inner = Return;
    ASSERT_TRUE(Inner);
    auto *Outer = Inner->getUnwindDest();
    llvm::InvokeInst *Invoke = nullptr;
    std::vector<llvm::CallInst *> CleanupCalls;
    for (auto &Block : *Function)
      for (auto &I : Block) {
        if (auto *Call = llvm::dyn_cast<llvm::InvokeInst>(&I);
            Call && Call->getUnwindDest() == Inner->getParent())
          Invoke = Call;
        if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&I);
            Call && &Block == Inner->getParent() &&
            !llvm::isa<llvm::IntrinsicInst>(Call))
          CleanupCalls.push_back(Call);
      }
    ASSERT_TRUE(Invoke && !CleanupCalls.empty());
    auto *CleanupCall = CleanupCalls.front();
    llvm::IRBuilder<> B(Inner);
    switch (Mutation) {
    case 0: {
      // A verifier-valid ordinary call drops the complete cleanup path.
      B.SetInsertPoint(Invoke);
      llvm::SmallVector<llvm::Value *, 4> Args(Invoke->args());
      auto *Call = B.CreateCall(Invoke->getFunctionType(),
                                Invoke->getCalledOperand(), Args);
      Call->setCallingConv(Invoke->getCallingConv());
      Call->setAttributes(Invoke->getAttributes());
      Call->copyMetadata(*Invoke);
      Invoke->replaceAllUsesWith(Call);
      B.CreateBr(Invoke->getNormalDest());
      Invoke->eraseFromParent();
      break;
    }
    case 1:
      if (Outer) {
        B.CreateCleanupRet(Inner->getCleanupPad(), nullptr);
        Inner->eraseFromParent();
      } else {
        ASSERT_EQ(CleanupCalls.size(), 2u);
        // O1 combines both actions. Reverse the borrowed objects while
        // preserving dominance and otherwise valid LLVM control flow.
        B.SetInsertPoint(CleanupCall);
        auto *Second = B.Insert(
            llvm::cast<llvm::Instruction>(CleanupCalls.back()->getArgOperand(0))
                ->clone());
        CleanupCalls.back()->setArgOperand(0, CleanupCall->getArgOperand(0));
        CleanupCall->setArgOperand(0, Second);
      }
      break;
    case 2:
      CleanupCall->eraseFromParent();
      break;
    case 3:
      B.SetInsertPoint(CleanupCall);
      CleanupCall->setArgOperand(0, B.CreateGEP(B.getInt8Ty(),
                                                CleanupCall->getArgOperand(0),
                                                B.getInt32(4)));
      break;
    case 4:
      CleanupCall->removeFnAttr(llvm::Attribute::NoUnwind);
      break;
    case 5:
      Invoke->setMetadata(windows_eh_md::RegistrationOperationAttachment,
                          nullptr);
      break;
    case 6:
      Function->setMetadata(windows_eh_md::NativeAttachment, nullptr);
      break;
    case 7: {
      llvm::StoreInst *Initialization = nullptr;
      for (auto &Block : *Function)
        for (auto &I : Block) {
          auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I);
          auto *Value =
              Store
                  ? llvm::dyn_cast<llvm::ConstantInt>(Store->getValueOperand())
                  : nullptr;
          if (!Initialization && Value && Value->equalsInt(1) &&
              Store->getMetadata(
                  windows_eh_md::RegistrationOperationAttachment))
            Initialization = Store;
        }
      ASSERT_TRUE(Initialization);
      Initialization->setOperand(
          0,
          llvm::UndefValue::get(Initialization->getValueOperand()->getType()));
      auto Control =
          validateCOFFRegistrationCxxControlIR(*Function, *Source, *Image);
      ASSERT_FALSE(bool(Control)) << llvm::toString(std::move(Control));
      break;
    }
    }
    ASSERT_FALSE(llvm::verifyModule(*Changed, &llvm::errs()));
    auto Error = validateCOFFRegistrationCxxIR(*Function, *Source, *Image);
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
  }
  // The normal LLVM backend must retain real cleanup funclets and their EH
  // tables even though PE installation remains a separate unsupported step.
  auto Compiled = Codegen().compile(*Module, Arch::X86, BinaryFormat::COFF);
  ASSERT_TRUE(Compiled.Success);
}
#endif
} // namespace
