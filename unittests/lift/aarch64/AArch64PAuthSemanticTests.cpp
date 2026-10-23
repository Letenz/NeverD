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
#include "neverd/ir/intrinsics/AArch64CPA.h"
#include "neverd/ir/intrinsics/AArch64PAuth.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowIntrinsicEffects.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/lift/AArch64Regs.h"
#include "neverd/loader/MachO/MachOLoader.h"
#include "neverd/loader/MachO/MachOMetadataPointer.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/IR/Dominators.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Transforms/Utils/PromoteMemToReg.h"

#include <array>
#include <functional>
#include <string>
#include <vector>

#if defined(__APPLE__) && defined(__aarch64__)
#include <sys/sysctl.h>
#endif

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
                    const std::function<void(llvm::StringRef)> &Inspect = {},
                    bool EmitLLVM = false, llvm::StringRef ExecuteTarget = {},
                    bool *ExecutionAvailable = nullptr,
                    llvm::StringRef Language = "c") {
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
  const bool ExecuteNative = !ExecuteTarget.empty();
  for (llvm::StringRef Optimization : {"-O0", "-O2"}) {
    llvm::SmallVector<llvm::StringRef> Args = {
        Compiler,
        "-target",
        ExecuteNative ? ExecuteTarget : "arm64e-apple-macos",
        "-ffreestanding",
        "-nostdlibinc",
        "-I",
        NEVERD_ARM64E_TEST_HEADERS,
        "-std=gnu11",
        "-Werror=unused-value",
        "-x",
        Language,
        Optimization,
        SourcePath,
        "-o",
        ObjectPath};
    if (EmitLLVM)
      Args.append({"-S", "-emit-llvm"});
    else if (ExecuteNative)
      Args.push_back("-march=armv8.3-a+pauth");
    else
      Args.push_back("-c");
    std::string Error;
    const int Status = llvm::sys::ExecuteAndWait(Compiler, Args, std::nullopt,
                                                 Redirects, 30, 0, &Error);
    const auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Status, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "")
                         << Source;
    if (Inspect)
      Inspect(ObjectPath);
    if (ExecuteNative) {
      const llvm::StringRef RunArgs[] = {ObjectPath};
      const int Result = llvm::sys::ExecuteAndWait(
          ObjectPath, RunArgs, std::nullopt, Redirects, 30, 0, &Error);
      if (ExecutionAvailable) {
        *ExecutionAvailable = Result == 0;
        return;
      }
      EXPECT_EQ(Result, 0) << Error << Source;
    }
  }
}

MedFunc pointerFunction(Intrinsic Id, llvm::StringRef Name) {
  MedFunc F;
  F.Name = Name.str();
  F.Entry = 0x1000;
  F.ReturnType = NdType::makeInt(8, false);
  const auto Checked = aarch64CPAOperation(Id);
  const unsigned Params = Checked ? Checked->Sources : 2;
  for (unsigned Index = 0; Index < Params; ++Index) {
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
  if (Checked || Id == Intrinsic::A64_AuthBranch ||
      Id == Intrinsic::A64_Pacga || !aarch64PAuthOperation(Id)->Strip)
    Auth.addInput(F.Params[1]);
  if (Checked && Checked->Sources == 3)
    Auth.addInput(F.Params[2]);
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.addInput(Auth.Output);
  F.Blocks.emplace_back();
  F.Blocks[0].Id = 0;
  F.Blocks[0].StartAddr = F.Entry;
  F.Blocks[0].Ops = {Auth, Return};
  return F;
}
} // namespace

