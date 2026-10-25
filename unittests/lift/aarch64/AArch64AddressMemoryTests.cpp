//===- AArch64AddressMemoryTests.cpp - Original memory contracts ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Raw instruction admission, malformed IR, memory effects, real emitted
/// opcodes and native outcomes independently check checked-address ownership.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/c/render/AArch64/AArch64AddressMemoryC.h"
#include "neverd/backend/c/render/HighC/HighCIntrinsicRender.h"
#include "neverd/backend/c/render/LLVMC/LLVMCIntrinsicRender.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowIntrinsicEffects.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/loader/MachO/MachOLoader.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace neverd;

namespace {
#define MEMORY_VALUE(Type, Name, Value) inline constexpr Type Name = Value;
#define MEMORY_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "AArch64AddressMemoryFixtures.def"

struct MemoryCase {
  uint32_t Word;
  unsigned Destination;
  unsigned Base;
  unsigned Index;
  uint32_t Scratch;
};
constexpr MemoryCase Cases[] = {
#define MEMORY_CASE(Word, Dst, Base, Index, Scratch)                           \
  {Word, Dst, Base, Index, Scratch},
#include "AArch64AddressMemoryFixtures.def"
};

std::vector<LowOp> liftMemory(uint32_t Word, bool Fast, bool Strict = true) {
  Decoder Dec;
  EXPECT_TRUE(Dec.init(Arch::AArch64));
  Dec.setStrict(Strict);
  std::array<uint8_t, kWordBytes> Bytes;
  writeLE<uint32_t>(Bytes.data(), Word);
  DecodedInsn Insn;
  EXPECT_EQ(Fast
                ? Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kEntry, Insn)
                : Dec.decodeOne(Bytes.data(), Bytes.size(), kEntry, Insn),
            kWordBytes);
  std::vector<LowOp> Ops;
  Dec.liftToLow(Insn, Ops);
  return Ops;
}

MedFunc memoryFunction(uint32_t Word, bool Discard = false) {
  MedFunc Function;
  Function.Name = kName;
  Function.Entry = kEntry;
  Function.ReturnType = NdType::makeInt(kScalarBytes, false);
  Function.ReturnValueEvidence = MedReturnValueEvidence::ReturnsInteger;
  for (unsigned I = 0; I != 2; ++I) {
    MedVar Parameter;
    Parameter.Kind = MedVar::Param;
    Parameter.TheArch = Arch::AArch64;
    Parameter.Id = I;
    Parameter.RegOff = a64reg::X0 + I * kScalarBytes;
    Parameter.Size = kScalarBytes;
    Function.Params.push_back(Parameter);
  }
  MedOp Load;
  Load.Opcode = NdOp::INTRINSIC;
  Load.Output.Kind = Discard ? MedVar::Temp : MedVar::Reg;
  Load.Output.TheArch = Arch::AArch64;
  Load.Output.Id = 2;
  Load.Output.SSAVer = 1;
  Load.Output.Size = kScalarBytes;
  Load.Output.RegOff = a64reg::X0;
  Load.addInput(MedVar::makeConst(
      static_cast<uint64_t>(Intrinsic::A64_Load64RegisterOffset), kIdBytes));
  Load.addInput(Function.Params[0]);
  Load.addInput(aarch64AddressMemoryInstruction(Word)->index() == kZeroRegister
                    ? MedVar::makeConst(0, kScalarBytes)
                    : Function.Params[1]);
  Load.addInput(MedVar::makeConst(Word, kWordBytes));
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Function.Blocks.emplace_back();
  Function.Blocks.front().Id = 0;
  Function.Blocks.front().StartAddr = kEntry;
  Function.Blocks.front().Ops = {Load};
  if (Discard) {
    MedOp Zero;
    Zero.Opcode = NdOp::COPY;
    Zero.Output = Load.Output;
    Zero.Output.Kind = MedVar::Reg;
    Zero.Output.Id = 3;
    Zero.Output.SSAVer = 2;
    Zero.addInput(MedVar::makeConst(0, kScalarBytes));
    Function.Blocks.front().Ops.push_back(Zero);
  }
  Function.Blocks.front().Ops.push_back(Return);
  return Function;
}

LowFunc lowFunction(const LowOp &Load) {
  LowFunc Function;
  Function.Entry = kEntry;
  Function.Name = kName;
  Function.Blocks.emplace_back();
  Function.Blocks.front().StartAddr = kEntry;
  Function.Blocks.front().EndAddr = kEntry + kWordBytes;
  Function.Blocks.front().Ops.push_back(Load);
  LowInstructionBoundary Boundary;
  Boundary.Address = kEntry;
  Boundary.Size = kWordBytes;
  Boundary.OpCount = 1;
  Function.Blocks.front().InstructionBoundaries.push_back(Boundary);
  return Function;
}

