//===- X86APXAtomicSourceTests.cpp - APX native/source boundaries -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise native transactions and source contracts. Export complete wrappers
/// for an independent instruction decoder and controlled completion oracle.
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/c/render/X86/X86APXAtomic.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

#include <array>
#include <cstdlib>
#include <stdexcept>
#include <vector>

using namespace neverd;
namespace {
constexpr va_t kEntry = 0x1000;
struct Case {
  X86APXAtomicPlan Plan;
  unsigned Variant;
};
std::vector<Case> cases() {
  std::vector<Case> Cases;
  for (unsigned Bytes : {4u, 8u})
    for (unsigned Segment : {0u, 0x64u, 0x65u})
      for (auto Id : X86APXAtomicInventory)
        for (unsigned CC = 0; CC < (Id == Intrinsic::ApxCmpccXadd ? 16u : 1u);
             ++CC)
          for (unsigned Variant = 0; Variant < 3; ++Variant)
            Cases.push_back(
                {*x86APXAtomicPlan(Id, Bytes, 64, Segment, CC), Variant});
  return Cases;
}
MedVar temporary(unsigned Id, unsigned Bytes) {
  MedVar V;
  V.Id = Id;
  V.Size = Bytes;
  V.TheArch = Arch::X64;
  return V;
}
ExprPtr high(const X86APXAtomicPlan &P) {
  auto E = HighExpr::makeCall("apx", 0, {});
  E->IntrinsicId = P.Id;
  E->Type = P.compare() ? NdType::makeInt(P.Bytes, false) : NdType::makeVoid();
  E->MemoryOrdering = P.compare() ? NdMemoryOrdering::SequentiallyConsistent
                                  : NdMemoryOrdering::Relaxed;
  E->MemoryAddressSpace = P.SegmentPrefix == 0x64 ? NdMemoryAddressSpace::X86FS
                          : P.SegmentPrefix == 0x65
                              ? NdMemoryAddressSpace::X86GS
                              : NdMemoryAddressSpace::Default;
  E->Operands = {HighExpr::makeVar(temporary(0, 8)),
                 HighExpr::makeVar(temporary(2, P.Bytes))};
  if (P.compare()) {
    E->Operands.push_back(HighExpr::makeVar(temporary(1, P.Bytes)));
    E->Operands.push_back(HighExpr::makeConst(P.Condition, 1));
  }
  return E;
}
std::string highExpression(const HighExpr &E, Arch A = Arch::X64) {
  return renderX86APXAtomicExpression(A, E, [](const HighExpr &V) {
    return V.Var.Id == 0 ? "address" : V.Var.Id == 1 ? "compare" : "add";
  });
}
llvm::CallInst *call(llvm::IRBuilder<> &B, llvm::Module &M,
                     const X86APXAtomicPlan &P, std::vector<llvm::Value *> Args,
                     bool Native) {
  if (Native) {
    if (P.compare())
      Args.push_back(B.getInt32(P.Condition));
    Args.push_back(B.getInt32(P.AddressBits));
    Args.push_back(B.getInt32(P.SegmentPrefix));
    const auto Id = llvm::Intrinsic::lookupIntrinsicID(P.llvmName());
    return B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(&M, Id), Args);
  }
  std::vector<llvm::Type *> Types;
  for (auto *V : Args)
    Types.push_back(V->getType());
  auto *Return = P.compare() ? B.getIntNTy(P.Bytes * 8) : B.getVoidTy();
  auto *Asm =
      llvm::InlineAsm::get(llvm::FunctionType::get(Return, Types, false),
                           P.assembly(), P.constraints(), true);
  return B.CreateCall(Asm, Args);
}
llvm::Function *wrapper(llvm::Module &M, const Case &C, unsigned Index,
                        bool Native) {
  const auto &P = C.Plan;
  llvm::IRBuilder<> B(M.getContext());
  auto *F = llvm::Function::Create(
      llvm::FunctionType::get(
          B.getInt64Ty(),
          {B.getInt64Ty(), B.getInt64Ty(), B.getInt64Ty(), B.getPtrTy()},
          false),
      llvm::GlobalValue::ExternalLinkage, "apx_" + std::to_string(Index), M);
  B.SetInsertPoint(llvm::BasicBlock::Create(M.getContext(), "entry", F));
  auto *Condition = B.CreateICmpULT(F->getArg(1), F->getArg(2));
  B.CreateStore(B.CreateSelect(Condition, F->getArg(1), F->getArg(2)),
                F->getArg(3));
  auto *Word = B.getIntNTy(P.Bytes * 8);
  std::vector<llvm::Value *> Args = {F->getArg(0)};
  if (P.compare())
    Args.push_back(B.CreateZExtOrTrunc(F->getArg(1), Word));
  Args.push_back(B.CreateZExtOrTrunc(F->getArg(2), Word));
  std::vector<llvm::Value *> Results;
  for (unsigned I = 0; I < (C.Variant == 2 ? 2u : 1u); ++I) {
    auto *Call = call(B, M, P, Args, Native);
    if (P.compare() && C.Variant != 1)
      Results.push_back(B.CreateZExtOrTrunc(Call, B.getInt64Ty()));
  }
  llvm::Value *Result = B.CreateXor(F->getArg(0), F->getArg(1));
  for (auto *V :
       {static_cast<llvm::Value *>(F->getArg(2)),
        B.CreateZExt(Condition, B.getInt64Ty()),
        static_cast<llvm::Value *>(B.CreateLoad(B.getInt64Ty(), F->getArg(3)))})
    Result = B.CreateXor(Result, V);
  for (auto *V : Results)
    Result = B.CreateXor(Result, V);
  B.CreateRet(Result);
  return F;
}
MedFunc med(const X86APXAtomicPlan &P) {
  MedOp Op;
  Op.Addr = kEntry;
  Op.Opcode = NdOp::INTRINSIC;
  Op.MemoryOrdering = P.compare() ? NdMemoryOrdering::SequentiallyConsistent
                                  : NdMemoryOrdering::Relaxed;
  Op.MemoryAddressSpace = high(P)->MemoryAddressSpace;
  if (P.compare())
    Op.Output = temporary(1, P.Bytes);
  Op.addInput(MedVar::makeConst(static_cast<uint64_t>(P.Id), 2));
  Op.addInput(MedVar::makeConst(0x8765432101234567ull, 8));
  Op.addInput(MedVar::makeConst(0x9abcdef0fedcba98ull, P.Bytes));
  if (P.compare()) {
    Op.addInput(MedVar::makeConst(0x12345678abcdef01ull, P.Bytes));
    Op.addInput(MedVar::makeConst(P.Condition, 1));
  }
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  if (P.compare())
    Return.addInput(Op.Output);
  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = kEntry;
  Block.EndAddr = kEntry + 1;
  Block.Ops = {Op, Return};
  MedFunc F;
  F.Name = "apx_med";
  F.Entry = kEntry;
  F.ReturnType =
      P.compare() ? NdType::makeInt(P.Bytes, false) : NdType::makeVoid();
  F.Blocks.push_back(std::move(Block));
  return F;
}
void write(const std::string &Path, llvm::StringRef Text) {
  std::error_code Error;
  llvm::raw_fd_ostream OS(Path, Error);
  ASSERT_FALSE(Error);
  OS << Text;
}