TEST(AArch64PAuthSemantics, CompilerObjCRecipesRetainLocalMetadataTargets) {
  const char Source[] = R"(
    __attribute__((objc_root_class))
    @interface ArmMetadata
    - (int)increment:(int)value;
    @end
    @implementation ArmMetadata
    - (int)increment:(int)value { return value + 1; }
    @end
  )";
  compileArm64eC(
      Source,
      [](llvm::StringRef Path) {
        auto Buffer = llvm::MemoryBuffer::getFile(Path);
        ASSERT_TRUE(Buffer);
        auto Image = MachOLoader().loadBuffer((*Buffer)->getMemBufferRef());
        ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
        EXPECT_TRUE(Image->isArm64e());
        EXPECT_FALSE(Image->MachOAuthenticatedPointers.empty());
        bool ClassRO = false, MetaclassRO = false, Implementation = false;
        for (const auto &[Slot, Recipe] : Image->MachOAuthenticatedPointers) {
          ASSERT_TRUE(Recipe.EncodedValue);
          EXPECT_EQ(readLE<uint64_t>(Image->readVA(Slot, 8)),
                    *Recipe.EncodedValue);
          EXPECT_FALSE(Image->CodePtrRelocSlots.count(Slot));
          EXPECT_EQ(macho_loader::readMetadataPointerTarget(*Image, Slot),
                    Recipe.Target);
          ClassRO |= Recipe.Symbol == "__OBJC_CLASS_RO_$_ArmMetadata";
          MetaclassRO |= Recipe.Symbol == "__OBJC_METACLASS_RO_$_ArmMetadata";
          if (Recipe.Symbol == "-[ArmMetadata increment:]") {
            Implementation = true;
            EXPECT_TRUE(Image->isCodeAddress(Recipe.Target));
          }
        }
        EXPECT_TRUE(ClassRO && MetaclassRO && Implementation);
        // Source type recovery still requires a linked image. Loading an
        // object's recipes does not prove its runtime class/dispatch layout.
        EXPECT_TRUE(Image->ObjCClasses.empty());
        EXPECT_TRUE(Image->ObjCMethods.empty());
#if defined(__APPLE__) && defined(__aarch64__)
        // Apple's linker supplies a real linked metadata graph. Strip local
        // symbols so class/method recovery must follow the signing recipes.
        const auto Driver = llvm::sys::findProgramByName("clang");
        ASSERT_TRUE(Driver);
        llvm::SmallString<128> BundlePath;
        ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-arm64e-objc",
                                                        "bundle", BundlePath));
        llvm::FileRemover RemoveBundle(BundlePath);
        const llvm::StringRef Args[] = {*Driver,
                                        "-target",
                                        "arm64e-apple-macos",
                                        "-bundle",
                                        "-Wl,-undefined,dynamic_lookup",
                                        "-Wl,-x",
                                        Path,
                                        "-o",
                                        BundlePath};
        std::string Error;
        ASSERT_EQ(llvm::sys::ExecuteAndWait(*Driver, Args, std::nullopt, {}, 30,
                                            0, &Error),
                  0)
            << Error;
        auto BundleBytes = llvm::MemoryBuffer::getFile(BundlePath);
        ASSERT_TRUE(BundleBytes);
        auto Bundle =
            MachOLoader().loadBuffer((*BundleBytes)->getMemBufferRef());
        ASSERT_TRUE(bool(Bundle)) << llvm::toString(Bundle.takeError());
        EXPECT_FALSE(Bundle->IsRelocatable);
        ASSERT_EQ(Bundle->ObjCClasses.size(), 1);
        ASSERT_EQ(Bundle->ObjCMethods.size(), 1);
        const auto &Method = Bundle->ObjCMethods[0];
        EXPECT_EQ(Method.ClassName, "ArmMetadata");
        EXPECT_EQ(Method.Selector, "increment:");
        EXPECT_EQ(Method.Status, "supported");
        EXPECT_TRUE(Bundle->isCodeAddress(Method.Implementation));
        EXPECT_EQ(Bundle->getFunctionNameAt(Method.Implementation),
                  "-[ArmMetadata increment:]");
        EXPECT_EQ(Bundle->statedFunctionNameAt(Method.Implementation),
                  "-[ArmMetadata increment:]");
        const Symbol *Name = Bundle->findSymbolAt(Method.Implementation);
        ASSERT_TRUE(Name);
        EXPECT_EQ(Name->Name, "-[ArmMetadata increment:]");
        EXPECT_EQ(Name->Origin, NameOrigin::Stated);
        ASSERT_TRUE(Method.TypeHint);
#endif
      },
      false, {}, nullptr, "objective-c");
}