void optimizeMemory(llvm::Module &Module) {
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
  auto Pipeline =
      Passes.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
  Pipeline.run(Module, Modules);
}

void inspectMemoryObject(llvm::MemoryBufferRef Buffer, uint32_t Word) {
  auto Image = MachOLoader().loadBuffer(Buffer);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  unsigned Count = 0;
  for (const auto &Segment : Image->Segments)
    if (Segment.isExecutable())
      for (size_t Offset = 0; Offset + kWordBytes <= Segment.Data.size();
           Offset += kWordBytes)
        Count += readLE<uint32_t>(Segment.Data.data() + Offset) == Word;
  EXPECT_EQ(Count, 1);
}

void compileMemoryC(
    const std::string &Source, bool Execute,
    const std::function<void(llvm::MemoryBufferRef)> &Inspect = {}) {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  const auto Found = llvm::sys::findProgramByName(kClang);
  if (!Found)
    GTEST_SKIP() << kMissingCompiler;
  const auto Compiler = *Found;
#endif
  llvm::SmallString<128> SourcePath, OutputPath, ErrorPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(kTempPrefix, kSourceExtension,
                                                  SourcePath));
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(kTempPrefix, kOutputExtension,
                                                  OutputPath));
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(kTempPrefix, kErrorExtension,
                                                  ErrorPath));
  llvm::FileRemover RemoveSource(SourcePath), RemoveOutput(OutputPath),
      RemoveError(ErrorPath);
  std::error_code Error;
  {
    llvm::raw_fd_ostream Out(SourcePath, Error);
    ASSERT_FALSE(Error);
    Out << Source;
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  for (llvm::StringRef Optimization : {kO0, kO2}) {
    llvm::SmallVector<llvm::StringRef> Args = {Compiler,
                                               kTargetFlag,
                                               kTarget,
                                               kFreestanding,
                                               kNoSystemHeaders,
                                               kInclude,
                                               NEVERD_ARM64E_TEST_HEADERS,
                                               kLanguage,
                                               Optimization,
                                               SourcePath,
                                               kOutputFlag,
                                               OutputPath};
    if (!Execute)
      Args.push_back(kCompileOnly);
    std::string Diagnostic;
    const auto Status =
        llvm::sys::ExecuteAndWait(Compiler, Args, std::nullopt, Redirects,
                                  kTimeoutSeconds, 0, &Diagnostic);
    const auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Status, 0) << Diagnostic
                         << (Errors ? (*Errors)->getBuffer().str()
                                    : std::string())
                         << Source;
    if (Inspect) {
      const auto Buffer = llvm::MemoryBuffer::getFile(OutputPath);
      ASSERT_TRUE(bool(Buffer));
      Inspect((*Buffer)->getMemBufferRef());
    }
    if (Execute) {
      const llvm::StringRef RunArgs[] = {OutputPath};
      EXPECT_EQ(llvm::sys::ExecuteAndWait(OutputPath, RunArgs, std::nullopt,
                                          Redirects, kTimeoutSeconds, 0,
                                          &Diagnostic),
                0)
          << Diagnostic << Source;
    }
  }
}

std::string memorySource(const MedFunc &Function, llvm::Module &Module,
                         bool LLVMRoute) {
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  Options.Format = BinaryFormat::MachO;
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  if (LLVMRoute)
    EXPECT_TRUE(LLVMCEmitter().emit(Module, Out, Options));
  else {
    const auto High = MedToHighConverter().convert(Function, Arch::AArch64);
    EXPECT_TRUE(HighCEmitter().emit({High}, Out, Options));
  }
  return Source;
}
} // namespace