TEST(X86APXAtomicSource, MedEmitterPreservesOperandsAndRefusesMalformedState) {
  for (const auto &C : cases()) {
    if (C.Variant)
      continue;
    llvm::LLVMContext Context;
    const auto F = med(C.Plan);
    auto M = MedLLVMEmitter().emit({F}, Context, "apx", Arch::X64);
    ASSERT_TRUE(M);
    ASSERT_FALSE(llvm::verifyModule(*M, &llvm::errs()));
    unsigned Count = 0;
    for (auto &Function : *M)
      for (auto &Block : Function)
        for (auto &I : Block)
          if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&I))
            if (const auto P = x86APXAtomicCallPlan(Arch::X64, *Call)) {
              ++Count;
              EXPECT_EQ(P->Id, C.Plan.Id);
              EXPECT_EQ(P->Bytes, C.Plan.Bytes);
              EXPECT_EQ(P->SegmentPrefix, C.Plan.SegmentPrefix);
              EXPECT_EQ(P->Condition, C.Plan.Condition);
            }
    EXPECT_EQ(Count, 1u);
  }
  const auto P = *x86APXAtomicPlan(Intrinsic::ApxCmpccXadd, 8, 64, 0, 4);
  for (unsigned Mutation = 0; Mutation < 7; ++Mutation) {
    auto F = med(P);
    auto &Op = F.Blocks[0].Ops[0];
    Arch A = Arch::X64;
    switch (Mutation) {
    case 0:
      A = Arch::X86;
      break;
    case 1:
      Op.MemoryOrdering = NdMemoryOrdering::Relaxed;
      break;
    case 2:
      Op.Inputs[1].Size = 4;
      break;
    case 3:
      Op.Inputs[2].Size = 4;
      break;
    case 4:
      Op.Inputs[4].ConstVal = 16;
      break;
    case 5:
      Op.NumInputs = 3;
      break;
    case 6:
      Op.MemoryAddressSpace = static_cast<NdMemoryAddressSpace>(99);
      break;
    }
    llvm::LLVMContext Context;
    if (Mutation == 6)
      EXPECT_DEATH(MedLLVMEmitter().emit({F}, Context, "invalid", A),
                   "unknown memory address space");
    else
      EXPECT_THROW(MedLLVMEmitter().emit({F}, Context, "invalid", A),
                   std::invalid_argument);
  }
}