TEST(AArch64CPASemantics, ScalarOperandsRetainChecksThroughBothDecoders) {
  // Independently assembled with llvm-mc -mattr=+cpa. The multiply-add's
  // final input is its pointer base, not another multiplier.
  const struct {
    uint32_t Word;
    Intrinsic Id;
  } Cases[] = {{0x9a022020, Intrinsic::A64_Addpt},
               {0xda022020, Intrinsic::A64_Subpt},
               {0x9b620c20, Intrinsic::A64_Maddpt},
               {0x9b628c20, Intrinsic::A64_Msubpt}};
  for (bool Fast : {false, true}) {
    for (const auto &Case : Cases) {
      SCOPED_TRACE(testing::Message() << Case.Word << " " << Fast);
      const auto Ops = lift(Case.Word, Fast);
      ASSERT_EQ(Ops.size(), 1);
      const unsigned Inputs = aarch64CPAOperation(Case.Id)->Sources;
      expectIntrinsic(Ops[0], Case.Id, Inputs);
      expectRegister(Ops[0].Output, a64reg::X0);
      expectRegister(Ops[0].Inputs[1], a64reg::X1);
      expectRegister(Ops[0].Inputs[2], a64reg::X2);
      if (Inputs == 3)
        expectRegister(Ops[0].Inputs[3], a64reg::X3);
      EXPECT_TRUE(lowAArch64CPAShapeIsValid(Ops[0]));
      EXPECT_TRUE(isSideeffectIntrinsic(Case.Id));
      for (unsigned Index = 0; Index <= Inputs; ++Index) {
        auto Bad = Ops[0];
        (Index ? Bad.Inputs[Index] : Bad.Output).Size = 4;
        EXPECT_FALSE(lowAArch64CPAShapeIsValid(Bad));
      }
      auto Bad = Ops[0];
      --Bad.NumInputs;
      EXPECT_FALSE(lowAArch64CPAShapeIsValid(Bad));
    }
    const auto Shifted = lift(0x9a023fff, Fast); // addpt sp, sp, x2, lsl #7
    ASSERT_EQ(Shifted.size(), 2);
    EXPECT_EQ(Shifted[0].Opcode, NdOp::INT_LEFT);
    expectRegister(Shifted[0].Inputs[0], a64reg::X2);
    EXPECT_EQ(Shifted[0].Inputs[1].Offset, 7);
    expectIntrinsic(Shifted[1], Intrinsic::A64_Addpt, 2);
    expectRegister(Shifted[1].Output, a64reg::SP);
    expectRegister(Shifted[1].Inputs[1], a64reg::SP);
    EXPECT_EQ(Shifted[1].Inputs[2], Shifted[0].Output);

    const auto Alias = lift(0xda012c21, Fast); // subpt x1, x1, x1, lsl #3
    ASSERT_EQ(Alias.size(), 2);
    expectRegister(Alias[0].Inputs[0], a64reg::X1);
    expectRegister(Alias[1].Output, a64reg::X1);
    expectRegister(Alias[1].Inputs[1], a64reg::X1);
    EXPECT_EQ(Alias[1].Inputs[2], Alias[0].Output);

    const auto Zero = lift(0x9b7f7fff, Fast); // maddpt xzr, xzr, xzr, xzr
    ASSERT_EQ(Zero.size(), 1);
    expectIntrinsic(Zero[0], Intrinsic::A64_Maddpt, 3);
    for (unsigned I = 1; I < 4; ++I) {
      EXPECT_TRUE(Zero[0].Inputs[I].isConst());
      EXPECT_EQ(Zero[0].Inputs[I].Offset, 0);
    }
    // Vector ADDPT/SUBPT/MADPT/MLAPT cannot fall through to integer or
    // fixed-width vector arithmetic while scalable state is unmodelled.
    for (uint32_t Word : {0x04e20820u, 0x04e20c20u, 0x44c1d840u, 0x44c2d020u})
      EXPECT_THROW(lift(Word, Fast), UnliftedInstruction);
  }
}

TEST(AArch64CPASemantics, MalformedShapesCannotPreserveCallArguments) {
  for (const auto &Operation : kAArch64CPAOperations) {
    const auto Function = pointerFunction(Operation.Id, "checked_pointer");
    const auto &Op = Function.Blocks[0].Ops[0];
    EXPECT_TRUE(aarch64CPAMedShapeIsValid(Op));
    EXPECT_FALSE(endsCallArgumentSetup(Op));
    for (unsigned Index = 0; Index <= Operation.Sources; ++Index) {
      auto Bad = Op;
      (Index ? Bad.Inputs[Index] : Bad.Output).Size = 4;
      EXPECT_FALSE(aarch64CPAMedShapeIsValid(Bad));
      EXPECT_TRUE(endsCallArgumentSetup(Bad));
    }
    for (unsigned Invalid = 0; Invalid < 4; ++Invalid) {
      auto Bad = Op;
      if (Invalid == 0)
        --Bad.NumInputs;
      else if (Invalid == 1)
        Bad.IntrinsicOutputs.push_back(Bad.Output);
      else if (Invalid == 2)
        Bad.Output = MedVar::makeConst(0, 8);
      else
        Bad.MemoryOrdering = NdMemoryOrdering::Acquire;
      EXPECT_FALSE(aarch64CPAMedShapeIsValid(Bad));
      EXPECT_TRUE(endsCallArgumentSetup(Bad));
    }
    bool HasIntrinsics = false;
    const std::vector<std::string> Args(Operation.Sources, "value");
    EXPECT_THROW(renderARMIntrinsicCall(Operation.Id, Args, 4, HasIntrinsics),
                 std::invalid_argument);
    EXPECT_THROW(renderARMIntrinsicCall(Operation.Id, {}, 8, HasIntrinsics),
                 std::invalid_argument);
    EXPECT_THROW(renderARMInlineAsm(Arch::AArch64, Operation.MachineAsm, "",
                                    "result", true, {}),
                 std::invalid_argument);
  }
}

