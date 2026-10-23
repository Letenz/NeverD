//===- AArch64PAuthSemanticTests.cpp - PAC operand and failure contracts -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independently assembled Armv8.3 words exercise both decoder routes and
/// distinguish signed pointers, raw pointers and authenticated control flow.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/c/render/HighC/HighCIntrinsicRender.h"
#include "neverd/backend/c/render/LLVMC/LLVMCIntrinsicRender.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/intrinsics/AArch64PAuth.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/lift/AArch64Regs.h"
#include "neverd/loader/MachO/MachOLoader.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/IR/Dominators.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Transforms/Utils/PromoteMemToReg.h"

#include <array>
#include <functional>
#include <string>
#include <vector>

using namespace neverd;

namespace {
void optimize(llvm::Module &Module) {
  llvm::LoopAnalysisManager Loops;
  llvm::FunctionAnalysisManager Functions;
  llvm::CGSCCAnalysisManager CallGraph;
  llvm::ModuleAnalysisManager Modules;
  llvm::PassBuilder Passes;
  Passes.registerLoopAnalyses(Loops);
  Passes.registerFunctionAnalyses(Functions);
  Passes.registerCGSCCAnalyses(CallGraph);
  Passes.registerModuleAnalyses(Modules);
  Passes.crossRegisterProxies(Loops, Functions, CallGraph, Modules);
  auto Optimize =
      Passes.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
  Optimize.run(Module, Modules);
}

std::vector<LowOp> lift(uint32_t Word, bool Fast) {
  Decoder Dec;
  EXPECT_TRUE(Dec.init(Arch::AArch64));
  Dec.setStrict(true);
  std::array<uint8_t, 4> Bytes;
  writeLE<uint32_t>(Bytes.data(), Word);
  DecodedInsn Insn;
  EXPECT_EQ(Fast ? Dec.decodeOneForLift(Bytes.data(), 4, 0x1000, Insn)
                 : Dec.decodeOne(Bytes.data(), 4, 0x1000, Insn),
            4);
  std::vector<LowOp> Ops;
  Dec.liftToLow(Insn, Ops);
  return Ops;
}

void expectRegister(const NdVar &V, uint64_t Offset) {
  EXPECT_TRUE(V.isReg());
  EXPECT_EQ(V.Offset, Offset);
  EXPECT_EQ(V.Size, 8);
}

void expectIntrinsic(const LowOp &Op, Intrinsic Id, unsigned Inputs) {
  EXPECT_EQ(Op.Opcode, NdOp::INTRINSIC);
  ASSERT_EQ(Op.NumInputs, Inputs + 1);
  ASSERT_TRUE(Op.Inputs[0].isConst());
  EXPECT_EQ(Op.Inputs[0].Offset, static_cast<uint64_t>(Id));
}

void compileArm64eC(const std::string &Source,
                    const std::function<void(llvm::StringRef)> &Inspect = {}) {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  const auto Found = llvm::sys::findProgramByName("clang");
  if (!Found)
    GTEST_SKIP() << "clang is required for the arm64e source compilation check";
  const std::string Compiler = *Found;
#endif
  llvm::SmallString<128> SourcePath, ObjectPath, ErrorPath;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-arm64e", "c", SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-arm64e", "o", ObjectPath));
  llvm::FileRemover RemoveObject(ObjectPath);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-arm64e", "err", ErrorPath));
  llvm::FileRemover RemoveError(ErrorPath);
  std::error_code FileError;
  {
    llvm::raw_fd_ostream Out(SourcePath, FileError);
    ASSERT_FALSE(FileError);
    Out << Source;
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  // Exclude host libc headers explicitly. Only Clang's resource headers and
  // the fixture's C declarations are needed, including on Linux and Windows.
  for (llvm::StringRef Optimization : {"-O0", "-O2"}) {
    const llvm::StringRef Args[] = {Compiler,
                                    "-target",
                                    "arm64e-apple-macos",
                                    "-ffreestanding",
                                    "-nostdlibinc",
                                    "-I",
                                    NEVERD_ARM64E_TEST_HEADERS,
                                    "-std=gnu11",
                                    Optimization,
                                    "-c",
                                    SourcePath,
                                    "-o",
                                    ObjectPath};
    std::string Error;
    const int Status = llvm::sys::ExecuteAndWait(Compiler, Args, std::nullopt,
                                                 Redirects, 30, 0, &Error);
    const auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Status, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "")
                         << Source;
    if (Inspect)
      Inspect(ObjectPath);
  }
}
} // namespace