TEST(X86APXAtomicSource, SourceRoutesValidateContractsBeforePrinting) {
  for (auto Id : X86APXAtomicInventory)
    for (unsigned Bytes : {4u, 8u}) {
      const auto P = *x86APXAtomicPlan(Id, Bytes, 64, 0);
      for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
        auto E = high(P);
        Arch A = Arch::X64;
        switch (Mutation) {
        case 0:
          A = Arch::AArch64;
          break;
        case 1:
          E->MemoryOrdering = NdMemoryOrdering::None;
          break;
        case 2:
          E->Operands[0]->Type = NdType::makeInt(4, false);
          break;
        case 3:
          E->Operands[1]->Type = NdType::makeInt(2, false);
          break;
        case 4:
          E->Operands.push_back(HighExpr::makeConst(0, 8));
          break;
        case 5:
          E->IsIndirectCall = true;
          break;
        case 6:
          E->MemoryAddressSpace = static_cast<NdMemoryAddressSpace>(99);
          break;
        case 7:
          E->IntrinsicOutputs.push_back(temporary(20, 8));
          break;
        }
        EXPECT_THROW(highExpression(*E, A), std::invalid_argument);
      }
    }
  for (bool Native : {false, true}) {
    auto P = *x86APXAtomicPlan(Intrinsic::ApxCmpccXadd, 8, 32, 0x65, 15);
    if (Native && !llvm::Intrinsic::lookupIntrinsicID(P.llvmName()))
      continue;
    for (unsigned Mutation = 0; Mutation < 7; ++Mutation) {
      llvm::LLVMContext Context;
      llvm::Module M("invalid-apx", Context);
      auto *F = wrapper(M, {P, 0}, 0, Native);
      llvm::CallInst *Call = nullptr;
      for (auto &I : F->getEntryBlock())
        if (auto *Candidate = llvm::dyn_cast<llvm::CallInst>(&I))
          Call = Candidate;
      ASSERT_NE(Call, nullptr);
      Arch A = Arch::X64;
      if (Mutation == 0)
        A = Arch::X86;
      else if (Mutation == 1)
        Call->setDoesNotAccessMemory();
      else if (Mutation == 2)
        Call->addFnAttr(llvm::Attribute::NoSync);
      else if (Mutation == 3)
        Call->setDoesNotReturn();
      else if (Native)
        Call->setArgOperand(
            Mutation == 4   ? 3
            : Mutation == 5 ? 4
                            : 5,
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context),
                                   Mutation == 4   ? 16
                                   : Mutation == 5 ? 16
                                                   : 99));
      else {
        auto *Asm = llvm::cast<llvm::InlineAsm>(Call->getCalledOperand());
        Call->setCalledOperand(
            llvm::InlineAsm::get(Asm->getFunctionType(),
                                 llvm::StringRef(Asm->getAsmString()).str() +
                                     (Mutation == 4 ? "\nnop" : ""),
                                 Mutation == 5 ? "=r,{bx},0,r,~{memory}"
                                               : Asm->getConstraintString(),
                                 Mutation != 6));
      }
      EXPECT_THROW(x86APXAtomicCallPlan(A, *Call), std::invalid_argument);
    }
  }
}