TEST(AArch64PAuthSemantics, MalformedPointerShapesFailIRVerification) {
  const struct {
    uint32_t Word;
    Intrinsic Id;
  } Cases[] = {
      {0xdac10020, Intrinsic::Pacia},     {0xdac11020, Intrinsic::Autia},
      {0xdac143e0, Intrinsic::Xpaci},     {0x9ac23020, Intrinsic::A64_Pacga},
      {0x9a022020, Intrinsic::A64_Addpt}, {0x9b620c20, Intrinsic::A64_Maddpt}};
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Word);
    auto Ops = lift(Case.Word, false);
    ASSERT_EQ(Ops.size(), 1);
    LowBlock Block;
    Block.StartAddr = 0x1000;
    Block.EndAddr = 0x1004;
    Block.Ops = Ops;
    LowInstructionBoundary Boundary;
    Boundary.Address = Block.StartAddr;
    Boundary.Size = 4;
    Boundary.OpCount = 1;
    Block.InstructionBoundaries.push_back(Boundary);
    auto Valid = validateLowInstructionBoundaries(Block);
    EXPECT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
    for (bool BadOutput : {false, true}) {
      Block.Ops = Ops;
      (BadOutput ? Block.Ops[0].Output : Block.Ops[0].Inputs[1]).Size = 4;
      auto Error = validateLowInstructionBoundaries(Block);
      EXPECT_TRUE(bool(Error));
      llvm::consumeError(std::move(Error));
    }
  }
  for (Intrinsic Id : {Intrinsic::Pacia, Intrinsic::Autia, Intrinsic::Xpaci,
                       Intrinsic::A64_Pacga, Intrinsic::A64_AuthBranch,
                       Intrinsic::A64_Addpt, Intrinsic::A64_Maddpt}) {
    const auto Function = pointerFunction(Id, "pointer_shape");
    EXPECT_TRUE(verifyMedFunc(Function, "pointer-shape"));
    for (bool BadOutput : {false, true}) {
      auto Bad = Function;
      auto &Op = Bad.Blocks[0].Ops[0];
      (BadOutput ? Op.Output : Op.Inputs[1]).Size = 4;
      EXPECT_FALSE(verifyMedFunc(Bad, "bad-pointer-shape"));
    }
  }
}

TEST(AArch64CPASemantics, LLVMAndBothCRoutesKeepExactInstructions) {
  const auto CheckObject = [](llvm::MemoryBufferRef Buffer, Intrinsic Id) {
    // Independently assembled fixed-register forms, not read from the
    // production instruction table. This also catches a misplaced operand.
    uint32_t Expected = 0;
    switch (Id) {
    case Intrinsic::A64_Addpt:
      Expected = 0x9a112210;
      break;
    case Intrinsic::A64_Subpt:
      Expected = 0xda112210;
      break;
    case Intrinsic::A64_Maddpt:
      Expected = 0x9b713e10;
      break;
    case Intrinsic::A64_Msubpt:
      Expected = 0x9b71be10;
      break;
    default:
      FAIL() << "unexpected checked instruction";
    }
    auto Image = MachOLoader().loadBuffer(Buffer);
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    unsigned Count = 0;
    for (const auto &Segment : Image->Segments)
      if (Segment.isExecutable())
        for (size_t Offset = 0; Offset + 4 <= Segment.Data.size(); Offset += 4)
          Count += readLE<uint32_t>(Segment.Data.data() + Offset) == Expected;
    EXPECT_EQ(Count, 1);
  };
  for (const auto &Operation : kAArch64CPAOperations)
    for (bool Optimize : {false, true}) {
      SCOPED_TRACE(testing::Message()
                   << intrinsicName(Operation.Id) << " " << Optimize);
      const auto Function = pointerFunction(Operation.Id, "checked_pointer");
      llvm::LLVMContext Context;
      auto Module =
          MedLLVMEmitter().emit({Function}, Context, "cpa", Arch::AArch64);
      ASSERT_TRUE(Module);
      if (Optimize)
        optimize(*Module);
      ASSERT_FALSE(llvm::verifyModule(*Module));
      unsigned Count = 0;
      for (const auto &Block : *Module->getFunction(Function.Name))
        for (const auto &Insn : Block)
          if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Insn))
            if (const auto *Asm =
                    llvm::dyn_cast<llvm::InlineAsm>(Call->getCalledOperand())) {
              EXPECT_EQ(Asm->getAsmString(), Operation.MachineAsm);
              EXPECT_EQ(Asm->getConstraintString(),
                        Operation.llvmConstraints());
              EXPECT_TRUE(Asm->hasSideEffects());
              EXPECT_EQ(Call->arg_size(), Operation.Sources);
              if (Optimize)
                for (unsigned I = 0; I < Operation.Sources; ++I)
                  EXPECT_EQ(Call->getArgOperand(I),
                            Call->getFunction()->getArg(I));
              ++Count;
            }
      EXPECT_EQ(Count, 1);
      CEmitterOptions Options;
      Options.TheArch = Arch::AArch64;
      Options.Format = BinaryFormat::MachO;
      for (bool LLVMRoute : {false, true}) {
        if (!LLVMRoute && Optimize)
          continue;
        std::string Source;
        llvm::raw_string_ostream Out(Source);
        if (LLVMRoute)
          ASSERT_TRUE(LLVMCEmitter().emit(*Module, Out, Options));
        else {
          auto High = MedToHighConverter().convert(Function, Arch::AArch64);
          ASSERT_TRUE(HighCEmitter().emit({High}, Out, Options));
        }
        EXPECT_NE(Source.find(Operation.MachineAsm), std::string::npos)
            << Source;
        EXPECT_EQ(Source.find("unknown_intrinsic"), std::string::npos)
            << Source;
        compileArm64eC(Source, [&](llvm::StringRef Path) {
          auto Buffer = llvm::MemoryBuffer::getFile(Path);
          ASSERT_TRUE(Buffer);
          CheckObject((*Buffer)->getMemBufferRef(), Operation.Id);
        });
      }
      const auto Object =
          Codegen().compile(*Module, Arch::AArch64, BinaryFormat::MachO);
      EXPECT_TRUE(Object.Success);
      CheckObject(
          llvm::MemoryBufferRef(llvm::StringRef(reinterpret_cast<const char *>(
                                                    Object.ObjectData.data()),
                                                Object.ObjectData.size()),
                                "cpa.o"),
          Operation.Id);
    }
}

