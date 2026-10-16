//===- RegistrationPrologueSavesTests.cpp - Source memory before EH proof -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep source save occurrences available until registration reconstruction.

#include "gtest/gtest.h"

#include "neverd/backend/llvm/MedLLVMEmitter.h"

#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"

using namespace neverd;

TEST(RegistrationPrologueSaves, ProjectionRetainsSavesForSourceMemoryProof) {
  for (bool Registration : {false, true}) {
    SCOPED_TRACE(Registration);
    MedFunc Function;
    Function.Entry = 0x1000;
    Function.Name = "prologue_save";
    Function.CC = CallingConv::CDECL;
    Function.ReturnType = NdType::makeVoid();
    if (Registration)
      Function.RegistrationStates.emplace();
    Function.Blocks.resize(1);
    auto &Block = Function.Blocks.front();
    Block.Id = 0;
    Block.StartAddr = Function.Entry;
    Block.EndAddr = Function.Entry + 2;
    MedOp Store;
    Store.Opcode = NdOp::STORE;
    Store.Addr = Function.Entry;
    Store.PrologueSave = true;
    Store.addInput(MedVar::makeConst(0x2000, 4));
    Store.addInput(MedVar::makeConst(7, 4));
    Block.Ops.push_back(Store);
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = Function.Entry + 1;
    Block.Ops.push_back(Return);
    llvm::LLVMContext Context;

    auto Module =
        MedLLVMEmitter().emit({Function}, Context, Function.Name, Arch::X86);

    ASSERT_TRUE(Module);
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    const auto *Emitted = Module->getFunction(Function.Name);
    ASSERT_TRUE(Emitted);
    unsigned Writes = 0;
    for (const auto &Instruction : llvm::instructions(Emitted)) {
      const auto *Write = llvm::dyn_cast<llvm::StoreInst>(&Instruction);
      if (!Write)
        continue;
      const auto *Value =
          llvm::dyn_cast<llvm::ConstantInt>(Write->getValueOperand());
      Writes += Value && Value->getZExtValue() == 7;
    }
    EXPECT_EQ(Writes, unsigned(Registration));
  }
}