TEST(AArch64PAuthSemantics, RegisterAndZeroFormsRetainPointersAndModifiers) {
  const std::array<Intrinsic, 8> Ids = {
      Intrinsic::Pacia, Intrinsic::Pacib, Intrinsic::Pacda, Intrinsic::Pacdb,
      Intrinsic::Autia, Intrinsic::Autib, Intrinsic::Autda, Intrinsic::Autdb};
  for (bool Fast : {false, true})
    for (unsigned I = 0; I < Ids.size(); ++I)
      for (bool Zero : {false, true}) {
        SCOPED_TRACE(testing::Message() << I << " " << Fast << " " << Zero);
        const auto Ops =
            lift((Zero ? 0xdac123e0u : 0xdac10020u) + I * 0x400, Fast);
        ASSERT_EQ(Ops.size(), 1);
        expectIntrinsic(Ops[0], Ids[I], 2);
        expectRegister(Ops[0].Output, a64reg::X0);
        expectRegister(Ops[0].Inputs[1], a64reg::X0);
        if (Zero) {
          EXPECT_TRUE(Ops[0].Inputs[2].isConst());
          EXPECT_EQ(Ops[0].Inputs[2].Offset, 0);
        } else
          expectRegister(Ops[0].Inputs[2], a64reg::X1);
      }
}

TEST(AArch64PAuthSemantics, HintAliasesUseLRSPAnd1716InsteadOfClobberingX0) {
  const struct {
    unsigned Hint;
    Intrinsic Id;
  } Cases[] = {
      {8, Intrinsic::Pacia},  {10, Intrinsic::Pacib}, {12, Intrinsic::Autia},
      {14, Intrinsic::Autib}, {24, Intrinsic::Pacia}, {25, Intrinsic::Pacia},
      {26, Intrinsic::Pacib}, {27, Intrinsic::Pacib}, {28, Intrinsic::Autia},
      {29, Intrinsic::Autia}, {30, Intrinsic::Autib}, {31, Intrinsic::Autib}};
  for (bool Fast : {false, true})
    for (const auto &C : Cases) {
      SCOPED_TRACE(C.Hint);
      const auto Ops = lift(0xd503201f | (C.Hint << 5), Fast);
      ASSERT_EQ(Ops.size(), 1);
      expectIntrinsic(Ops[0], C.Id, 2);
      expectRegister(Ops[0].Output, C.Hint < 16 ? a64reg::X17 : a64reg::X30);
      expectRegister(Ops[0].Inputs[1], C.Hint < 16 ? a64reg::X17 : a64reg::X30);
      if (C.Hint < 16 || (C.Hint & 1))
        expectRegister(Ops[0].Inputs[2],
                       C.Hint < 16 ? a64reg::X16 : a64reg::SP);
      else {
        EXPECT_TRUE(Ops[0].Inputs[2].isConst());
        EXPECT_EQ(Ops[0].Inputs[2].Offset, 0);
      }
    }
}

TEST(AArch64PAuthSemantics,
     StripInstructionsRetainTheirReadModifyWriteOperand) {
  for (bool Fast : {false, true})
    for (uint32_t Word : {0xdac143e0u, 0xdac147e0u, 0xd50320ffu}) {
      const auto Ops = lift(Word, Fast);
      ASSERT_EQ(Ops.size(), 1);
      expectIntrinsic(
          Ops[0], Word == 0xdac147e0 ? Intrinsic::Xpacd : Intrinsic::Xpaci, 1);
      expectRegister(Ops[0].Inputs[1],
                     Word == 0xd50320ff ? a64reg::X30 : a64reg::X0);
      EXPECT_EQ(Ops[0].Output.Offset, Ops[0].Inputs[1].Offset);
    }
}