TEST(AArch64CPASemantics,
     DeadResultsKeepChecksAndTemporariesDoNotCaptureInputs) {
  for (const auto &Operation : kAArch64CPAOperations) {
    auto Function = pointerFunction(Operation.Id, "checked_pointer");
    Function.Blocks[0].Ops.back().Inputs[0] = MedVar::makeConst(11, 8);
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Function}, Context, "dead_cpa", Arch::AArch64);
    ASSERT_TRUE(Module);
    optimize(*Module);
    CEmitterOptions Options;
    Options.TheArch = Arch::AArch64;
    Options.Format = BinaryFormat::MachO;
    for (bool LLVMRoute : {false, true}) {
      std::string Source;
      llvm::raw_string_ostream Out(Source);
      if (LLVMRoute)
        ASSERT_TRUE(LLVMCEmitter().emit(*Module, Out, Options));
      else {
        auto High = MedToHighConverter().convert(Function, Arch::AArch64);
        ASSERT_TRUE(HighCEmitter().emit({High}, Out, Options));
      }
      EXPECT_NE(Source.find(Operation.MachineAsm), std::string::npos) << Source;
      compileArm64eC(Source);
      std::vector<std::string> Args = {"__checked_pointer",
                                       "__checked_pointer_1"};
      if (Operation.Sources == 3)
        Args.push_back("__checked_pointer_2");
      bool HasIntrinsics = false;
      const std::string Body =
          LLVMRoute ? renderARMInlineAsm(Arch::AArch64, Operation.MachineAsm,
                                         "", Args[0], true, Args)
                              .Code +
                          "return " + Args[0] + ";"
                    : "return " +
                          renderARMIntrinsicCall(Operation.Id, Args, 8,
                                                 HasIntrinsics) +
                          ";";
      std::string Capture = "#include <stdint.h>\nuint64_t capture(";
      for (unsigned I = 0; I < Args.size(); ++I) {
        if (I)
          Capture += ", ";
        Capture += "uint64_t " + Args[I];
      }
      Capture += ") { " + Body + " }\n";
      compileArm64eC(
          Capture,
          [&](llvm::StringRef Path) {
            llvm::LLVMContext CContext;
            llvm::SMDiagnostic Diagnostic;
            auto CModule = llvm::parseIRFile(Path, Diagnostic, CContext);
            ASSERT_TRUE(CModule) << Diagnostic.getMessage().str();
            auto *CFn = CModule->getFunction("capture");
            ASSERT_TRUE(CFn);
            CFn->removeFnAttr(llvm::Attribute::OptimizeNone);
            optimize(*CModule);
            ASSERT_FALSE(llvm::verifyModule(*CModule));
            unsigned Count = 0;
            for (auto &Block : *CFn)
              for (auto &Insn : Block) {
                const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Insn);
                const auto *Asm = Call ? llvm::dyn_cast<llvm::InlineAsm>(
                                             Call->getCalledOperand())
                                       : nullptr;
                if (!Asm || Asm->getAsmString().find(Operation.MachineAsm) ==
                                std::string::npos)
                  continue;
                ++Count;
                ASSERT_EQ(Call->arg_size(), Operation.Sources);
                for (unsigned I = 0; I < Operation.Sources; ++I)
                  EXPECT_EQ(Call->getArgOperand(I), CFn->getArg(I));
                const auto *Return =
                    llvm::dyn_cast<llvm::ReturnInst>(Block.getTerminator());
                ASSERT_TRUE(Return);
                EXPECT_EQ(Return->getReturnValue(), Call);
              }
            EXPECT_EQ(Count, 1);
          },
          /*EmitLLVM=*/true);
    }
  }
}

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

