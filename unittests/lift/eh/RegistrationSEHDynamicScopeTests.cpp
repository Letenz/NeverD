//===- RegistrationSEHDynamicScopeTests.cpp - Dynamic state mutations ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/llvm/X86/MedLLVMRegistrationSEHDynamicScope.h"
#include "RegistrationSEHDynamicScopeTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"

namespace neverd::registration_test {

TEST(RegistrationSEHDynamicScope, PHIsRetainTheirSourcePredecessors) {
  llvm::LLVMContext Context;
  llvm::Module Module("dynamic-scope-phi", Context);
  llvm::IRBuilder<> B(Context);
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(B.getInt32Ty(), {B.getInt1Ty()}, false),
      llvm::GlobalValue::ExternalLinkage, "parent", Module);
  Function->setPersonalityFn(llvm::cast<llvm::Constant>(
      Module
          .getOrInsertFunction("_except_handler3",
                               llvm::FunctionType::get(B.getInt32Ty(), true))
          .getCallee()));
  auto Block = [&](const char *Name) {
    return llvm::BasicBlock::Create(Context, Name, Function);
  };
  auto *Setup = Block("setup");
  auto *Left = Block("left");
  auto *Right = Block("right");
  auto *Body = Block("body");
  B.SetInsertPoint(Setup);
  auto *Frame = B.CreateAlloca(llvm::ArrayType::get(B.getInt8Ty(), 64));
  B.CreateCondBr(Function->getArg(0), Left, Right);
  B.SetInsertPoint(Left);
  B.CreateBr(Body);
  B.SetInsertPoint(Right);
  B.CreateBr(Body);
  B.SetInsertPoint(Body);
  auto *Phi = B.CreatePHI(B.getInt32Ty(), 2);
  Phi->addIncoming(B.getInt32(11), Left);
  Phi->addIncoming(B.getInt32(22), Right);
  auto *Return = B.CreateRet(Phi);
  std::map<int32_t, llvm::BasicBlock *> Unwinds;
  for (int32_t State : {0, 1}) {
    auto *Unwind = Block("unwind");
    B.SetInsertPoint(Unwind);
    auto *Pad = B.CreateCleanupPad(llvm::ConstantTokenNone::get(Context));
    B.CreateCleanupRet(Pad, nullptr);
    Unwinds.emplace(State, Unwind);
  }
  MedBlock Source;
  Source.Id = 3;
  Source.StartAddr = 0x401020;
  Source.EndAddr = 0x401030;
  auto *Begin = llvm::Intrinsic::getOrInsertDeclaration(
      &Module, llvm::Intrinsic::seh_scope_begin);
  auto *End = llvm::Intrinsic::getOrInsertDeclaration(
      &Module, llvm::Intrinsic::seh_scope_end);
  ASSERT_TRUE(x86_registration::canEmitDynamicSEHScope(*Body));
  EXPECT_EQ(x86_registration::emitDynamicSEHScope(*Body, *Frame, 32, 0x401000,
                                                  Source, {0, 1}, Unwinds,
                                                  *Begin, *End),
            0u);
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  EXPECT_EQ(Phi->getIncomingBlock(0), Left);
  EXPECT_EQ(Phi->getIncomingBlock(1), Right);
  EXPECT_EQ(Phi->getIncomingValueForBlock(Left), B.getInt32(11));
  EXPECT_EQ(Phi->getIncomingValueForBlock(Right), B.getInt32(22));
  EXPECT_EQ(Left->getTerminator()->getSuccessor(0), Phi->getParent());
  EXPECT_EQ(Right->getTerminator()->getSuccessor(0), Phi->getParent());
  EXPECT_EQ(Return->getReturnValue(), Phi);
}

void checkDynamicSEHScopeEdits(const llvm::Function &Parent,
                               const ExceptionFunction &Source,
                               const BinaryImage &Image) {
#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
  for (unsigned Mutation = 0; Mutation != 12; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Module = llvm::CloneModule(*Parent.getParent());
    auto *Function = Module->getFunction(Parent.getName());
    llvm::LoadInst *State = nullptr;
    llvm::SwitchInst *Switch = nullptr;
    llvm::SelectInst *Restore = nullptr;
    llvm::StoreInst *SourceWrite = nullptr;
    for (auto &Block : *Function)
      for (auto &I : Block) {
        if (auto *Choice = llvm::dyn_cast<llvm::SwitchInst>(&I))
          if (auto *Read =
                  llvm::dyn_cast<llvm::LoadInst>(Choice->getCondition());
              Read && Read->getName().starts_with("registration.scope")) {
            State = Read;
            if (Read->getParent() == &Block)
              Switch = Choice;
          }
        const auto *MD =
            I.getMetadata(windows_eh_md::RegistrationOperationAttachment);
        if (auto *Write = llvm::dyn_cast<llvm::StoreInst>(&I)) {
          if (I.getMetadata(windows_eh_md::RegistrationJumpAttachment))
            Restore =
                llvm::dyn_cast<llvm::SelectInst>(Write->getValueOperand());
          if (MD && MD->getNumOperands() == 5) {
            const auto *Address = llvm::mdconst::dyn_extract<llvm::ConstantInt>(
                MD->getOperand(1));
            if (Address && llvm::any_of(Source.Registration->TryLevelStores,
                                        [&](const auto &S) {
                                          return S.StoreVA ==
                                                 Address->getZExtValue();
                                        }))
              SourceWrite = Write;
          }
        }
      }
    ASSERT_TRUE(State && Switch && SourceWrite);
    if (Mutation == 6 && !Restore)
      continue;
    auto *Address =
        llvm::cast<llvm::GetElementPtrInst>(State->getPointerOperand());
    llvm::IRBuilder<> B(State);
    switch (Mutation) {
    case 0:
      Switch->setCondition(B.getInt32(0));
      break;
    case 1:
      State->setVolatile(false);
      break;
    case 2:
      Address->setOperand(1, B.getInt32(0));
      break;
    case 3:
      Switch->case_begin()->setSuccessor(Switch->getDefaultDest());
      break;
    case 4:
      B.CreateStore(B.getInt32(0), Address);
      break;
    case 5:
      B.CreateStore(B.getInt8(0),
                    B.CreateGEP(B.getInt8Ty(), Address, B.getInt32(1)));
      break;
    case 6:
      Restore->setFalseValue(Restore->getTrueValue());
      break;
    case 7:
      SourceWrite->setOperand(0, B.getInt32(42));
      break;
    case 8: {
      llvm::IRBuilder<> AtWrite(SourceWrite);
      SourceWrite->setOperand(
          1, AtWrite.CreateGEP(B.getInt8Ty(), SourceWrite->getPointerOperand(),
                               B.getInt32(1)));
      break;
    }
    case 9:
      SourceWrite->setMetadata(windows_eh_md::RegistrationOperationAttachment,
                               nullptr);
      break;
    case 10:
      SourceWrite->setAlignment(llvm::Align(4));
      break;
    case 11:
      SourceWrite->clone()->insertBefore(SourceWrite->getIterator());
      break;
    }
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    auto Error = validateCOFFRegistrationIR(*Function, Source, Image);
    ASSERT_TRUE(bool(Error));
    const auto Message = llvm::toString(std::move(Error));
    EXPECT_TRUE(Message.find("scope") != std::string::npos ||
                Message.find("source try level") != std::string::npos ||
                Message.find("source execution") != std::string::npos)
        << Message;
  }
#endif
}