TEST(AArch64PAuthSemantics, CallsAuthenticateOldLRBeforeWritingTheLink) {
  for (bool Fast : {false, true})
    for (bool KeyB : {false, true})
      for (bool Zero : {false, true}) {
        const auto Ops =
            lift((Zero ? 0xd63f0bdfu : 0xd73f0bdeu) + (KeyB ? 0x400 : 0), Fast);
        ASSERT_EQ(Ops.size(), 3);
        expectIntrinsic(Ops[0], Intrinsic::A64_AuthBranch, 3);
        expectRegister(Ops[0].Inputs[1], a64reg::X30);
        EXPECT_EQ(Ops[0].Inputs[2].Offset, unsigned(KeyB));
        if (!Zero)
          expectRegister(Ops[0].Inputs[3], a64reg::X30);
        else
          EXPECT_EQ(Ops[0].Inputs[3].Offset, 0);
        EXPECT_EQ(Ops[1].Opcode, NdOp::COPY);
        expectRegister(Ops[1].Output, a64reg::X30);
        EXPECT_EQ(Ops[1].Inputs[0].Offset, 0x1004);
        EXPECT_EQ(Ops[2].Opcode, NdOp::INDIR_CALL);
        EXPECT_EQ(Ops[2].Inputs[0], Ops[0].Output);
      }
}

TEST(AArch64PAuthSemantics, BranchesAndReturnsAuthenticateAndTerminate) {
  for (bool Fast : {false, true})
    for (uint32_t Word : {0xd71f0801u, 0xd71f0c01u, 0xd61f081fu, 0xd61f0c1fu,
                          0xd65f0bffu, 0xd65f0fffu}) {
      SCOPED_TRACE(Word);
      const bool Return = (Word & 0xfffff000) == 0xd65f0000;
      const auto Ops = lift(Word, Fast);
      ASSERT_EQ(Ops.size(), 2);
      expectIntrinsic(Ops[0], Intrinsic::A64_AuthBranch, 3);
      EXPECT_EQ(Ops[0].Inputs[2].Offset, (Word >> 10) & 1);
      if (Return) {
        expectRegister(Ops[0].Inputs[1], a64reg::X30);
        expectRegister(Ops[0].Inputs[3], a64reg::SP);
      }
      EXPECT_EQ(Ops[1].Opcode, Return ? NdOp::RETURN : NdOp::INDIR_BR);
      EXPECT_EQ(Ops[1].Inputs[0], Ops[0].Output);
      Decoder Dec;
      ASSERT_TRUE(Dec.init(Arch::AArch64));
      uint8_t Bytes[4];
      writeLE<uint32_t>(Bytes, Word);
      DecodedInsn Insn;
      ASSERT_EQ(Dec.decodeOne(Bytes, 4, 0x1000, Insn), 4);
      EXPECT_TRUE(Dec.isFunctionTerminator(Insn));
    }
}

TEST(AArch64PAuthSemantics,
     AuthenticatedLoadAuthenticatesBaseBeforeOffsetAndWriteback) {
  for (bool Fast : {false, true}) {
    const auto Ops = lift(0xf87fec20u, Fast); // ldraa x0, [x1, #-16]!
    ASSERT_EQ(Ops.size(), 4);
    expectIntrinsic(Ops[0], Intrinsic::Autda, 2);
    expectRegister(Ops[0].Inputs[1], a64reg::X1);
    EXPECT_EQ(Ops[0].Inputs[2].Offset, 0);
    EXPECT_EQ(Ops[1].Opcode, NdOp::INT_ADD);
    EXPECT_EQ(Ops[1].Inputs[0], Ops[0].Output);
    EXPECT_EQ(Ops[1].Inputs[1].Offset, uint64_t(-16));
    EXPECT_EQ(Ops[2].Opcode, NdOp::LOAD);
    EXPECT_EQ(Ops[2].Inputs[0], Ops[1].Output);
    EXPECT_EQ(Ops[3].Opcode, NdOp::COPY);
    expectRegister(Ops[3].Output, a64reg::X1);
    EXPECT_EQ(Ops[3].Inputs[0], Ops[1].Output);
  }
}

TEST(AArch64PAuthSemantics, AuthenticationCannotBeEliminatedAsDead) {
  for (Intrinsic Id : {Intrinsic::Autia, Intrinsic::Autib, Intrinsic::Autda,
                       Intrinsic::Autdb, Intrinsic::A64_AuthBranch})
    EXPECT_TRUE(isSideeffectIntrinsic(Id));
}

