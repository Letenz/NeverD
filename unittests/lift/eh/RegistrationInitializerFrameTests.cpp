//===- RegistrationInitializerFrameTests.cpp - Constructor LLVM proof -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check constructor must-writes against normal and exceptional LLVM edges.
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

TEST(RegistrationInitializerFrame, InitializesOnlyTheNormalEdge) {
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    SCOPED_TRACE(Mutation);
    llvm::LLVMContext Context;
    llvm::Module Module("initializer-frame", Context);
    Module.setDataLayout("e-m:x-p:32:32-i64:64-n8:16:32-S32");
    llvm::IRBuilder<> B(Context);
    auto *Function = llvm::Function::Create(
        llvm::FunctionType::get(B.getInt32Ty(), false),
        llvm::GlobalValue::ExternalLinkage, "parent", Module);
    auto Personality = Module.getOrInsertFunction(
        "__CxxFrameHandler3", llvm::FunctionType::get(B.getInt32Ty(), true));
    Function->setPersonalityFn(
        llvm::cast<llvm::Constant>(Personality.getCallee()));
    auto Constructor = Module.getOrInsertFunction(
        "constructor",
        llvm::FunctionType::get(B.getVoidTy(), {B.getPtrTy(), B.getInt32Ty()},
                                false));
    llvm::cast<llvm::Function>(Constructor.getCallee())
        ->setCallingConv(llvm::CallingConv::X86_ThisCall);
    auto Block = [&](const char *Name) {
      return llvm::BasicBlock::Create(Context, Name, Function);
    };
    auto *Entry = Block("entry");
    auto *Normal = Block("normal");
    auto *Dispatch = Block("dispatch");
    auto *Handler = Block("handler");
    auto *Exit = Block("exit");
    B.SetInsertPoint(Entry);
    auto *Frame = B.CreateAlloca(llvm::ArrayType::get(B.getInt8Ty(), 16));
    auto *Tail = B.CreateInBoundsGEP(B.getInt8Ty(), Frame, B.getInt32(8));
    llvm::Value *Argument = B.getInt32(7);
    if (Mutation == 3)
      Argument = B.CreatePtrToInt(Frame, B.getInt32Ty());
    if (Mutation == 4)
      Argument = llvm::PoisonValue::get(B.getInt32Ty());
    auto *Call =
        B.CreateInvoke(Constructor, Normal, Dispatch, {Frame, Argument});
    Call->setCallingConv(llvm::CallingConv::X86_ThisCall);
    B.SetInsertPoint(Normal);
    B.CreateRet(B.CreateAdd(B.CreateLoad(B.getInt32Ty(), Frame),
                            B.CreateLoad(B.getInt32Ty(), Tail)));
    B.SetInsertPoint(Dispatch);
    auto *Switch =
        B.CreateCatchSwitch(llvm::ConstantTokenNone::get(Context), nullptr, 1);
    Switch->addHandler(Handler);
    B.SetInsertPoint(Handler);
    auto *Pad = B.CreateCatchPad(
        Switch, {llvm::ConstantPointerNull::get(B.getPtrTy()), B.getInt32(64),
                 llvm::ConstantPointerNull::get(B.getPtrTy())});
    if (Mutation == 1)
      B.CreateLoad(B.getInt32Ty(), Frame)->setVolatile(true);
    B.CreateCatchRet(Pad, Exit);
    B.SetInsertPoint(Exit);
    B.CreateRet(B.getInt32(0));
    ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    BinaryImage Image;
    RegistrationCxxFrameContract Contract;
    Contract.Image = &Image;
    Contract.Catches.emplace_back().Catch = Pad;
    auto &Borrow = Contract.Borrows[Call];
    Borrow.Reads = Borrow.Writes = {{0, 4}, {8, 12}};
    Borrow.Initialized = Mutation == 2
                             ? std::vector<RegistrationObjectExtent>{{0, 4}}
                             : Borrow.Writes;
    Borrow.Initializer = Mutation != 5;
    Borrow.ScalarStackWords = 1;
    auto Error = validateFramePrivacy(*Function, {}, Frame, {}, {}, {}, {}, 0,
                                      {}, &Contract);
    const bool Failed = bool(Error);
    const auto Message = llvm::toString(std::move(Error));
    EXPECT_EQ(Failed, Mutation != 0) << Message;
    if (Mutation == 1 || Mutation == 2 || Mutation == 5)
      EXPECT_NE(Message.find("initialized on every path"), std::string::npos)
          << Message;
  }
}
} // namespace