TEST(AArch64AddressMemory, RawWordsPreserveIndexScaleAndAliases) {
  for (const auto &Case : Cases)
    for (bool Fast : {false, true})
      for (bool Strict : {false, true}) {
        SCOPED_TRACE(Case.Word);
        const auto Instruction = aarch64AddressMemoryInstruction(Case.Word);
        ASSERT_TRUE(Instruction);
        EXPECT_EQ(Instruction->destination(), Case.Destination);
        EXPECT_EQ(Instruction->base(), Case.Base);
        EXPECT_EQ(Instruction->index(), Case.Index);
        EXPECT_EQ(Instruction->scratchWord(), Case.Scratch);
        const auto Ops = liftMemory(Case.Word, Fast, Strict);
        ASSERT_EQ(Ops.size(), 1);
        const auto &Load = Ops.front();
        EXPECT_EQ(Load.Opcode, NdOp::INTRINSIC);
        ASSERT_EQ(Load.NumInputs, 4);
        EXPECT_TRUE(lowAArch64AddressMemoryShapeIsValid(Load));
        EXPECT_EQ(Load.Output.Size, kScalarBytes);
        if (Case.Destination == kZeroRegister)
          EXPECT_TRUE(Load.Output.isTemp());
        else {
          EXPECT_TRUE(Load.Output.isReg());
          EXPECT_EQ(Load.Output.Offset, Case.Destination * kScalarBytes);
        }
        EXPECT_EQ(Load.Inputs[1].Offset, Case.Base == kZeroRegister
                                             ? a64reg::SP
                                             : Case.Base * kScalarBytes);
        if (Case.Index == kZeroRegister) {
          EXPECT_TRUE(Load.Inputs[2].isConst());
          EXPECT_EQ(Load.Inputs[2].Offset, 0);
        } else {
          EXPECT_TRUE(Load.Inputs[2].isReg());
          EXPECT_EQ(Load.Inputs[2].Offset, Case.Index * kScalarBytes);
        }
        EXPECT_EQ(Load.Inputs[3].Offset, Case.Word);
        EXPECT_FALSE(lowIntrinsicHasOnlyValueEffects(Load));
        EXPECT_FALSE(lowIntrinsicMemoryOperands(Load).Complete);
        EXPECT_TRUE(isSideeffectIntrinsic(Intrinsic::A64_Load64RegisterOffset));
      }
  for (uint64_t Word :
       {uint64_t(kNotLoad), uint64_t(kWordLoad), uint64_t(kSignedIndex),
        uint64_t(kImmediate), kOversized})
    EXPECT_FALSE(aarch64AddressMemoryInstruction(Word));
}

TEST(AArch64AddressMemory,
     LLVMAndBothCProjectionsRetainOneLoadIncludingDeadResults) {
  for (const auto &Case : Cases) {
    if (Case.Base == kZeroRegister)
      continue;
    for (bool Discard : {false, true})
      for (bool Optimize : {false, true}) {
        SCOPED_TRACE(Case.Word);
        const auto Function = memoryFunction(Case.Word, Discard);
        EXPECT_FALSE(
            endsCallArgumentSetup(Function.Blocks.front().Ops.front()));
        ASSERT_TRUE(verifyMedFunc(Function, kName));
        llvm::LLVMContext Context;
        auto Module =
            MedLLVMEmitter().emit({Function}, Context, kName, Arch::AArch64);
        ASSERT_TRUE(Module);
        if (Optimize)
          optimizeMemory(*Module);
        EXPECT_FALSE(llvm::verifyModule(*Module));
        unsigned Count = 0;
        for (const auto &Block : *Module->getFunction(kName))
          for (const auto &Insn : Block)
            if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Insn)) {
              const auto *Asm =
                  llvm::dyn_cast<llvm::InlineAsm>(Call->getCalledOperand());
              ASSERT_TRUE(Asm);
              EXPECT_EQ(Asm->getAsmString(), kAddressLoadAsm);
              EXPECT_EQ(Asm->getConstraintString(), kAddressLoadConstraints);
              EXPECT_TRUE(Asm->hasSideEffects());
              ++Count;
            }
        EXPECT_EQ(Count, 1);
        for (bool LLVMRoute : {false, true}) {
          if (!LLVMRoute && Optimize)
            continue;
          compileMemoryC(memorySource(Function, *Module, LLVMRoute), false,
                         [&](llvm::MemoryBufferRef Buffer) {
                           inspectMemoryObject(Buffer, Case.Scratch);
                         });
        }
        const auto Object =
            Codegen().compile(*Module, Arch::AArch64, BinaryFormat::MachO);
        ASSERT_TRUE(Object.Success);
        inspectMemoryObject(llvm::MemoryBufferRef(
                                llvm::StringRef(reinterpret_cast<const char *>(
                                                    Object.ObjectData.data()),
                                                Object.ObjectData.size()),
                                kObject),
                            Case.Scratch);
      }
  }
}

