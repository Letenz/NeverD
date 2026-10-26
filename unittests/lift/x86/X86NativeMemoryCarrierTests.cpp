//===- X86NativeMemoryCarrierTests.cpp - Reserved frame bases -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep instruction-owned data addresses out of compiler-owned frame bases.
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/c/render/X86/X86APXAtomic.h"
#include "../../../lib/backend/c/render/X86/X86CompressExpand.h"
#include "../../../lib/backend/c/render/X86/X86FourSource.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/render/LLVMC/LLVMCIntrinsicRender.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/llvm/X86/X86APXAtomic.h"
#include "neverd/backend/llvm/X86/X86DescriptorStateNative.h"
#include "neverd/backend/llvm/X86/X86SystemStateAsm.h"

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
enum class MemoryFamily { Compact, FourSource, Descriptor, Query, Atomic };

TEST(X86NativeMemoryCarrier, RealignedDynamicFramesRetainDataAddresses) {
  for (Arch A : {Arch::X64, Arch::X86})
    for (const auto Family :
         {MemoryFamily::Compact, MemoryFamily::FourSource,
          MemoryFamily::Descriptor, MemoryFamily::Query, MemoryFamily::Atomic})
      for (bool Narrow : {false, true}) {
        const bool FourSource = Family == MemoryFamily::FourSource;
        if (Narrow && (FourSource || Family == MemoryFamily::Compact))
          continue;
        const unsigned WordBits = A == Arch::X64 ? 64 : 32;
        const unsigned AddressBits = Narrow ? WordBits / 2 : WordBits;
        for (auto Format :
             {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
          SCOPED_TRACE(unsigned(A));
          SCOPED_TRACE(unsigned(Family));
          SCOPED_TRACE(AddressBits);
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
          auto *Fixed =
              B.CreateAlloca(llvm::ArrayType::get(B.getInt64Ty(), 16));
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
          llvm::Type *Return = B.getVoidTy();
          std::string Assembly, Constraints, Mnemonic;
          if (FourSource) {
            const auto P = *x86FourSourcePlan(4, 0, 0);
            Assembly = P.assembly();
            Constraints = P.constraints();
            Mnemonic = P.Control.mnemonic();
            Args.insert(Args.end(), 5, Zero);
            Args.push_back(B.getInt16(1));
            Return = V;
          } else if (Family == MemoryFamily::Compact) {
            const auto P = *x86CompactPlan(false, 16, 2, 0, 0);
            Assembly = P.assembly();
            Constraints = P.constraints();
            Mnemonic = P.mnemonic();
            Args.insert(Args.end(), {B.getInt16(1), Zero});
          } else {
            Args[0] = B.CreatePtrToInt(F->getArg(0), B.getIntNTy(WordBits));
            if (Family == MemoryFamily::Descriptor) {
              const auto P = *x86DescriptorStateNative(
                  Intrinsic::Verr, A, true, 1, NdMemoryAddressSpace::Default,
                  X86DescriptorAddress{AddressBits, 0});
              Assembly = P.Text;
              Constraints = P.Constraints;
              Mnemonic = P.Mnemonic;
              Return = B.getInt8Ty();
            } else if (Family == MemoryFamily::Query) {
              const auto P = *x86SystemStateAddressAsm(
                  Intrinsic::X86LoadAccessRightsMemory, A, 4, AddressBits, 0);
              Assembly = P.Text;
              Constraints = P.Constraints;
              Mnemonic = "lar";
              Return =
                  llvm::StructType::get(C, {B.getInt32Ty(), B.getInt8Ty()});
            } else {
              const auto P = *x86APXAtomicPlan(Intrinsic::X86LegacyRaoAdd, 4,
                                               AddressBits, 0, 0, A);
              Assembly = P.assembly();
              Constraints = P.constraints();
              Mnemonic = P.mnemonic();
              Args.push_back(B.getInt32(7));
            }
          }
          std::vector<llvm::Type *> Types;
          for (const auto *Arg : Args)
            Types.push_back(Arg->getType());
          auto *Type = llvm::FunctionType::get(Return, Types, false);
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
                              A == Arch::X64 ? CS_MODE_64 : CS_MODE_32,
                              &Decoder),
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
              EXPECT_EQ(X86.addr_size, AddressBits / 8);
              unsigned MemoryOperands = 0;
              for (unsigned J = 0; J < X86.op_count; ++J)
                if (X86.operands[J].type == X86_OP_MEM) {
                  ++MemoryOperands;
                  const auto Base = X86.operands[J].mem.base;
                  // The operand must not use the live realignment base or a
                  // stack-default base. Its exact scratch register is
                  // immaterial.
                  EXPECT_NE(Base, A == Arch::X64 ? X86_REG_RBX : X86_REG_ESI);
                  EXPECT_NE(Base, A == Arch::X64 ? X86_REG_RBP : X86_REG_EBP);
                  EXPECT_NE(Base, A == Arch::X64 ? X86_REG_RSP : X86_REG_ESP);
                  EXPECT_NE(Base, X86_REG_SI);
                  EXPECT_NE(Base, X86_REG_BP);
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

TEST(X86NativeMemoryCarrier,
     OlderDescriptorAndAtomicConstraintsRemainReadable) {
  for (Arch A : {Arch::X86, Arch::X64})
    for (bool Narrow : {false, true}) {
      const unsigned WordBits = A == Arch::X64 ? 64 : 32;
      const unsigned AddressBits = Narrow ? WordBits / 2 : WordBits;
      for (auto Id : {Intrinsic::Verr, Intrinsic::Sgdt,
                      Intrinsic::X86LegacyRaoAdd, Intrinsic::ApxRaoAdd,
                      Intrinsic::X86LegacyCmpccXadd, Intrinsic::ApxCmpccXadd}) {
        SCOPED_TRACE(unsigned(A));
        SCOPED_TRACE(AddressBits);
        SCOPED_TRACE(unsigned(Id));
        llvm::LLVMContext C;
        llvm::Module M("legacy-scalar-memory", C);
        llvm::IRBuilder<> B(C);
        auto *F = llvm::Function::Create(
            llvm::FunctionType::get(B.getVoidTy(), false),
            llvm::GlobalValue::ExternalLinkage, "legacy", M);
        B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", F));
        std::vector<llvm::Value *> Args = {B.getIntN(WordBits, 0x1234)};
        std::string Assembly, Constraints;
        llvm::Type *Return = B.getVoidTy();
        const bool Descriptor = Id == Intrinsic::Verr || Id == Intrinsic::Sgdt;
        if (Descriptor) {
          const auto P = *x86DescriptorStateNative(
              Id, A, true, Id == Intrinsic::Verr ? 1 : 0,
              NdMemoryAddressSpace::Default,
              X86DescriptorAddress{AddressBits, 0x3e});
          Assembly = P.Text;
          Constraints = P.Constraints;
          Constraints.replace(Constraints.find(P.InputConstraint),
                              P.InputConstraint.size(), "{bx}");
          if (P.ResultBytes)
            Return = B.getInt8Ty();
        } else {
          const auto P = x86APXAtomicPlan(Id, 4, AddressBits, 0, 0, A);
          if (!P)
            continue;
          Assembly = P->assembly();
          Constraints = P->constraints(true);
          Args.push_back(B.getInt32(7));
          if (P->compare()) {
            Args.push_back(B.getInt32(13));
            Return = B.getInt32Ty();
          }
        }
        std::vector<llvm::Type *> Types;
        for (const auto *Arg : Args)
          Types.push_back(Arg->getType());
        auto *Type = llvm::FunctionType::get(Return, Types, false);
        auto *Call = B.CreateCall(
            llvm::InlineAsm::get(Type, Assembly, Constraints, true), Args);
        B.CreateRetVoid();
        ASSERT_FALSE(llvm::verifyModule(M, &llvm::errs()));
        if (Descriptor)
          EXPECT_TRUE(renderX86DescriptorStateCall(
              A, BinaryFormat::ELF, *Call, "out", !Return->isVoidTy(), {"p"}));
        else
          EXPECT_TRUE(x86APXAtomicCallPlan(A, *Call));
        Constraints.replace(Constraints.find("{bx}"), 4, "r");
        Call->setCalledOperand(
            llvm::InlineAsm::get(Type, Assembly, Constraints, true));
        if (Descriptor)
          EXPECT_THROW(renderX86DescriptorStateCall(A, BinaryFormat::ELF, *Call,
                                                    "out", !Return->isVoidTy(),
                                                    {"p"}),
                       std::invalid_argument);
        else
          EXPECT_THROW(x86APXAtomicCallPlan(A, *Call), std::invalid_argument);
      }
    }
}
} // namespace
