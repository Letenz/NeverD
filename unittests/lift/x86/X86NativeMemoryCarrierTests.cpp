//===- X86NativeMemoryCarrierTests.cpp - Reserved frame bases -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep instruction-owned data addresses out of compiler-owned frame bases.
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/c/render/X86/X86CompressExpand.h"
#include "../../../lib/backend/c/render/X86/X86FourSource.h"
#include "gtest/gtest.h"

#include "neverd/backend/codegen/CodeGen.h"

#include "capstone/capstone.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBufferRef.h"
#include "llvm/Support/raw_ostream.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

using namespace neverd;

namespace {
TEST(X86NativeMemoryCarrier, RealignedDynamicFramesRetainDataAddresses) {
  for (Arch A : {Arch::X64, Arch::X86})
    for (bool FourSource : {false, true})
      for (auto Format :
           {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
        SCOPED_TRACE(unsigned(A));
        SCOPED_TRACE(FourSource);
        SCOPED_TRACE(unsigned(Format));
        llvm::LLVMContext C;
        llvm::Module M("native-memory-frame", C);
        llvm::IRBuilder<> B(C);
        auto *F = llvm::Function::Create(
            llvm::FunctionType::get(B.getInt64Ty(),
                                    {B.getPtrTy(), B.getInt32Ty()}, false),
            llvm::GlobalValue::ExternalLinkage, "probe", M);
        F->addFnAttr("target-features", "+avx512f,+avx512vl,+evex512");
        B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", F));
        auto *Fixed = B.CreateAlloca(llvm::ArrayType::get(B.getInt64Ty(), 16));
        Fixed->setAlignment(llvm::Align(64));
        B.CreateStore(B.getInt64(0x12345678), Fixed)->setVolatile(true);
        auto *Dynamic = B.CreateAlloca(B.getInt8Ty(), F->getArg(1));
        Dynamic->setAlignment(llvm::Align(32));
        auto Observe = M.getOrInsertFunction(
            "observe",
            llvm::FunctionType::get(B.getVoidTy(), {B.getPtrTy()}, false));
        B.CreateCall(Observe, {Dynamic});
        auto *V =
            llvm::FixedVectorType::get(B.getInt32Ty(), FourSource ? 16 : 4);
        auto *Zero = llvm::Constant::getNullValue(V);
        std::vector<llvm::Value *> Args = {F->getArg(0)};
        std::string Assembly, Constraints, Mnemonic;
        if (FourSource) {
          const auto P = *x86FourSourcePlan(4, 0, 0);
          Assembly = P.assembly();
          Constraints = P.constraints();
          Mnemonic = P.Control.mnemonic();
          Args.insert(Args.end(), 5, Zero);
          Args.push_back(B.getInt16(1));
        } else {
          const auto P = *x86CompactPlan(false, 16, 2, 0, 0);
          Assembly = P.assembly();
          Constraints = P.constraints();
          Mnemonic = P.mnemonic();
          Args.insert(Args.end(), {B.getInt16(1), Zero});
        }
        std::vector<llvm::Type *> Types;
        for (const auto *Arg : Args)
          Types.push_back(Arg->getType());
        auto *Type = llvm::FunctionType::get(
            FourSource ? static_cast<llvm::Type *>(V) : B.getVoidTy(), Types,
            false);
        B.CreateCall(llvm::InlineAsm::get(Type, Assembly, Constraints, true),
                     Args);
        B.CreateCall(Observe, {Dynamic});
        auto *Canary = B.CreateLoad(B.getInt64Ty(), Fixed);
        Canary->setVolatile(true);
        B.CreateRet(Canary);
        ASSERT_FALSE(llvm::verifyModule(M, &llvm::errs()));
        const auto Object = Codegen().compile(M, A, Format);
        ASSERT_TRUE(Object.Success);
        const llvm::StringRef Bytes(
            reinterpret_cast<const char *>(Object.ObjectData.data()),
            Object.ObjectData.size());
        auto Parsed = llvm::object::ObjectFile::createObjectFile(
            llvm::MemoryBufferRef(Bytes, "probe"));
        ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
        unsigned Matches = 0;
        for (const auto &Section : (*Parsed)->sections()) {
          if (!Section.isText())
            continue;
          auto Contents = Section.getContents();
          ASSERT_TRUE(bool(Contents)) << llvm::toString(Contents.takeError());
          csh Decoder = 0;
          ASSERT_EQ(cs_open(CS_ARCH_X86,
                            A == Arch::X64 ? CS_MODE_64 : CS_MODE_32, &Decoder),
                    CS_ERR_OK);
          ASSERT_EQ(cs_option(Decoder, CS_OPT_DETAIL, CS_OPT_ON), CS_ERR_OK);
          cs_insn *Instructions = nullptr;
          const auto Count = cs_disasm(
              Decoder, reinterpret_cast<const uint8_t *>(Contents->data()),
              Contents->size(), 0, 0, &Instructions);
          for (size_t I = 0; I < Count; ++I) {
            const auto &Insn = Instructions[I];
            if (Insn.mnemonic != Mnemonic)
              continue;
            ++Matches;
            EXPECT_NE(Insn.detail, nullptr);
            if (!Insn.detail)
              continue;
            const auto &X86 = Insn.detail->x86;
            unsigned MemoryOperands = 0;
            for (unsigned J = 0; J < X86.op_count; ++J)
              if (X86.operands[J].type == X86_OP_MEM) {
                ++MemoryOperands;
                const auto Base = X86.operands[J].mem.base;
                // The operand must not use the live realignment base or a
                // stack-default base. Its exact scratch register is immaterial.
                EXPECT_NE(Base, A == Arch::X64 ? X86_REG_RBX : X86_REG_ESI);
                EXPECT_NE(Base, A == Arch::X64 ? X86_REG_RBP : X86_REG_EBP);
                EXPECT_NE(Base, A == Arch::X64 ? X86_REG_RSP : X86_REG_ESP);
                EXPECT_EQ(X86.operands[J].mem.index, X86_REG_INVALID);
                EXPECT_EQ(X86.operands[J].mem.disp, 0);
              }
            EXPECT_EQ(MemoryOperands, 1u);
          }
          cs_free(Instructions, Count);
          cs_close(&Decoder);
        }
        EXPECT_EQ(Matches, 1u);
      }
}

TEST(X86NativeMemoryCarrier, OlderOwnedConstraintsRemainReadable) {
  for (bool FourSource : {false, true})
    for (bool Expand : {false, true}) {
      llvm::LLVMContext C;
      llvm::Module M("legacy-native-memory", C);
      llvm::IRBuilder<> B(C);
      auto *F = llvm::Function::Create(
          llvm::FunctionType::get(B.getVoidTy(), false),
          llvm::GlobalValue::ExternalLinkage, "legacy", M);
      B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", F));
      auto *V = llvm::FixedVectorType::get(B.getInt32Ty(), FourSource ? 16 : 4);
      auto *Zero = llvm::Constant::getNullValue(V);
      std::vector<llvm::Value *> Args = {
          llvm::ConstantPointerNull::get(B.getPtrTy())};
      std::string Assembly, Constraints;
      if (FourSource) {
        const auto P = *x86FourSourcePlan(4, Expand, 0);
        Assembly = P.assembly();
        Constraints = P.constraints(true);
        Args.insert(Args.end(), 5, Zero);
        Args.push_back(B.getInt16(0));
      } else {
        const auto P = *x86CompactPlan(Expand, 16, 2, 0, 0);
        Assembly = P.assembly();
        Constraints = P.constraints(true);
        Args.insert(Args.end(), {B.getInt16(0), Zero});
      }
      std::vector<llvm::Type *> Types;
      for (const auto *Arg : Args)
        Types.push_back(Arg->getType());
      auto *Type = llvm::FunctionType::get(
          FourSource || Expand ? static_cast<llvm::Type *>(V) : B.getVoidTy(),
          Types, false);
      auto *Call = B.CreateCall(
          llvm::InlineAsm::get(Type, Assembly, Constraints, true), Args);
      B.CreateRetVoid();
      ASSERT_FALSE(llvm::verifyModule(M, &llvm::errs()));
      if (FourSource)
        EXPECT_TRUE(x86FourSourceCallPlan(Arch::X64, *Call));
      else
        EXPECT_TRUE(x86CompactCallPlan(Arch::X64, *Call));
      // An arbitrary register could change the default segment. Do not admit
      // it merely because the old, explicit BX convention remains readable.
      Constraints.replace(Constraints.find("{bx}"), 4, "r");
      Call->setCalledOperand(
          llvm::InlineAsm::get(Type, Assembly, Constraints, true));
      if (FourSource)
        EXPECT_THROW(x86FourSourceCallPlan(Arch::X64, *Call),
                     std::invalid_argument);
      else
        EXPECT_THROW(x86CompactCallPlan(Arch::X64, *Call),
                     std::invalid_argument);
    }
}
} // namespace