TEST(AArch64PAuthSemantics, CRenderersPreserveTiedPointerOperands) {
  for (Intrinsic Id : {Intrinsic::Pacia, Intrinsic::Autia, Intrinsic::Pacdb,
                       Intrinsic::Autdb, Intrinsic::Xpaci, Intrinsic::Xpacd}) {
    const auto Auth = *aarch64PAuthOperation(Id);
    const std::vector<std::string> Args =
        Auth.Strip ? std::vector<std::string>{"pointer"}
                   : std::vector<std::string>{"pointer", "modifier"};
    bool HasIntrinsics = false;
    const auto High = renderARMIntrinsicCall(Id, Args, 8, HasIntrinsics);
    EXPECT_NE(High.find("\"+r\"(__pointer)"), std::string::npos) << High;
    const auto Low = renderARMInlineAsm(Arch::AArch64,
                                        std::string(Auth.Mnemonic) +
                                            (Auth.Strip ? " $0" : " $0, $2"),
                                        Auth.Mnemonic, "result", true, Args);
    EXPECT_NE(Low.Code.find("\"+r\"(__pointer)"), std::string::npos);
    EXPECT_NE(Low.Code.find("result = __pointer"), std::string::npos);
  }
}

TEST(AArch64PAuthSemantics, LLVMAndBothCRoutesRetainMachineAuthentication) {
  for (Intrinsic Id :
       {Intrinsic::Pacia, Intrinsic::Pacib, Intrinsic::Pacda, Intrinsic::Pacdb,
        Intrinsic::Autia, Intrinsic::Autib, Intrinsic::Autda, Intrinsic::Autdb,
        Intrinsic::Xpaci, Intrinsic::Xpacd, Intrinsic::A64_AuthBranch}) {
    SCOPED_TRACE(intrinsicName(Id));
    MedFunc F;
    F.Name = "authenticate_pointer";
    F.Entry = 0x1000;
    F.ReturnType = NdType::makeInt(8, false);
    for (int Index = 0; Index < 2; ++Index) {
      MedVar V;
      V.Kind = MedVar::Param;
      V.TheArch = Arch::AArch64;
      V.Id = Index;
      V.RegOff = Index * 8;
      V.Size = 8;
      F.Params.push_back(V);
    }
    MedOp Auth;
    Auth.Opcode = NdOp::INTRINSIC;
    Auth.Output.Kind = MedVar::Reg;
    Auth.Output.TheArch = Arch::AArch64;
    Auth.Output.Id = 2;
    Auth.Output.SSAVer = 1;
    Auth.Output.Size = 8;
    Auth.Output.RegOff = a64reg::X0;
    Auth.addInput(MedVar::makeConst(static_cast<uint64_t>(Id), 4));
    Auth.addInput(F.Params[0]);
    if (Id == Intrinsic::A64_AuthBranch)
      Auth.addInput(MedVar::makeConst(1, 4));
    if (Id == Intrinsic::A64_AuthBranch || !aarch64PAuthOperation(Id)->Strip)
      Auth.addInput(F.Params[1]);
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.addInput(Auth.Output);
    F.Blocks.emplace_back();
    F.Blocks[0].Id = 0;
    F.Blocks[0].StartAddr = F.Entry;
    F.Blocks[0].Ops = {Auth, Return};
    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit({F}, Context, "pauth", Arch::AArch64);
    ASSERT_TRUE(Module);
    EXPECT_FALSE(llvm::verifyModule(*Module));
    const auto *Fn = Module->getFunction(F.Name);
    ASSERT_TRUE(Fn);
    unsigned AuthCalls = 0;
    for (const auto &Block : *Fn)
      for (const auto &Insn : Block)
        if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Insn)) {
          if (Id == Intrinsic::A64_AuthBranch) {
            if (const auto *Callee = Call->getCalledFunction();
                Callee &&
                Callee->getIntrinsicID() == llvm::Intrinsic::ptrauth_auth)
              ++AuthCalls;
          } else if (const auto *Asm = llvm::dyn_cast<llvm::InlineAsm>(
                         Call->getCalledOperand())) {
            EXPECT_EQ(Asm->getConstraintString(),
                      aarch64PAuthOperation(Id)->Strip ? "=r,0" : "=r,0,r");
            EXPECT_TRUE(Asm->hasSideEffects());
            ++AuthCalls;
          }
        }
    EXPECT_EQ(AuthCalls, 1);
    CEmitterOptions Options;
    Options.TheArch = Arch::AArch64;
    Options.Format = BinaryFormat::MachO;
    std::string LowC, HighC;
    llvm::raw_string_ostream LowOS(LowC), HighOS(HighC);
    EXPECT_TRUE(LLVMCEmitter().emit(*Module, LowOS, Options));
    auto High = MedToHighConverter().convert(F, Arch::AArch64);
    EXPECT_TRUE(HighCEmitter().emit({High}, HighOS, Options));
    for (const std::string *C : {&LowC, &HighC}) {
      EXPECT_NE(C->find(Id == Intrinsic::A64_AuthBranch
                            ? "__builtin_ptrauth_auth"
                            : aarch64PAuthOperation(Id)->Mnemonic),
                std::string::npos)
          << *C;
      EXPECT_EQ(C->find("unknown_intrinsic"), std::string::npos) << *C;
      compileArm64eC(*C);
    }
    auto Object =
        Codegen().compile(*Module, Arch::AArch64, BinaryFormat::MachO);
    EXPECT_TRUE(Object.Success);
  }
}

