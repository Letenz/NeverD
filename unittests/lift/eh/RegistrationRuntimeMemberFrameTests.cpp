//===- RegistrationRuntimeMemberFrameTests.cpp - Live LLVM receivers ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/codegen/COFF/COFFRegistrationFrameProof.h"
#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"

namespace {
using namespace neverd;
using namespace neverd::coff_registration;

void checkRuntimeMember(unsigned Words) {
  const char *Mutations[] = {
      "checked receiver",  "shifted receiver",      "parent frame",
      "constant receiver", "changed source offset", "out of bounds",
      "value catch",       "constructor authority", "runtime throw authority",
      "scalar stack word", "ended catch",           "partial pointer",
      "frame argument",    "runtime argument",      "undefined argument",
      "poison argument",   "missing argument proof"};
  for (unsigned Mutation = 0; Mutation != std::size(Mutations); ++Mutation) {
    if (!Words && Mutation >= 12)
      continue;
    SCOPED_TRACE(Mutations[Mutation]);
    SCOPED_TRACE(Words);
    llvm::LLVMContext Context;
    llvm::Module Module("runtime-member", Context);
    Module.setDataLayout("e-m:x-p:32:32-i64:64-n8:16:32-S32");
    llvm::IRBuilder<> B(Context);
    auto *Function = llvm::Function::Create(
        llvm::FunctionType::get(B.getInt32Ty(), false),
        llvm::GlobalValue::ExternalLinkage, "parent", Module);
    auto Personality = Module.getOrInsertFunction(
        "__CxxFrameHandler3", llvm::FunctionType::get(B.getInt32Ty(), true));
    Function->setPersonalityFn(
        llvm::cast<llvm::Constant>(Personality.getCallee()));
    std::vector<llvm::Type *> Parameters{B.getPtrTy()};
    Parameters.insert(Parameters.end(), Words, B.getInt32Ty());
    auto Member = Module.getOrInsertFunction(
        "member", llvm::FunctionType::get(B.getInt32Ty(), Parameters, false));
    llvm::cast<llvm::Function>(Member.getCallee())
        ->setCallingConv(llvm::CallingConv::X86_ThisCall);
    auto Trigger = Module.getOrInsertFunction(
        "trigger", llvm::FunctionType::get(B.getVoidTy(), false));
    auto Block = [&](const char *Name) {
      return llvm::BasicBlock::Create(Context, Name, Function);
    };
    auto *Entry = Block("entry"), *Normal = Block("normal");
    auto *Dispatch = Block("dispatch"), *Handler = Block("handler");
    auto *Exit = Block("exit");
    B.SetInsertPoint(Entry);
    auto *Frame = B.CreateAlloca(llvm::ArrayType::get(B.getInt8Ty(), 16));
    B.CreateInvoke(Trigger, Normal, Dispatch);
    B.SetInsertPoint(Normal);
    B.CreateRet(B.getInt32(0));
    B.SetInsertPoint(Dispatch);
    auto *Switch =
        B.CreateCatchSwitch(llvm::ConstantTokenNone::get(Context), nullptr, 1);
    Switch->addHandler(Handler);
    B.SetInsertPoint(Handler);
    auto *Pad =
        B.CreateCatchPad(Switch, {llvm::ConstantPointerNull::get(B.getPtrTy()),
                                  B.getInt32(8), Frame});
    auto *Bits = B.CreateLoad(B.getInt32Ty(), Frame);
    llvm::Value *Object = B.CreateIntToPtr(Bits, B.getPtrTy());
    if (Mutation == 1)
      Object = B.CreateGEP(B.getInt8Ty(), Object, B.getInt32(1));
    if (Mutation == 2)
      Object = Frame;
    if (Mutation == 3)
      Object =
          llvm::ConstantExpr::getIntToPtr(B.getInt32(0x3000), B.getPtrTy());
    if (Mutation == 11)
      Object =
          B.CreateIntToPtr(B.CreateTrunc(Bits, B.getInt16Ty()), B.getPtrTy());
    std::vector<llvm::Value *> Arguments{Object};
    for (unsigned I = 0; I != Words; ++I)
      Arguments.push_back(B.getInt32(I + 7));
    if (Mutation == 12)
      Arguments.back() = B.CreatePtrToInt(Frame, B.getInt32Ty());
    if (Mutation == 13)
      Arguments.back() = B.CreatePtrToInt(Object, B.getInt32Ty());
    if (Mutation == 14)
      Arguments.back() = llvm::UndefValue::get(B.getInt32Ty());
    if (Mutation == 15)
      Arguments.back() = llvm::PoisonValue::get(B.getInt32Ty());
    llvm::CallInst *Call = nullptr;
    if (Mutation != 10) {
      Call = B.CreateCall(Member, Arguments,
                          {llvm::OperandBundleDef("funclet", Pad)});
      Call->setCallingConv(llvm::CallingConv::X86_ThisCall);
    }
    B.CreateCatchRet(Pad, Exit);
    B.SetInsertPoint(Exit);
    if (Mutation == 10) {
      Call = B.CreateCall(Member, Arguments);
      Call->setCallingConv(llvm::CallingConv::X86_ThisCall);
    }
    B.CreateRet(B.getInt32(0));
    ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    BinaryImage Image;
    RegistrationCxxFrameContract Contract;
    Contract.Image = &Image;
    auto &Catch = Contract.Catches.emplace_back();
    Catch.Catch = Pad;
    Catch.ObjectSize = 12;
    Catch.Reference = Mutation != 6;
    Catch.CallbackBlocks = {Handler};
    auto &Borrow = Contract.Borrows[Call];
    Borrow.Root = Pad;
    Borrow.ScalarStackWords = Mutation == 16 ? 0 : Words;
    Borrow.Offset = Mutation == 4 ? 4 : 0;
    Borrow.Reads = {{0, 4}};
    Borrow.Writes = Mutation == 5
                        ? std::vector<RegistrationObjectExtent>{{8, 13}}
                        : Borrow.Reads;
    if (Mutation == 7) {
      Borrow.Initializer = true;
      Borrow.Initialized = Borrow.Writes;
    }
    if (Mutation == 8)
      Borrow.ThrowInfoVA = 0x4000;
    if (Mutation == 9)
      Borrow.ScalarStackWords = Words + 1;
    auto Error = validateFramePrivacy(*Function, {}, Frame, {}, {}, {}, {}, 0,
                                      {}, &Contract);
    const bool Failed = bool(Error);
    const auto Message = llvm::toString(std::move(Error));
    EXPECT_EQ(Failed, Mutation != 0) << Message;
  }
}
TEST(RegistrationRuntimeMemberFrame, RechecksTheActualAllocationAndLifetime) {
  checkRuntimeMember(0);
}

TEST(RegistrationRuntimeMemberFrame, RechecksDefinedScalarStackWords) {
  for (unsigned Words : {1u, 3u})
    checkRuntimeMember(Words);
}
} // namespace
