//===- RegistrationSEHNullAccessTests.cpp - Exact SEH null reads ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/codegen/COFF/COFFRegistrationSEHException.h"
#include "RegistrationSEHNullAccessTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"

using namespace neverd;

TEST(RegistrationSEHNullAccess, RetainsOnlyTheExactSourceRead) {
  for (unsigned Mutation = 0; Mutation != 11; ++Mutation) {
    SCOPED_TRACE(Mutation);
    MedFunc Source;
    Source.Entry = 0x1000;
    auto &Block = Source.Blocks.emplace_back();
    Block.Id = 0;
    MedVar Address;
    Address.Id = 1;
    Address.SSAVer = 1;
    Address.Size = 8;
    MedOp Copy;
    Copy.Opcode = NdOp::COPY;
    Copy.Output = Address;
    Copy.addInput(MedVar::makeConst(0, 8));
    Block.Ops.push_back(Copy);
    MedOp Read;
    Read.Opcode = NdOp::LOAD;
    Read.Addr = Source.Entry;
    Read.OriginSeq = 1;
    Read.Output.Size = 4;
    Read.addInput(Address);
    Block.Ops.push_back(Read);

    llvm::LLVMContext Context;
    llvm::Module Module("null-read", Context);
    auto *Function = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false),
        llvm::GlobalValue::ExternalLinkage, "guarded", Module);
    if (Mutation != 4)
      Function->addFnAttr(llvm::Attribute::NullPointerIsValid);
    auto *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
    llvm::IRBuilder<> B(Entry);
    auto *Slot = B.CreateAlloca(B.getInt64Ty());
    B.CreateStore(B.getInt64(7), Slot);
    auto *Definition = B.CreateStore(B.getInt64(Mutation == 5 ? 4 : 0), Slot);
    auto *Scalar = B.CreateLoad(B.getInt64Ty(), Slot);
    auto *Pointer = B.CreateIntToPtr(Scalar, B.getPtrTy());
    if (Mutation == 1)
      Pointer = B.CreateIntToPtr(B.getInt64(4), B.getPtrTy());
    auto *Load =
        B.CreateLoad(Mutation == 3 ? B.getInt64Ty() : B.getInt32Ty(), Pointer);
    Load->setAlignment(llvm::Align(1));
    Load->setVolatile(Mutation != 2);
    auto MD = [&](llvm::Constant *Value) {
      return llvm::ConstantAsMetadata::get(Value);
    };
    if (Mutation != 8)
      Load->setMetadata(
          windows_eh_md::RegistrationOperationAttachment,
          llvm::MDNode::get(Context, {MD(B.getInt64(Source.Entry)),
                                      MD(B.getInt64(Source.Entry)),
                                      MD(B.getInt32(Mutation == 9 ? 2 : 1)),
                                      MD(B.getInt32(0)), MD(B.getInt8(0))}));
    if (Mutation == 6)
      Definition->moveAfter(Load);
    if (Mutation == 7)
      B.CreateStore(B.getInt64(0),
                    B.CreateGEP(B.getInt8Ty(), Slot, B.getInt32(0)));
    if (Mutation == 10)
      B.Insert(Load->clone());
    B.CreateRetVoid();
    ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    auto Result = coff_registration::bindSEHNullReads(Source, {Function});
    if (Mutation) {
      ASSERT_FALSE(bool(Result));
      llvm::consumeError(Result.takeError());
    } else {
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      EXPECT_EQ(Result->size(), 1u);
      EXPECT_TRUE(Result->count(Load));
    }
  }
}

namespace neverd::registration_test {

void checkSEHNullReadEdits(const llvm::Function &Parent, const MedFunc &Med,
                           const ExceptionFunction &Source,
                           const BinaryImage &Image) {
#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
  std::vector<const llvm::Function *> Functions;
  for (const auto &F : *Parent.getParent())
    if (!F.isDeclaration())
      Functions.push_back(&F);
  auto Reads = coff_registration::bindSEHNullReads(Med, Functions);
  ASSERT_TRUE(bool(Reads)) << llvm::toString(Reads.takeError());
  for (const auto *Read : *Reads)
    for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
      SCOPED_TRACE(Mutation);
      llvm::ValueToValueMapTy Values;
      auto Module = llvm::CloneModule(*Parent.getParent(), Values);
      auto *Load = llvm::cast<llvm::LoadInst>(Values.lookup(Read));
      llvm::IRBuilder<> B(Load);
      switch (Mutation) {
      case 0:
        Load->setOperand(0, B.CreateIntToPtr(B.getInt32(4), B.getPtrTy()));
        break;
      case 1:
        Load->setVolatile(false);
        break;
      case 2:
        Load->setAlignment(llvm::Align(4));
        break;
      case 3:
        Load->setMetadata(windows_eh_md::RegistrationOperationAttachment,
                          nullptr);
        break;
      case 4:
        B.Insert(Load->clone());
        break;
      }
      ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
      auto Error = validateCOFFRegistrationIR(
          *Module->getFunction(Parent.getName()), Source, Image);
      ASSERT_TRUE(bool(Error));
      EXPECT_NE(llvm::toString(std::move(Error)).find("SEH null"),
                std::string::npos);
    }
#endif
}

} // namespace neverd::registration_test