TEST(AArch64AddressMemory, BothCProjectionsExecutePositiveAndNegativeOffsets) {
#if defined(__APPLE__) && defined(__aarch64__)
  for (bool Scaled : {false, true}) {
    const auto Function = memoryFunction(Scaled ? kScaled : kFirst);
    for (bool Optimize : {false, true}) {
      llvm::LLVMContext Context;
      auto Module =
          MedLLVMEmitter().emit({Function}, Context, kName, Arch::AArch64);
      ASSERT_TRUE(Module);
      if (Optimize)
        optimizeMemory(*Module);
      for (bool LLVMRoute : {false, true})
        compileMemoryC(
            memorySource(Function, *Module, LLVMRoute) +
                llvm::formatv(kNativeHarness, kName, Scaled ? 1 : kScalarBytes)
                    .str(),
            true);
    }
  }
#else
  GTEST_SKIP() << kMissingNative;
#endif
}

TEST(AArch64AddressMemory,
     MalformedIRCannotReachLoweringOrHiddenLLVMFunctions) {
  enum class Mutation {
    IdWidth,
    WrappedId,
    OutputWidth,
    OutputKind,
    BaseWidth,
    IndexWidth,
    ZeroIndexValue,
    WordWidth,
    DynamicWord,
    WordRange,
    Opcode,
    InputKind,
    Ordering,
    AddressSpace,
    ExtraInput,
    ExtraOutput
  };
  for (auto Change :
       {Mutation::IdWidth, Mutation::WrappedId, Mutation::OutputWidth,
        Mutation::OutputKind, Mutation::BaseWidth, Mutation::IndexWidth,
        Mutation::ZeroIndexValue, Mutation::WordWidth, Mutation::DynamicWord,
        Mutation::WordRange, Mutation::Opcode, Mutation::InputKind,
        Mutation::Ordering, Mutation::AddressSpace, Mutation::ExtraInput,
        Mutation::ExtraOutput}) {
    SCOPED_TRACE(static_cast<unsigned>(Change));
    auto Function = memoryFunction(kFirst);
    auto &Med = Function.Blocks.front().Ops.front();
    auto Low = liftMemory(kFirst, false).front();
    switch (Change) {
    case Mutation::IdWidth:
      Low.Inputs[0].Size = Med.Inputs[0].Size = kWordBytes;
      break;
    case Mutation::WrappedId:
      Low.Inputs[0].Offset += kWrappedId;
      Med.Inputs[0].ConstVal += kWrappedId;
      break;
    case Mutation::OutputWidth:
      Low.Output.Size = Med.Output.Size = kWordBytes;
      break;
    case Mutation::OutputKind:
      Low.Output.Space = VnodeSpace::RAM;
      Med.Output.Kind = MedVar::Stack;
      break;
    case Mutation::BaseWidth:
      Low.Inputs[1].Size = Med.Inputs[1].Size = kWordBytes;
      break;
    case Mutation::IndexWidth:
      Low.Inputs[2].Size = Med.Inputs[2].Size = kWordBytes;
      break;
    case Mutation::ZeroIndexValue:
      // The raw word's XZR index requires an explicit zero value, even when
      // an adapter fabricates otherwise well-typed register operands.
      Low.Inputs[3].Offset = Med.Inputs[3].ConstVal = kZeroIndex;
      break;
    case Mutation::WordWidth:
      Low.Inputs[3].Size = Med.Inputs[3].Size = kScalarBytes;
      break;
    case Mutation::DynamicWord:
      Low.Inputs[3].Space = VnodeSpace::TEMP;
      Med.Inputs[3].Kind = MedVar::Temp;
      break;
    case Mutation::WordRange:
      Low.Inputs[3].Offset = Med.Inputs[3].ConstVal = kOversized;
      break;
    case Mutation::Opcode:
      Low.Inputs[3].Offset = Med.Inputs[3].ConstVal = kNotLoad;
      break;
    case Mutation::InputKind:
      Low.Inputs[2].Space = VnodeSpace::RAM;
      Med.Inputs[2].Kind = MedVar::Stack;
      break;
    case Mutation::Ordering:
      Low.MemoryOrdering = Med.MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    case Mutation::AddressSpace:
      Low.MemoryAddressSpace = Med.MemoryAddressSpace =
          NdMemoryAddressSpace::X86FS;
      break;
    case Mutation::ExtraInput:
      Low.addInput(Low.Inputs[1]);
      Med.addInput(Med.Inputs[1]);
      break;
    case Mutation::ExtraOutput:
      Med.IntrinsicOutputs.push_back(Med.Output);
      break;
    }
    EXPECT_FALSE(aarch64AddressMemoryMedShapeIsValid(Med));
    EXPECT_TRUE(endsCallArgumentSetup(Med));
    EXPECT_FALSE(verifyMedFunc(Function, kName));
    if (Change != Mutation::ExtraOutput) {
      EXPECT_FALSE(lowAArch64AddressMemoryShapeIsValid(Low));
      const auto Input = lowFunction(Low);
      auto Error = validateLowInstructionBoundaries(Input.Blocks.front());
      EXPECT_TRUE(bool(Error));
      llvm::consumeError(std::move(Error));
      EXPECT_THROW(LowToMedConverter().convert(Input, Arch::AArch64),
                   LowToMedConversionError);
    }
    for (bool Masked : {false, true}) {
      llvm::LLVMContext Context;
      const std::vector<char> Mask{0};
      EXPECT_THROW(MedLLVMEmitter().emit(
                       {Function}, Context, kName, Arch::AArch64, {}, nullptr,
                       BinaryFormat::MachO, false, Masked ? &Mask : nullptr),
                   std::runtime_error);
    }
  }
}