TEST(X86APXAtomicSource, ExportsLiveDeadAndRepeatedTransactions) {
  llvm::LLVMContext Context;
  llvm::Module M("apx-source", Context);
  M.setTargetTriple(llvm::Triple("x86_64-unknown-linux-gnu"));
  const auto Cases = cases();
  std::string High =
      "#include <stdint.h>\n" + std::string(X86APXAtomicCompilerDispatch);
  for (unsigned Index = 0; Index < Cases.size(); ++Index) {
    const auto &C = Cases[Index];
    const auto &P = C.Plan;
    const bool Native = llvm::Intrinsic::lookupIntrinsicID(P.llvmName()) != 0;
    wrapper(M, C, Index, Native);
    auto E = high(P);
    High += "uint64_t apx_" + std::to_string(Index) +
            "(uint64_t address, uint64_t compare, uint64_t add, uint64_t "
            "*ordinary) { "
            "uint64_t condition = compare < add; *ordinary = condition ? "
            "compare : add; ";
    std::string Result = "address ^ compare ^ add ^ condition ^ *ordinary";
    for (unsigned I = 0; I < (C.Variant == 2 ? 2u : 1u); ++I) {
      const auto Name = "old" + std::to_string(I);
      if (P.compare())
        High += "uint64_t " + Name + " = ";
      High += highExpression(*E) + "; ";
      if (P.compare() && C.Variant != 1)
        Result += " ^ " + Name;
    }
    High += "return " + Result + "; }\n";
  }
  ASSERT_FALSE(llvm::verifyModule(M, &llvm::errs()));
  CEmitterOptions O;
  O.TheArch = Arch::X64;
  O.Format = BinaryFormat::ELF;
  std::string Source, IR;
  llvm::raw_string_ostream OS(Source), IROS(IR);
  ASSERT_TRUE(LLVMCEmitter().emit(M, OS, O));
  M.print(IROS, nullptr);
  const auto *Directory = std::getenv("NEVERD_APX_ATOMIC_EXPORT_DIR");
  if (Directory) {
    ASSERT_FALSE(llvm::sys::fs::create_directories(Directory));
    write(std::string(Directory) + "/formats.json",
          "{\"" + std::to_string(unsigned(BinaryFormat::ELF)) +
              "\":\"x86_64-unknown-linux-gnu\",\"" +
              std::to_string(unsigned(BinaryFormat::COFF)) +
              "\":\"x86_64-pc-windows-msvc\",\"" +
              std::to_string(unsigned(BinaryFormat::MachO)) +
              "\":\"x86_64-apple-darwin\"}\n");
    write(std::string(Directory) + "/atomic.ll", IR);
    write(std::string(Directory) + "/atomic-high.c", High);
    write(std::string(Directory) + "/atomic-llvm.c", Source);
  }
  for (auto [Format, Name] : {std::pair{BinaryFormat::ELF, "elf"},
                              {BinaryFormat::COFF, "coff"},
                              {BinaryFormat::MachO, "macho"}}) {
    auto Object = Codegen().compile(M, Arch::X64, Format);
    ASSERT_TRUE(Object.Success) << Name;
    if (Directory)
      write(std::string(Directory) + "/atomic-" + Name + ".o",
            llvm::StringRef(
                reinterpret_cast<const char *>(Object.ObjectData.data()),
                Object.ObjectData.size()));
  }
}