TEST(AArch64PAuthSemantics, GenericSignaturePreservesIndependentSources) {
  for (bool Fast : {false, true}) {
    for (uint32_t Word : {0x9ac23020u, 0x9adf33e3u, 0x9ac13021u}) {
      SCOPED_TRACE(testing::Message() << Word << " " << Fast);
      const auto Ops = lift(Word, Fast);
      ASSERT_EQ(Ops.size(), 1);
      expectIntrinsic(Ops[0], Intrinsic::A64_Pacga, 2);
      EXPECT_TRUE(lowAArch64PAuthShapeIsValid(Ops[0]));
      if (Word == 0x9adf33e3u) {
        expectRegister(Ops[0].Output, a64reg::X3);
        EXPECT_TRUE(Ops[0].Inputs[1].isConst());
        EXPECT_EQ(Ops[0].Inputs[1].Offset, 0);
        expectRegister(Ops[0].Inputs[2], a64reg::SP);
      } else {
        expectRegister(Ops[0].Output,
                       Word == 0x9ac23020u ? a64reg::X0 : a64reg::X1);
        expectRegister(Ops[0].Inputs[1], a64reg::X1);
        expectRegister(Ops[0].Inputs[2],
                       Word == 0x9ac23020u ? a64reg::X2 : a64reg::X1);
      }
      for (unsigned Invalid = 0; Invalid < 4; ++Invalid) {
        auto Bad = Ops[0];
        if (Invalid == 0)
          Bad.Output.Size = 4;
        else if (Invalid == 1)
          Bad.Inputs[1].Size = 4;
        else if (Invalid == 2)
          Bad.Inputs[2].Size = 4;
        else
          --Bad.NumInputs;
        EXPECT_FALSE(lowAArch64PAuthShapeIsValid(Bad));
      }
    }
  }
  bool HasIntrinsics = false;
  EXPECT_THROW(renderARMIntrinsicCall(Intrinsic::A64_Pacga, {"a", "b"}, 4,
                                      HasIntrinsics),
               std::invalid_argument);
  EXPECT_THROW(renderARMIntrinsicCall(Intrinsic::A64_Pacga, {"a", "b", "c"}, 8,
                                      HasIntrinsics),
               std::invalid_argument);
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
    EXPECT_NE(Low.Code.find("result = "), std::string::npos);
  }
}

TEST(AArch64PAuthSemantics, CTemporariesNeverCaptureOperandsOrResults) {
  for (Intrinsic Id : {Intrinsic::Pacia, Intrinsic::Autdb, Intrinsic::Xpaci,
                       Intrinsic::A64_AuthBranch, Intrinsic::A64_Pacga}) {
    const bool Branch = Id == Intrinsic::A64_AuthBranch;
    const bool Strip = Id == Intrinsic::Xpaci;
    const bool Generic = Id == Intrinsic::A64_Pacga;
    const std::string Pointer = Branch ? "__target" : "__pointer";
    const std::string Modifier = Pointer + "_";
    std::vector<std::string> Args{Pointer};
    if (Branch)
      Args.push_back("1");
    if (!Strip)
      Args.push_back(Modifier);
    for (bool LLVMRoute : {false, true}) {
      if (Branch && LLVMRoute)
        continue; // LLVMC's auth builtin introduces no local C scope.
      if (Generic && LLVMRoute)
        continue; // Tested through the LLVM intrinsic's complete C projection.
      SCOPED_TRACE(testing::Message() << intrinsicName(Id) << " " << LLVMRoute);
      std::string Body;
      if (LLVMRoute) {
        const auto Auth = *aarch64PAuthOperation(Id);
        // The output deliberately has the same name as the original input.
        Body = renderARMInlineAsm(Arch::AArch64, Auth.llvmAsm(), Auth.Mnemonic,
                                  Pointer, true, Args)
                   .Code +
               "return " + Pointer + ";";
      } else {
        bool HasIntrinsics = false;
        Body = "return " + renderARMIntrinsicCall(Id, Args, 8, HasIntrinsics) +
               ";";
      }
      const std::string Source =
          "#include <stdint.h>\nuint64_t capture(uint64_t " + Pointer +
          ", uint64_t " + Modifier + ") { " + Body + " }\n";
      compileArm64eC(
          Source,
          [&](llvm::StringRef Path) {
            llvm::LLVMContext Context;
            llvm::SMDiagnostic Diagnostic;
            auto Module = llvm::parseIRFile(Path, Diagnostic, Context);
            ASSERT_TRUE(Module) << Diagnostic.getMessage().str();
            auto *Function = Module->getFunction("capture");
            ASSERT_TRUE(Function);
            Function->removeFnAttr(llvm::Attribute::OptimizeNone);
            optimize(*Module);
            ASSERT_FALSE(llvm::verifyModule(*Module));
            Function = Module->getFunction("capture");
            ASSERT_TRUE(Function);
            unsigned AuthenticationCount = 0;
            for (auto &Block : *Function)
              for (auto &Insn : Block) {
                const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Insn);
                if (!Call)
                  continue;
                const auto *Asm =
                    llvm::dyn_cast<llvm::InlineAsm>(Call->getCalledOperand());
                const auto *Callee = Call->getCalledFunction();
                const bool Machine =
                    Asm && !Branch &&
                    llvm::StringRef(Asm->getAsmString())
                        .starts_with(
                            std::string(
                                Generic ? intrinsicAsmMnemonic(Id)
                                        : aarch64PAuthOperation(Id)->Mnemonic) +
                            " ");
                const bool Control =
                    Branch && Callee &&
                    Callee->getIntrinsicID() == llvm::Intrinsic::ptrauth_auth;
                if (!Machine && !Control)
                  continue;
                ++AuthenticationCount;
                // Clang places the explicit modifier before the implicit tied
                // input of a +r output. NeverD's direct LLVM emitter uses its
                // own valid ordering; this oracle checks the C compiler's IR.
                if (Machine)
                  EXPECT_EQ(Asm->getConstraintString(), Generic ? "=r,r,r"
                                                        : Strip ? "=r,0"
                                                                : "=r,r,0");
                EXPECT_EQ(
                    Call->getArgOperand(Branch || Strip || Generic ? 0 : 1),
                    Function->getArg(0));
                if (!Strip)
                  EXPECT_EQ(Call->getArgOperand(Branch    ? 2
                                                : Generic ? 1
                                                          : 0),
                            Function->getArg(1));
                const auto *Return =
                    llvm::dyn_cast<llvm::ReturnInst>(Block.getTerminator());
                ASSERT_TRUE(Return);
                EXPECT_EQ(Return->getReturnValue(), Call);
              }
            EXPECT_EQ(AuthenticationCount, 1u) << Source;
          },
          /*EmitLLVM=*/true);
    }
  }
}