TEST(AArch64AddressMemory,
     SPOriginAndDifferentTargetsRemainExplicitlyUnavailable) {
  for (Arch Target : {Arch::AArch64, Arch::X64}) {
    const auto Function =
        memoryFunction(Target == Arch::AArch64 ? kStackBase : kFirst);
    llvm::LLVMContext Context;
    EXPECT_THROW(MedLLVMEmitter().emit({Function}, Context, kName, Target),
                 std::runtime_error);
    const auto High = MedToHighConverter().convert(Function, Arch::AArch64);
    CEmitterOptions Options;
    Options.TheArch = Target;
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    EXPECT_THROW(HighCEmitter().emit({High}, Out, Options),
                 std::invalid_argument);
    EXPECT_TRUE(Source.empty());
  }
}

TEST(AArch64AddressMemory, LoadedPointerBitsSurviveBothTypedSourceRoutes) {
  auto Function = memoryFunction(kFirst);
  Function.ReturnType = NdType::makePtr();
  for (bool Optimize : {false, true}) {
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Function}, Context, kName, Arch::AArch64);
    ASSERT_TRUE(Module);
    if (Optimize)
      optimizeMemory(*Module);
    for (bool LLVMRoute : {false, true}) {
      const auto Source = memorySource(Function, *Module, LLVMRoute);
      compileMemoryC(Source, false);
#if defined(__APPLE__) && defined(__aarch64__)
      compileMemoryC(Source + llvm::formatv(kPointerHarness, kName).str(),
                     true);
#endif
    }
  }
}

TEST(AArch64AddressMemory, PointerResultsUseTheCompleteABIReturnRegister) {
  for (Arch Target : {Arch::AArch64, Arch::ARM, Arch::X64, Arch::X86}) {
    const auto &Registers = getTargetRegInfo(Target);
    auto Function = memoryFunction(kFirst);
    Function.Params.resize(1);
    auto &Parameter = Function.Params.front();
    Parameter.TheArch = Target;
    Parameter.Size = Registers.PointerSize;
    Parameter.RegOff = Registers.IntReturnReg;
    Function.ReturnType = NdType::makePtr();
    Function.ReturnType->Size = Registers.PointerSize;
    auto &Copy = Function.Blocks.front().Ops.front();
    Copy.Opcode = NdOp::COPY;
    Copy.Output.TheArch = Target;
    Copy.Output.Size = Registers.PointerSize;
    Copy.Output.RegOff = Registers.IntReturnReg;
    Copy.NumInputs = 1;
    Copy.Inputs.front() = Parameter;
    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit({Function}, Context, kName, Target);
    ASSERT_TRUE(Module);
    optimizeMemory(*Module);
    EXPECT_FALSE(llvm::verifyModule(*Module));
    const auto *Return = llvm::dyn_cast<llvm::ReturnInst>(
        Module->getFunction(kName)->back().getTerminator());
    ASSERT_TRUE(Return);
    EXPECT_FALSE(llvm::isa<llvm::Constant>(Return->getReturnValue()));
  }
}

TEST(AArch64AddressMemory, SourceTemporariesCannotCaptureEitherInput) {
  const auto Expression =
      aarch64AddressMemoryC(*aarch64AddressMemoryInstruction(kFirst),
                            kBaseExpression, kIndexExpression);
  compileMemoryC(llvm::formatv(kCaptureSource, Expression).str(), false);
#if defined(__APPLE__) && defined(__aarch64__)
  const auto Calls = aarch64AddressMemoryC(
      *aarch64AddressMemoryInstruction(kFirst), kBaseCall, kIndexCall);
  compileMemoryC(llvm::formatv(kInputCallsSource, Calls).str(), true);
#endif
}