TEST(AArch64PAuthSemantics,
     CompilerReturnSequenceKeepsAuthenticationAndX0Result) {
  // pacibsp; mov x0, #7; retab. RETAB's LR is control flow, never the C result.
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 12;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(12);
  writeLE<uint32_t>(Text.Data.data(), 0xd503237f);
  writeLE<uint32_t>(Text.Data.data() + 4, 0xd28000e0);
  writeLE<uint32_t>(Text.Data.data() + 8, 0xd65f0fff);
  Image.Segments.push_back(Text);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::AArch64));
  Dec.setStrict(true);
  auto Low = CFGBuilder().build(Image, Dec, 0x1000, "signed_return");
  LowToMedConverter Converter;
  Converter.setBinaryImage(&Image);
  auto Med = Converter.convert(Low, Arch::AArch64, BinaryFormat::MachO);
  unsigned Auths = 0;
  for (const auto &Block : Med.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
          Op.Inputs[0].ConstVal ==
              static_cast<uint64_t>(Intrinsic::A64_AuthBranch))
        ++Auths;
  EXPECT_EQ(Auths, 1);
  llvm::LLVMContext Context;
  auto Module =
      MedLLVMEmitter().emit({Med}, Context, "signed-return", Arch::AArch64);
  ASSERT_TRUE(Module);
  EXPECT_FALSE(llvm::verifyModule(*Module));
  auto *Fn = Module->getFunction("signed_return");
  ASSERT_TRUE(Fn);
  llvm::SmallVector<llvm::AllocaInst *> Allocas;
  for (auto &Insn : Fn->getEntryBlock())
    if (auto *Alloca = llvm::dyn_cast<llvm::AllocaInst>(&Insn))
      if (llvm::isAllocaPromotable(Alloca))
        Allocas.push_back(Alloca);
  llvm::DominatorTree DT(*Fn);
  llvm::PromoteMemToReg(Allocas, DT);
  unsigned Returns = 0;
  for (const auto &Block : *Fn)
    if (const auto *Ret =
            llvm::dyn_cast<llvm::ReturnInst>(Block.getTerminator())) {
      const auto *Value =
          llvm::dyn_cast_or_null<llvm::ConstantInt>(Ret->getReturnValue());
      ASSERT_TRUE(Value);
      EXPECT_EQ(Value->getZExtValue(), 7);
      ++Returns;
    }
  EXPECT_EQ(Returns, 1);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  Options.Format = BinaryFormat::MachO;
  std::string LowC, HighC;
  llvm::raw_string_ostream LowOS(LowC), HighOS(HighC);
  EXPECT_TRUE(LLVMCEmitter().emit(*Module, LowOS, Options));
  auto High = MedToHighConverter().convert(Med, Arch::AArch64);
  EXPECT_TRUE(HighCEmitter().emit({High}, HighOS, Options));
  for (const std::string *C : {&LowC, &HighC}) {
    EXPECT_NE(C->find("__builtin_ptrauth_auth"), std::string::npos) << *C;
    compileArm64eC(*C);
  }
}