TEST(AArch64PAuthSemantics, LLVMAndBothCRoutesRetainMachineAuthentication) {
  for (Intrinsic Id :
       {Intrinsic::Pacia, Intrinsic::Pacib, Intrinsic::Pacda, Intrinsic::Pacdb,
        Intrinsic::Autia, Intrinsic::Autib, Intrinsic::Autda, Intrinsic::Autdb,
        Intrinsic::Xpaci, Intrinsic::Xpacd, Intrinsic::A64_AuthBranch,
        Intrinsic::A64_Pacga}) {
    SCOPED_TRACE(intrinsicName(Id));
    MedFunc F = pointerFunction(Id, "authenticate_pointer");
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
          if (Id == Intrinsic::A64_AuthBranch || Id == Intrinsic::A64_Pacga) {
            if (const auto *Callee = Call->getCalledFunction();
                Callee && Callee->getIntrinsicID() ==
                              (Id == Intrinsic::A64_Pacga
                                   ? llvm::Intrinsic::ptrauth_sign_generic
                                   : llvm::Intrinsic::ptrauth_auth))
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
                        : Id == Intrinsic::A64_Pacga
                            ? "pacga %0, %1, %2"
                            : aarch64PAuthOperation(Id)->Mnemonic),
                std::string::npos)
          << *C;
      EXPECT_EQ(C->find("unknown_intrinsic"), std::string::npos) << *C;
      if (Id != Intrinsic::A64_Pacga) {
        compileArm64eC(*C);
        continue;
      }
      compileArm64eC(
          *C,
          [&](llvm::StringRef Path) {
            llvm::LLVMContext CContext;
            llvm::SMDiagnostic Diagnostic;
            auto CModule = llvm::parseIRFile(Path, Diagnostic, CContext);
            ASSERT_TRUE(CModule) << Diagnostic.getMessage().str();
            auto *Function = CModule->getFunction(F.Name);
            ASSERT_TRUE(Function);
            Function->removeFnAttr(llvm::Attribute::OptimizeNone);
            optimize(*CModule);
            Function = CModule->getFunction(F.Name);
            ASSERT_TRUE(Function);
            unsigned Count = 0;
            for (auto &Block : *Function)
              for (auto &Insn : Block) {
                const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Insn);
                const auto *Asm = Call ? llvm::dyn_cast<llvm::InlineAsm>(
                                             Call->getCalledOperand())
                                       : nullptr;
                if (!Asm || Asm->getAsmString() != "pacga $0, $1, $2")
                  continue;
                ++Count;
                EXPECT_EQ(Asm->getConstraintString(), "=r,r,r");
                EXPECT_EQ(Call->getArgOperand(0), Function->getArg(0));
                EXPECT_EQ(Call->getArgOperand(1), Function->getArg(1));
                const auto *Return =
                    llvm::dyn_cast<llvm::ReturnInst>(Block.getTerminator());
                ASSERT_TRUE(Return);
                EXPECT_EQ(Return->getReturnValue(), Call);
              }
            EXPECT_EQ(Count, 1);
          },
          /*EmitLLVM=*/true);
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