TEST(AArch64AddressMemory, DecodedFunctionReachesBothCProjectionsAndExecutes) {
  for (bool Scaled : {false, true}) {
    BinaryImage Image;
    Image.Arch = Arch::AArch64;
    Image.Format = BinaryFormat::MachO;
    Image.Bits = Bitness::Bits64;
    Segment Text;
    Text.VA = kEntry;
    Text.Size = Text.FileSz = 2 * kWordBytes;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(Text.Size);
    writeLE<uint32_t>(Text.Data.data(), Scaled ? kABIScaled : kABIFirst);
    writeLE<uint32_t>(Text.Data.data() + kWordBytes, kReturn);
    Image.Segments.push_back(Text);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Image));
    Dec.setStrict(true);
    const auto Low = CFGBuilder().build(Image, Dec, kEntry, kName);
    LowToMedConverter Converter;
    Converter.setBinaryImage(&Image);
    const auto Function =
        Converter.convert(Low, Arch::AArch64, BinaryFormat::MachO);
    ASSERT_TRUE(verifyMedFunc(Function, kName));
    ASSERT_EQ(Function.Params.size(), 2);
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Function}, Context, kName, Arch::AArch64);
    ASSERT_TRUE(Module);
    for (bool LLVMRoute : {false, true}) {
      const auto Source = memorySource(Function, *Module, LLVMRoute);
      compileMemoryC(Source, false);
#if defined(__APPLE__) && defined(__aarch64__)
      compileMemoryC(Source + llvm::formatv(kNativeHarness, kName,
                                            Scaled ? 1 : kScalarBytes)
                                  .str(),
                     true);
#endif
    }
  }
}

TEST(AArch64AddressMemory, StoresRemainOnTheirOriginalSidesOfTheFaultingLoad) {
  auto Function = memoryFunction(kFirst);
  MedOp Before;
  Before.Opcode = NdOp::STORE;
  Before.addInput(Function.Params[0]);
  Before.addInput(MedVar::makeConst(11, kScalarBytes));
  auto After = Before;
  After.Inputs[1].ConstVal = 23;
  auto &Ops = Function.Blocks.front().Ops;
  Ops.insert(Ops.begin(), Before);
  Ops.insert(Ops.begin() + 2, After);
  for (bool Optimize : {false, true}) {
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Function}, Context, kName, Arch::AArch64);
    ASSERT_TRUE(Module);
    if (Optimize)
      optimizeMemory(*Module);
    bool Loaded = false;
    unsigned BeforeCount = 0, AfterCount = 0;
    for (const auto &Block : *Module->getFunction(kName))
      for (const auto &Instruction : Block) {
        if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Instruction))
          Loaded |= llvm::isa<llvm::InlineAsm>(Call->getCalledOperand());
        if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction))
          if (!llvm::isa<llvm::AllocaInst>(
                  llvm::getUnderlyingObject(Store->getPointerOperand())))
            ++(Loaded ? AfterCount : BeforeCount);
      }
    EXPECT_TRUE(Loaded);
    EXPECT_EQ(BeforeCount, 1);
    EXPECT_EQ(AfterCount, 1);
    for (bool LLVMRoute : {false, true}) {
      const auto Source = memorySource(Function, *Module, LLVMRoute);
      compileMemoryC(Source, false);
#if defined(__APPLE__) && defined(__aarch64__)
      compileMemoryC(Source + llvm::formatv(kBarrierHarness, kName).str(),
                     true);
#endif
    }
  }
}

