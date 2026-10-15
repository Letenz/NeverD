//===- RegistrationMemberArgumentTests.cpp - Scalar call words -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/loader/COFF/COFFLoader.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <cstdlib>

namespace {
using namespace neverd;

TEST(RegistrationMemberArguments, InputPE32BindsScalarStackWords) {
  const auto *Path = std::getenv("NEVERD_REGISTRATION_REALIGNED_NATIVE_PE32");
  if (!Path)
    GTEST_SKIP() << "set NEVERD_REGISTRATION_REALIGNED_NATIVE_PE32";
  auto Image = COFFLoader().load(Path);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const auto Entry = llvm::find_if(Image->Exports, [](const auto &Export) {
    return Export.Name == "callback_parent";
  });
  ASSERT_NE(Entry, Image->Exports.end());
  Decoder Decode;
  ASSERT_TRUE(Decode.init(*Image));
  const auto Low =
      CFGBuilder().build(*Image, Decode, Entry->Addr, "member_parent");
  ASSERT_TRUE(Low.RegistrationStates);
  const auto &States = *Low.RegistrationStates;
  ASSERT_TRUE(States.Complete);
  ASSERT_TRUE(States.CallFrameEffectsComplete);
  ASSERT_TRUE(States.RuntimeObjectAccessesComplete);
  const auto Borrow =
      llvm::find_if(States.CallFrameEffects, [](const auto &Call) {
        return Call.ECXRuntimeObject.has_value() && Call.StackPopBytes;
      });
  ASSERT_NE(Borrow, States.CallFrameEffects.end());
  EXPECT_FALSE(Borrow->ECXFrameOffset);
  EXPECT_TRUE(Borrow->FrameReads.empty());
  EXPECT_TRUE(Borrow->FrameWrites.empty());
  auto Fresh = getCheckedX86RegistrationMemberCalleeABI(*Image, Borrow->Target);
  ASSERT_TRUE(Fresh);
  EXPECT_TRUE(Fresh->HasIndependentScalarReturn);
  EXPECT_EQ(Fresh->StackPopBytes, Borrow->StackPopBytes);
  EXPECT_EQ(Borrow->StackPopBytes, 4u);
  LowToMedConverter Converter;
  Converter.setBinaryImage(&*Image);
  auto Med = Converter.convert(Low, Arch::X86, BinaryFormat::COFF);
  recoverCallAbi(Med, Arch::X86, {}, &*Image);
  inferMedTypes(Med, Arch::X86);
  const MedCallInfo *SourceCall = nullptr;
  for (const auto &Block : Med.Blocks)
    for (size_t I = 0; I != Block.Ops.size(); ++I)
      if (Block.Ops[I].Addr == Borrow->Address &&
          Block.Ops[I].OriginSeq == Borrow->OpSeq)
        SourceCall = Med.findCall(Block.Id, I);
  ASSERT_TRUE(SourceCall);
  EXPECT_EQ(SourceCall->Args.size(), 2u);
  llvm::LLVMContext Context;
  auto Module =
      MedLLVMEmitter().emit({Med}, Context, "reference-member", Arch::X86, {},
                            &*Image, BinaryFormat::COFF);
  ASSERT_TRUE(Module);
  auto *Parent = Module->getFunction(Med.Name);
  ASSERT_TRUE(Parent);
  ASSERT_TRUE(Parent->getMetadata(windows_eh_md::NativeAttachment));
  ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  auto Proof =
      validateCOFFRegistrationCxxIR(*Parent, *Low.ExceptionMetadata, *Image);
  ASSERT_FALSE(bool(Proof)) << llvm::toString(std::move(Proof));
  for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = Med;
    auto &Effect =
        Changed.RegistrationStates
            ->CallFrameEffects[Borrow - States.CallFrameEffects.begin()];
    if (Mutation == 0)
      Effect.ECXRuntimeObject.reset();
    if (Mutation == 1)
      Effect.ECXRuntimeObject->TryIndex = UINT32_MAX;
    if (Mutation == 2)
      Effect.ECXRuntimeObject->CatchIndex = UINT32_MAX;
    if (Mutation == 3)
      Effect.ECXRuntimeObject->Offset = INT32_MAX;
    if (Mutation == 4)
      Effect.ECXFrameOffset = -24;
    if (Mutation == 5)
      Changed.RegistrationStates->CalleeContracts[Effect.CalleeIndex]
          .ECXReads = {{0, 1}};
    auto &Arguments = Changed.CallInfos[SourceCall - Med.CallInfos.data()].Args;
    if (Mutation == 6)
      Arguments.clear();
    if (Mutation == 7)
      Arguments.push_back(MedVar::makeConst(UINT32_MAX, 4));
    if (Mutation == 8)
      Arguments.front() = MedVar::makeConst(0x3000, 4);
    llvm::LLVMContext ChangedContext;
    auto Rejected =
        MedLLVMEmitter().emit({Changed}, ChangedContext, "changed-receiver",
                              Arch::X86, {}, &*Image, BinaryFormat::COFF);
    ASSERT_TRUE(Rejected);
    const auto *ChangedParent = Rejected->getFunction(Changed.Name);
    ASSERT_TRUE(ChangedParent);
    EXPECT_FALSE(ChangedParent->getMetadata(windows_eh_md::NativeAttachment));
  }
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = llvm::CloneModule(*Module);
    auto *Function = Changed->getFunction(Parent->getName());
    llvm::CallBase *Call = nullptr;
    llvm::AllocaInst *Frame = nullptr;
    for (auto &Block : *Function)
      for (auto &I : Block) {
        if (I.getMetadata(windows_eh_md::RegistrationFrameAttachment))
          Frame = llvm::cast<llvm::AllocaInst>(&I);
        const auto *MD =
            I.getMetadata(windows_eh_md::RegistrationOperationAttachment);
        const auto *Address =
            MD && MD->getNumOperands() > 1
                ? llvm::mdconst::dyn_extract<llvm::ConstantInt>(
                      MD->getOperand(1))
                : nullptr;
        if (Address && Address->getZExtValue() == Borrow->Address)
          if (auto *Candidate = llvm::dyn_cast<llvm::CallBase>(&I))
            Call = Candidate;
      }
    ASSERT_TRUE(Call && Frame);
    llvm::IRBuilder<> B(Call);
    llvm::Value *Object = llvm::ConstantPointerNull::get(B.getPtrTy());
    if (Mutation == 0)
      Object =
          B.CreateGEP(B.getInt8Ty(), Call->getArgOperand(0), B.getInt32(1));
    else if (Mutation == 1)
      Object = Frame;
    if (Mutation < 3)
      Call->setArgOperand(0, Object);
    if (Mutation == 3)
      Call->setArgOperand(
          1, B.CreatePtrToInt(Call->getArgOperand(0), B.getInt32Ty()));
    if (Mutation == 4)
      Call->setArgOperand(1, llvm::UndefValue::get(B.getInt32Ty()));
    if (Mutation == 5)
      Call->setArgOperand(1, llvm::PoisonValue::get(B.getInt32Ty()));
    ASSERT_FALSE(llvm::verifyModule(*Changed, &llvm::errs()));
    auto Error = validateCOFFRegistrationCxxIR(*Function,
                                               *Low.ExceptionMetadata, *Image);
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
  }
}
} // namespace