TEST(AArch64PAuthSemantics, ExceptionReturnsNeverLoseAuthentication) {
  for (bool Fast : {false, true})
    for (uint32_t Word : {0xd69f0bffu, 0xd69f0fffu})
      EXPECT_THROW(lift(Word, Fast), UnliftedInstruction);
}

TEST(AArch64PAuthSemantics, AuthenticatedTailCallsRequireARestoredFrame) {
  const struct {
    bool TailCall;
    std::vector<uint32_t> Words;
  } Cases[] = {
      {true, {0xd61f081f}},                          // braaz x0
      {true, {0xd71f081f}},                          // braa x0, sp
      {true, {0xd503237f, 0xd50323ff, 0xd61f081f}},  // matching PACIBSP/AUTIBSP
      {false, {0xd10043ff, 0xd61f081f}},             // live frame
      {false, {0x910003e0, 0xd61f081f}},             // stack target
      {false, {0x10000020, 0xd61f081f}},             // local code target
      {false, {0xd503237f, 0xd61f081f}},             // link still signed
      {false, {0xd503237f, 0xd50323bf, 0xd61f081f}}, // wrong authentication key
      {false,
       {0xd503237f, 0xd10043ff, 0xd50323ff, 0x910043ff,
        0xd61f081f}},        // wrong modifier
      {false, {0xd61f0bdf}}, // braaz x30 returns
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(testing::Message() << Case.Words.front());
    BinaryImage Image;
    Image.Arch = Arch::AArch64;
    Image.Format = BinaryFormat::MachO;
    Image.Bits = Bitness::Bits64;
    Segment Text;
    Text.VA = 0x1000;
    Text.Size = Text.FileSz = Case.Words.size() * 4;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(Text.Size);
    for (size_t I = 0; I < Case.Words.size(); ++I)
      writeLE<uint32_t>(Text.Data.data() + I * 4, Case.Words[I]);
    Image.Segments.push_back(Text);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::AArch64));
    Dec.setStrict(true);
    auto Low = CFGBuilder().build(Image, Dec, Text.VA, "authenticated_tail");
    unsigned Calls = 0, Branches = 0;
    for (const auto &Block : Low.Blocks)
      for (const auto &Op : Block.Ops) {
        Calls += Op.Opcode == NdOp::INDIR_CALL;
        Branches += Op.Opcode == NdOp::INDIR_BR;
      }
    EXPECT_EQ(Calls, Case.TailCall ? 1u : 0u);
    EXPECT_EQ(Branches, Case.TailCall ? 0u : 1u);
  }
}