TEST(X86APXAtomicSource, PublicPipelineKeepsFlagsAddressesAndNativeMemory) {
  const char *Directory = std::getenv("NEVERD_APX_ATOMIC_EXPORT_DIR");
  if (Directory)
    ASSERT_FALSE(llvm::sys::fs::create_directories(std::string(Directory) +
                                                   "/pipeline"));
  unsigned Index = 0;
  for (auto Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
    for (const auto &C : cases()) {
      const auto &P = C.Plan;
      if (C.Variant || (P.compare() && P.Condition != 0 && P.Condition != 4 &&
                        P.Condition != 15))
        continue;
      for (bool Address32 : {false, true})
        for (bool NoOpt : {false, true}) {
          SCOPED_TRACE(P.name() + "/" + std::to_string(P.Condition) + "/" +
                       std::to_string(P.SegmentPrefix) + "/" +
                       std::to_string(Address32) + "/" + std::to_string(NoOpt) +
                       "/" + std::to_string(unsigned(Format)));
          std::vector<uint8_t> Bytes;
          const auto Immediate = [&](uint8_t Opcode, uint64_t Value) {
            Bytes.insert(Bytes.end(), {0x48, Opcode});
            for (unsigned I = 0; I != 8; ++I)
              Bytes.push_back(uint8_t(Value >> (8 * I)));
          };
          Immediate(0xb9, 0x8765432100000208ull);        // RCX: source offset
          Immediate(0xb8, 0x0123456789abcdefull);        // RAX: comparator
          Immediate(0xba, 0xfedcba9876543210ull);        // RDX: addend
          Bytes.insert(Bytes.end(), {0x48, 0x39, 0xd0}); // incoming flags
          if (Address32)
            Bytes.push_back(0x67);
          if (P.SegmentPrefix)
            Bytes.push_back(P.SegmentPrefix);
          unsigned PP = P.Id == Intrinsic::ApxRaoAnd   ? 1
                        : P.Id == Intrinsic::ApxRaoOr  ? 3
                        : P.Id == Intrinsic::ApxRaoXor ? 2
                                                       : 0;
          Bytes.insert(Bytes.end(),
                       {0x62, uint8_t(P.compare() ? 0xf2 : 0xf4),
                        uint8_t((P.Bytes == 8 ? 0x80 : 0) |
                                (P.compare() ? 0x6d : 0x7c | PP)),
                        0x08, uint8_t(P.compare() ? 0xe0 | P.Condition : 0xfc),
                        uint8_t(P.compare() ? 0x01 : 0x11)});
          Bytes.insert(Bytes.end(),
                       {0x9f, 0x0f, 0x90, 0xc0, 0x0f, 0xb7, 0xc0, 0xc3});
          BinaryImage Image;
          Image.Arch = Arch::X64;
          Image.Bits = Bitness::Bits64;
          Image.Format = Format;
          Image.Entry = Image.Base = kEntry;
          Segment Text;
          Text.VA = kEntry;
          Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
          Text.Data = Bytes;
          Text.Size = Text.FileSz = Bytes.size();
          Image.Segments.push_back(std::move(Text));
          auto Symbol = Symbol::makeFunc(kEntry, Bytes.size());
          Symbol.Name = "apx_pipeline";
          Image.Symbols.push_back(std::move(Symbol));
          PipelineOptions Options;
          Options.EmitDumpOutput = false;
          Options.NoOpt = NoOpt;
          Options.OnlyFunctionEntries = {kEntry};
          llvm::LLVMContext Context;
          auto R = Pipeline().run(Image, Context, Options);
          ASSERT_TRUE(R.Success) << R.Error;
          auto M = MedLLVMEmitter().emit(R.MedFuncs, Context, "apx-pipeline",
                                         Arch::X64, {}, &Image, Format);
          ASSERT_TRUE(M);
          ASSERT_FALSE(llvm::verifyModule(*M, &llvm::errs()));
          CEmitterOptions COptions;
          COptions.TheArch = Arch::X64;
          COptions.Format = Format;
          COptions.Image = &Image;
          std::string High, LLVM, IR;
          llvm::raw_string_ostream HOS(High), LOS(LLVM), IROS(IR);
          ASSERT_TRUE(HighCEmitter().emit(R.HighFuncs, HOS, COptions));
          Pipeline::promoteScaffoldingAllocas(*M);
          ASSERT_TRUE(LLVMCEmitter().emit(*M, LOS, COptions));
          M->print(IROS, nullptr);
          EXPECT_NE(High.find(P.builtin()), std::string::npos);
          EXPECT_NE(LLVM.find(P.builtin()), std::string::npos);
          EXPECT_EQ(High.find("segmented-memory output requires"),
                    std::string::npos);
          auto Object = Codegen().compile(*M, Arch::X64, Format);
          ASSERT_TRUE(Object.Success) << IR;
          if (Directory) {
            const auto Stem = std::string(Directory) + "/pipeline/" +
                              std::to_string(Index) + "-" +
                              std::to_string(unsigned(Format));
            write(Stem + "-high.c", High);
            write(Stem + "-llvm.c", LLVM);
            write(Stem + ".ll", IR);
            write(Stem + ".o", llvm::StringRef(reinterpret_cast<const char *>(
                                                   Object.ObjectData.data()),
                                               Object.ObjectData.size()));
            write(Stem + ".json",
                  "{\"operation\":\"" + P.operation() + "\",\"bits\":" +
                      std::to_string(P.Bytes * 8) + ",\"address_bits\":" +
                      std::to_string(Address32 ? 32 : 64) +
                      ",\"segment\":" + std::to_string(P.SegmentPrefix) +
                      ",\"condition\":" + std::to_string(P.Condition) + "}\n");
          }
          ++Index;
        }
    }
  EXPECT_EQ(Index, 504u);
}

} // namespace