void checkDynamicSEHCallEdits(const llvm::Function &Parent,
                              const ExceptionFunction &Source,
                              const BinaryImage &Image) {
#ifdef LLVM_NEVERD_X86_REGISTRATION_SEH_DYNAMIC_CALLS
  const auto Attribute =
      llvm::mc_rewrite::RewriteWinX86SEHInheritStateAttribute;
  bool HasCalls = false;
  for (const auto &Block : Parent)
    for (const auto &I : Block)
      if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(&I))
        HasCalls |= Call->getAttributes().hasFnAttr(Attribute);
  if (!HasCalls)
    return;
  auto Contract =
      exception_rewrite::validateExceptionRewriteContracts(*Parent.getParent());
  ASSERT_TRUE(bool(Contract)) << llvm::toString(Contract.takeError());
  for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Module = llvm::CloneModule(*Parent.getParent());
    auto *Function = Module->getFunction(Parent.getName());
    llvm::CallInst *Call = nullptr;
    llvm::CallInst *Boundary = nullptr;
    for (auto &Block : *Function)
      for (auto &I : Block)
        if (auto *Candidate = llvm::dyn_cast<llvm::CallInst>(&I)) {
          if (Candidate->getAttributes().hasFnAttr(Attribute))
            Call = Candidate;
          if (Candidate->getIntrinsicID() == llvm::Intrinsic::seh_scope_end)
            Boundary = Candidate;
        }
    ASSERT_TRUE(Call && Boundary);
    auto *Receipt = llvm::dyn_cast_or_null<llvm::CallInst>(Call->getPrevNode());
    ASSERT_TRUE(Receipt);
    ASSERT_TRUE(Receipt->getOperandBundle(windows_eh_md::ProvenanceBundle));
    switch (Mutation) {
    case 0:
      Call->removeFnAttr(Attribute);
      break;
    case 1:
      Call->addFnAttr(
          llvm::Attribute::get(Module->getContext(), Attribute, "unchecked"));
      break;
    case 2:
      Call->removeFnAttr(llvm::Attribute::NoInline);
      break;
    case 3:
      Call->setTailCallKind(llvm::CallInst::TCK_Tail);
      break;
    case 4:
      ASSERT_TRUE(Call->getCalledFunction());
      Call->getCalledFunction()->addFnAttr(Attribute);
      break;
    case 5:
      Boundary->addFnAttr(
          llvm::Attribute::get(Module->getContext(), Attribute));
      Boundary->addFnAttr(llvm::Attribute::NoInline);
      break;
    case 6:
      Function->addFnAttr(Attribute);
      break;
    case 7:
      Receipt->eraseFromParent();
      break;
    case 8:
      Receipt->clone()->insertBefore(Call->getIterator());
      break;
    case 9: {
      const auto *MD =
          Call->getMetadata(windows_eh_md::RegistrationOperationAttachment);
      llvm::SmallVector<llvm::Metadata *, 5> Operands(MD->op_begin(),
                                                      MD->op_end());
      Operands[2] = llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
          llvm::Type::getInt32Ty(Module->getContext()), UINT32_MAX));
      Call->setMetadata(windows_eh_md::RegistrationOperationAttachment,
                        llvm::MDNode::get(Module->getContext(), Operands));
      break;
    }
    }
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    auto Error = validateCOFFRegistrationIR(*Function, Source, Image);
    ASSERT_TRUE(bool(Error));
    const auto Message = llvm::toString(std::move(Error));
    EXPECT_TRUE(Message.find("dynamic SEH") != std::string::npos ||
                Message.find("native jump value") != std::string::npos)
        << Message;
    // An unrelated marked boundary is rejected by scope replay. Every change
    // to the counted source call must also invalidate the generic contract.
    if (Mutation != 5) {
      auto AlteredContract =
          exception_rewrite::validateExceptionRewriteContracts(*Module);
      ASSERT_FALSE(bool(AlteredContract));
      llvm::consumeError(AlteredContract.takeError());
    }
  }
#endif
}

} // namespace neverd::registration_test
