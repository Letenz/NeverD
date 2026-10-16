//===- RegistrationCopyEdgeTests.cpp - Unobserved handler edge values -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keeps unavailable handler registers out of observable native copy edges.

#include "../../../lib/backend/codegen/COFF/COFFRegistrationCopyEdges.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"

using namespace neverd;

TEST(RegistrationCopyEdge, FrozenPoisonRequiresAClosedPrivateTransport) {
  for (bool Volatile : {false, true})
    for (unsigned Mutation = 0; Mutation != 11; ++Mutation) {
      SCOPED_TRACE(Volatile);
      SCOPED_TRACE(Mutation);
      llvm::LLVMContext Context;
      llvm::Module Module("copy-edge", Context);
      llvm::IRBuilder<> B(Context);
      auto *I32 = B.getInt32Ty();
      auto *Type = Mutation == 7 ? B.getInt128Ty() : I32;
      auto *Function = llvm::Function::Create(
          llvm::FunctionType::get(I32, {Type}, false),
          llvm::GlobalValue::ExternalLinkage, "parent", Module);
      auto *From = llvm::BasicBlock::Create(Context, "from", Function);
      auto *Copy = llvm::BasicBlock::Create(Context, "copy", Function);
      auto *Target = llvm::BasicBlock::Create(Context, "target", Function);
      B.SetInsertPoint(From);
      auto *Register = B.CreateAlloca(Type);
      auto *Snapshot = B.CreateAlloca(Type);
      B.CreateBr(Copy);
      B.SetInsertPoint(Copy);
      llvm::Value *Input = llvm::PoisonValue::get(Type);
      if (Mutation == 5)
        Input = llvm::ConstantInt::get(Type, 0);
      else if (Mutation == 6)
        Input = Function->getArg(0);
      auto *Frozen = new llvm::FreezeInst(Input);
      B.Insert(Frozen);
      B.CreateStore(Frozen, Register)->setVolatile(Volatile);
      B.CreateBr(Target);
      B.SetInsertPoint(Target);
      auto *Read = B.CreateLoad(Type, Register);
      Read->setVolatile(Volatile);
      auto *Save = B.CreateStore(B.CreateFreeze(Read), Snapshot);
      Save->setVolatile(Volatile);
      auto *Reload = B.CreateLoad(Type, Snapshot);
      Reload->setVolatile(Volatile);
      B.CreateStore(Reload, Register)->setVolatile(Volatile);
      if (Mutation == 2 || Mutation == 4) {
        auto *Argument =
            Mutation == 4 ? static_cast<llvm::Value *>(Register) : Read;
        auto Callee = Module.getOrInsertFunction(
            "observer", llvm::FunctionType::get(B.getVoidTy(),
                                                {Argument->getType()}, false));
        B.CreateCall(Callee, {Argument});
      }
      if (Mutation == 3) {
        auto *Global = new llvm::GlobalVariable(
            Module, Type, false, llvm::GlobalValue::ExternalLinkage, nullptr,
            "observed");
        B.CreateStore(Read, Global);
      }
      if (Mutation == 8)
        B.Insert(new llvm::ICmpInst(llvm::CmpInst::ICMP_EQ, Read,
                                    llvm::ConstantInt::get(Type, 0)));
      if (Mutation == 9) {
        Save->setAtomic(llvm::AtomicOrdering::Monotonic);
        Save->setAlignment(llvm::Align(4));
      }
      B.CreateRet(Mutation == 1 ? static_cast<llvm::Value *>(Read)
                                : B.getInt32(0));
      ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
      size_t Work = Mutation == 10 ? limits::kMaxRegistrationEHStateWork : 0;
      const auto *Destination = coff_registration::registrationCopyDestination(
          *From, *Copy, {Target}, Work, Volatile);
      EXPECT_EQ(Destination, Mutation == 0 ? Target : nullptr);
      if (Volatile && Mutation == 0) {
        Work = 0;
        EXPECT_EQ(coff_registration::registrationCopyDestination(
                      *From, *Copy, {Target}, Work, false),
                  nullptr);
      }
    }
}