TEST(AArch64AddressMemory,
     TypedSourceAdmissionPrecedesHiddenOrDiscardedBodies) {
  enum class Mutation {
    ExtraOutput,
    Indirect,
    NonCall,
    MissingOperand,
    WrongWidth,
    DynamicWord,
    StackBase,
    Target
  };
  for (auto Change :
       {Mutation::ExtraOutput, Mutation::Indirect, Mutation::NonCall,
        Mutation::MissingOperand, Mutation::WrongWidth, Mutation::DynamicWord,
        Mutation::StackBase, Mutation::Target})
    for (bool Hidden : {false, true}) {
      SCOPED_TRACE(static_cast<unsigned>(Change));
      HighFunc Function;
      Function.Name = kName;
      Function.Entry = kEntry;
      Function.ReturnType = NdType::makeVoid();
      auto Load = std::make_shared<HighExpr>();
      Load->Kind = ExprKind::Call;
      Load->IntrinsicId = Intrinsic::A64_Load64RegisterOffset;
      Load->Type = NdType::makeInt(kScalarBytes, false);
      Load->Operands = {HighExpr::makeConst(kEntry, kScalarBytes),
                        HighExpr::makeConst(0, kScalarBytes),
                        HighExpr::makeConst(kFirst, kWordBytes)};
      ASSERT_TRUE(aarch64AddressMemoryHighShapeIsValid(*Load));
      CEmitterOptions Options;
      Options.TheArch = Arch::AArch64;
      switch (Change) {
      case Mutation::ExtraOutput:
        Load->IntrinsicOutputs.push_back(MedVar::makeConst(0, kScalarBytes));
        break;
      case Mutation::Indirect:
        Load->IsIndirectCall = true;
        break;
      case Mutation::NonCall:
        Load->Kind = ExprKind::Const;
        break;
      case Mutation::MissingOperand:
        Load->Operands.front().reset();
        break;
      case Mutation::WrongWidth:
        Load->Operands[1]->Type = NdType::makeInt(kWordBytes, false);
        break;
      case Mutation::DynamicWord:
        Load->Operands[2]->Kind = ExprKind::Var;
        break;
      case Mutation::StackBase:
        Load->Operands[2]->ConstVal = kStackBase;
        break;
      case Mutation::Target:
        Options.TheArch = Arch::X64;
        break;
      }
      HighStmt Invoke;
      Invoke.Kind = StmtKind::ExprStmt;
      Invoke.Val = Load;
      if (Hidden) {
        HighStmt Branch;
        Branch.Kind = StmtKind::If;
        Branch.Cond = HighExpr::makeConst(0, 1);
        Branch.Body.push_back(Invoke);
        Function.Body.push_back(Branch);
      } else {
        Function.Body.push_back(Invoke);
      }
      std::string Source;
      llvm::raw_string_ostream Out(Source);
      EXPECT_THROW(HighCEmitter().emit({Function}, Out, Options),
                   std::invalid_argument);
      EXPECT_TRUE(Source.empty());
    }
}

TEST(AArch64AddressMemory,
     LLVMSourceValidatesTheExactTypedAssemblyBeforeFiltering) {
  const auto Function = memoryFunction(kFirst);
  llvm::LLVMContext Context;
  auto Module =
      MedLLVMEmitter().emit({Function}, Context, kName, Arch::AArch64);
  ASSERT_TRUE(Module);
  llvm::CallInst *Load = nullptr;
  for (auto &Block : *Module->getFunction(kName))
    for (auto &Instruction : Block)
      if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&Instruction))
        Load = Call;
  ASSERT_TRUE(Load);
  const std::vector<std::string> Args = {kBaseExpression, kIndexExpression,
                                         std::to_string(Cases[0].Scratch)};
  EXPECT_TRUE(renderAArch64AddressMemoryCall(Arch::AArch64, *Load, kResultName,
                                             true, Args));
  EXPECT_THROW(
      renderAArch64AddressMemoryCall(Arch::X64, *Load, kResultName, true, Args),
      std::invalid_argument);
  bool HasIntrinsics = false;
  EXPECT_THROW(renderIntrinsicCall(Intrinsic::A64_Load64RegisterOffset,
                                   Arch::X64, Args, kScalarBytes, HasIntrinsics,
                                   true,
                                   {kScalarBytes, kScalarBytes, kWordBytes}),
               std::invalid_argument);
  const auto *Original = llvm::cast<llvm::InlineAsm>(Load->getCalledOperand());
  auto *BadAsm =
      llvm::InlineAsm::get(Original->getFunctionType(), kAddressLoadAsm,
                           kAddressLoadConstraints, false);
  Load->setCalledOperand(BadAsm);
  EXPECT_THROW(validateAArch64AddressMemoryCall(Arch::AArch64, *Load),
               std::invalid_argument);
  Load->setCalledOperand(const_cast<llvm::InlineAsm *>(Original));
  Load->setMemoryEffects(llvm::MemoryEffects::none());
  EXPECT_THROW(validateAArch64AddressMemoryCall(Arch::AArch64, *Load),
               std::invalid_argument);
  Load->setMemoryEffects(llvm::MemoryEffects::unknown());
  Load->addFnAttr(llvm::Attribute::Speculatable);
  EXPECT_THROW(validateAArch64AddressMemoryCall(Arch::AArch64, *Load),
               std::invalid_argument);
  Load->removeFnAttr(llvm::Attribute::Speculatable);
  auto *Word = Load->getArgOperand(2);
  Load->setArgOperand(2, llvm::ConstantInt::get(Word->getType(), kFirst));
  EXPECT_THROW(renderAArch64AddressMemoryCall(Arch::AArch64, *Load, kResultName,
                                              false, Args),
               std::invalid_argument);
  // Even selecting a different, empty function must validate this bad body.
  auto *Other = llvm::Function::Create(Original->getFunctionType(),
                                       llvm::GlobalValue::ExternalLinkage,
                                       kResultName, *Module);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  EXPECT_THROW(
      LLVMCEmitter().emit(*Module, Out, Options, nullptr, nullptr, Other),
      std::invalid_argument);
  EXPECT_TRUE(Source.empty());
  Load->setArgOperand(2, Word);
}