TEST(AArch64PAuthSemantics,
     CompilerGeneratedArm64eObjectRetainsAuthentication) {
  compileArm64eC(
      R"(
typedef unsigned long long word;
__attribute__((noinline)) word target(word x) { return x + 1; }
word (*const slot)(word) = target;
__attribute__((noinline)) word dispatch(word (*fn)(word), word x) {
  return fn(x);
}
__attribute__((noinline)) word dispatch_after_call(word (*fn)(word), word x) {
  return fn(target(x));
}
)",
      [](llvm::StringRef Path) {
        auto Image = MachOLoader().load(Path.str());
        ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
        ASSERT_TRUE(Image->isArm64e());
        ASSERT_FALSE(Image->MachOAuthenticatedPointers.empty());
        const auto *Dispatch = Image->findSymbol("_dispatch");
        ASSERT_TRUE(Dispatch);
        Decoder Dec;
        ASSERT_TRUE(Dec.init(Arch::AArch64));
        Dec.setStrict(true);
        auto Low = CFGBuilder().build(*Image, Dec, Dispatch->Addr, "dispatch");
        LowToMedConverter Converter;
        Converter.setBinaryImage(&*Image);
        auto Med = Converter.convert(Low, Arch::AArch64, BinaryFormat::MachO);
        recoverCallAbi(Med, Arch::AArch64, {}, &*Image);
        unsigned Authentication = 0;
        for (const auto &Block : Med.Blocks)
          for (const auto &Op : Block.Ops)
            if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
                Op.Inputs[0].ConstVal ==
                    static_cast<uint64_t>(Intrinsic::A64_AuthBranch))
              ++Authentication;
        EXPECT_GT(Authentication, 0);
        llvm::LLVMContext Context;
        auto Module =
            MedLLVMEmitter().emit({Med}, Context, "clang-arm64e", Arch::AArch64,
                                  {}, &*Image, BinaryFormat::MachO);
        ASSERT_TRUE(Module);
        EXPECT_FALSE(llvm::verifyModule(*Module));
        optimize(*Module);
        const auto *Fn = Module->getFunction("dispatch");
        ASSERT_TRUE(Fn);
        ASSERT_EQ(Fn->arg_size(), 2u);
        unsigned IndirectCalls = 0;
        for (const auto &Block : *Fn)
          for (const auto &Insn : Block)
            if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Insn);
                Call && Call->isIndirectCall() && !Call->isInlineAsm()) {
              // Without a prototype, ABI recovery may retain extra live
              // registers. The explicit X0 argument must still be x.
              ASSERT_GE(Call->arg_size(), 1u);
              EXPECT_EQ(Call->getArgOperand(0), Fn->getArg(1));
              ++IndirectCalls;
            }
        EXPECT_EQ(IndirectCalls, 1);
        const auto *AfterCall = Image->findSymbol("_dispatch_after_call");
        ASSERT_TRUE(AfterCall);
        auto NonLeaf = CFGBuilder().build(*Image, Dec, AfterCall->Addr,
                                          "dispatch_after_call");
        unsigned TailCalls = 0, UnresolvedBranches = 0;
        for (const auto &Block : NonLeaf.Blocks)
          for (const auto &Op : Block.Ops) {
            TailCalls += Op.Opcode == NdOp::INDIR_CALL;
            UnresolvedBranches += Op.Opcode == NdOp::INDIR_BR;
          }
        EXPECT_EQ(TailCalls, 1);
        EXPECT_EQ(UnresolvedBranches, 0);
        auto NonLeafMed =
            Converter.convert(NonLeaf, Arch::AArch64, BinaryFormat::MachO);
        const auto *Target = Image->findSymbol("_target");
        ASSERT_TRUE(Target);
        ASSERT_EQ(Target->Addr, 0u);
        const std::vector<std::pair<va_t, std::string>> Names = {
            {Target->Addr, Target->Name}};
        recoverCallAbi(NonLeafMed, Arch::AArch64,
                       {{Target->Addr, Target->Name}}, &*Image);
        auto NonLeafModule = MedLLVMEmitter().emit(
            {NonLeafMed}, Context, "clang-arm64e-nonleaf", Arch::AArch64, Names,
            &*Image, BinaryFormat::MachO);
        ASSERT_TRUE(NonLeafModule);
        optimize(*NonLeafModule);
        EXPECT_FALSE(llvm::verifyModule(*NonLeafModule));
        EXPECT_TRUE(NonLeafModule->getFunction("target"));
        const auto *NonLeafFn =
            NonLeafModule->getFunction("dispatch_after_call");
        ASSERT_TRUE(NonLeafFn);
        unsigned ForwardedCalls = 0;
        for (const auto &Block : *NonLeafFn)
          for (const auto &Insn : Block)
            if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Insn);
                Call && Call->isIndirectCall() && !Call->isInlineAsm()) {
              ASSERT_GE(Call->arg_size(), 1u);
              const auto *Forwarded =
                  llvm::dyn_cast<llvm::CallBase>(Call->getArgOperand(0));
              ASSERT_TRUE(Forwarded);
              ASSERT_TRUE(Forwarded->getCalledFunction());
              EXPECT_EQ(Forwarded->getCalledFunction()->getName(), "target");
              ++ForwardedCalls;
            }
        EXPECT_EQ(ForwardedCalls, 1);
        CEmitterOptions Options;
        Options.TheArch = Arch::AArch64;
        Options.Format = BinaryFormat::MachO;
        std::string LowC, HighC;
        llvm::raw_string_ostream LowOS(LowC), HighOS(HighC);
        ASSERT_TRUE(LLVMCEmitter().emit(*NonLeafModule, LowOS, Options));
        const auto High =
            MedToHighConverter().convert(NonLeafMed, Arch::AArch64);
        ASSERT_TRUE(HighCEmitter().emit({High}, HighOS, Options));
        EXPECT_NE(HighC.find("__builtin_return_address(0)"), std::string::npos);
        EXPECT_EQ(HighC.find("unknown register"), std::string::npos);
        compileArm64eC(LowC);
        compileArm64eC(HighC);
      });
}