namespace {
void executeCProjections(const std::vector<MedFunc> &Functions,
                         llvm::StringRef Oracle,
                         llvm::StringRef ExecuteTarget) {
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  Options.Format = BinaryFormat::MachO;
  for (bool LLVMRoute : {false, true})
    for (bool Optimize : {false, true}) {
      if (!LLVMRoute && Optimize)
        continue;
      SCOPED_TRACE(testing::Message() << LLVMRoute << " " << Optimize);
      std::string Source;
      llvm::raw_string_ostream Out(Source);
      if (LLVMRoute) {
        llvm::LLVMContext Context;
        auto Module =
            MedLLVMEmitter().emit(Functions, Context, "native", Arch::AArch64);
        ASSERT_TRUE(Module);
        if (Optimize)
          optimize(*Module);
        ASSERT_FALSE(llvm::verifyModule(*Module));
        ASSERT_TRUE(LLVMCEmitter().emit(*Module, Out, Options));
      } else {
        std::vector<HighFunc> High;
        for (const auto &F : Functions)
          High.push_back(MedToHighConverter().convert(F, Arch::AArch64));
        ASSERT_TRUE(HighCEmitter().emit(High, Out, Options));
      }
      Source.append(Oracle.data(), Oracle.size());
      compileArm64eC(Source, {}, /*EmitLLVM=*/false, ExecuteTarget);
    }
}

bool hasNativePAuth() {
#if defined(__APPLE__) && defined(__aarch64__)
  int Supported = 0;
  size_t Size = sizeof(Supported);
  return sysctlbyname("hw.optional.arm.FEAT_PAuth", &Supported, &Size, nullptr,
                      0) == 0 &&
         Supported;
#else
  return false;
#endif
}
} // namespace

TEST(AArch64PAuthSemantics, NativeGenericSignaturesAgreeWithBothCProjections) {
  if (!hasNativePAuth())
    GTEST_SKIP() << "native key comparison requires a PAuth Apple Silicon host";
  const auto Oracle = llvm::MemoryBuffer::getFile(
      std::string(NEVERD_ARM64E_TEST_HEADERS) + "/pacga_oracle.c");
  ASSERT_TRUE(Oracle);
  executeCProjections({pointerFunction(Intrinsic::A64_Pacga, "test_pacga")},
                      (*Oracle)->getBuffer(), "arm64-apple-macos");
}

TEST(AArch64PAuthSemantics, NativeKeysAgreeWithBothCProjections) {
  if (!hasNativePAuth())
    GTEST_SKIP() << "native key comparison requires a PAuth Apple Silicon host";
  // macOS can execute ordinary arm64 with PAC/AUT disabled, even though the
  // CPU advertises PAuth. Only an arm64e process qualifies these operations.
  bool ExecutionAvailable = false;
  compileArm64eC("int main(void) { return 0; }\n", {}, /*EmitLLVM=*/false,
                 "arm64e-apple-macos", &ExecutionAvailable);
  if (HasFatalFailure())
    return;
  if (!ExecutionAvailable)
    GTEST_SKIP() << "this host cannot execute the arm64e signing-key oracle";
  const auto Oracle = llvm::MemoryBuffer::getFile(
      std::string(NEVERD_ARM64E_TEST_HEADERS) + "/pauth_oracle.c");
  ASSERT_TRUE(Oracle);
  std::vector<MedFunc> Functions;
  const std::pair<Intrinsic, const char *> Cases[] = {
      {Intrinsic::Pacia, "test_pacia"},    {Intrinsic::Pacib, "test_pacib"},
      {Intrinsic::Pacda, "test_pacda"},    {Intrinsic::Pacdb, "test_pacdb"},
      {Intrinsic::Autia, "test_autia"},    {Intrinsic::Autib, "test_autib"},
      {Intrinsic::Autda, "test_autda"},    {Intrinsic::Autdb, "test_autdb"},
      {Intrinsic::Xpaci, "test_xpaci"},    {Intrinsic::Xpacd, "test_xpacd"},
      {Intrinsic::A64_Pacga, "test_pacga"}};
  for (const auto &[Id, Name] : Cases) {
    auto F = pointerFunction(Id, Name);
    F.Entry += Functions.size() * 0x10;
    F.Blocks.front().StartAddr = F.Entry;
    Functions.push_back(std::move(F));
  }
  executeCProjections(Functions, (*Oracle)->getBuffer(), "arm64e-apple-macos");
}

TEST(AArch64PAuthSemantics, UnsupportedPAuthLRNeverFallsBackToHints) {
  // Independently assembled with LLVM MC's +pauth-lr feature. PACM is the
  // compatible HINT that changes how later baseline PAC/AUT use modifiers.
  for (bool Fast : {false, true})
    for (uint32_t Word :
         {0xd50324ffu, 0xdac18bfeu, 0xdac18ffeu, 0xdac1bbfeu, 0xdac1bffeu,
          0xdac1a3feu, 0xdac1a7feu, 0xdac183feu, 0xdac187feu, 0xf380001fu,
          0xf3a0001fu, 0x5500001fu, 0x5520001fu}) {
      SCOPED_TRACE(testing::Message() << Word << " " << Fast);
      EXPECT_THROW(lift(Word, Fast), UnliftedInstruction);
    }
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