TEST(AArch64AddressMemory,
     SoftwareEmulationCannotAssumeWrappingAddressArithmetic) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  NdOpEmulator Emulator(Image);
  Emulator.setRegister(a64reg::X0, kEntry);
  Emulator.setRegister(a64reg::X1, kEntry);
  Emulator.setRegister(a64reg::X2, 0);
  for (bool Strict : {false, true}) {
    Emulator.setStrictMode(Strict);
    EXPECT_FALSE(Emulator.step(liftMemory(kFirst, false).front()));
    EXPECT_EQ(Emulator.getRegister(a64reg::X0), kEntry);
  }
}

TEST(AArch64AddressMemory, ImageReadsRequireControlIndependentAddresses) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Segment Data;
  Data.VA = kEntry;
  Data.Size = Data.FileSz = 2 * kScalarBytes;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(Data.Size);
  writeLE<uint64_t>(Data.Data.data(), kLoadedValue);
  writeLE<uint64_t>(Data.Data.data() + kScalarBytes, ~kLoadedValue);
  Image.Segments.push_back(Data);
  const auto Load = liftMemory(kFirst, false).front();
  const auto Scaled = liftMemory(kScaled, false).front();
  const auto Instruction = aarch64AddressMemoryInstruction(kFirst);
  ASSERT_TRUE(Instruction);
  EXPECT_FALSE(Instruction->controlIndependentAddress(kAccessBoundary, 0));
  EXPECT_FALSE(Instruction->controlIndependentAddress(kTagHealingBase,
                                                      kTagHealingIndex));
  EXPECT_FALSE(Instruction->controlIndependentAddress(kPoisonHealingBase,
                                                      kPoisonHealingIndex));
  // Checking an already poisoned address without changing its region is
  // idempotent. Identical bits alone still grant no mapped-memory evidence.
  EXPECT_EQ(Instruction->controlIndependentAddress(kPoisonHealingBase, 0),
            kPoisonHealingBase);
  EXPECT_FALSE(aarch64AddressMemoryInstruction(kStackBase)
                   ->controlIndependentAddress(kEntry, 0));
  for (bool Strict : {false, true}) {
    NdOpEmulator UnknownIndex(Image);
    UnknownIndex.setStrictMode(Strict);
    UnknownIndex.setRegister(a64reg::X1, kEntry);
    EXPECT_FALSE(UnknownIndex.step(Load));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(Strict);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(a64reg::X1, kEntry);
    Emulator.setRegister(a64reg::X2, 0);
    ASSERT_TRUE(Emulator.step(Load));
    EXPECT_EQ(Emulator.getRegister(a64reg::X0), kLoadedValue);
    Emulator.setRegister(a64reg::X2, 1);
    ASSERT_TRUE(Emulator.step(Scaled));
    EXPECT_EQ(Emulator.getRegister(a64reg::X0), ~kLoadedValue);
    Emulator.setRegister(a64reg::X2, kScalarBytes);
    ASSERT_TRUE(Emulator.step(Load));
    EXPECT_EQ(Emulator.getRegister(a64reg::X0), ~kLoadedValue);
    const auto Reads = Emulator.getLoadRecords().size();
    // Both sums would read the mapped first word under unchecked addition.
    // Unknown CPTA must not turn either poisoned/tagged base into that read.
    for (const auto &[Base, Index] :
         {std::pair{kTagHealingBase, kTagHealingIndex},
          std::pair{kPoisonHealingBase, kPoisonHealingIndex},
          std::pair{kAccessBoundary, uint64_t(0)}}) {
      Emulator.setRegister(a64reg::X0, kEntry);
      Emulator.setRegister(a64reg::X1, Base);
      Emulator.setRegister(a64reg::X2, Index);
      EXPECT_FALSE(Emulator.step(Load));
      EXPECT_EQ(Emulator.getRegister(a64reg::X0), kEntry);
      EXPECT_EQ(Emulator.getLoadRecords().size(), Reads);
    }
  }
}
