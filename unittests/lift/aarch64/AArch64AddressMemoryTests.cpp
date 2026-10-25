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
#include "neverd/ir/med/MedDispatchSlots.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/loader/BinaryImageModel.h"
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

#include <algorithm>
#include <array>
#include <climits>
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
  unsigned IndexBytes;
};
constexpr MemoryCase Cases[] = {
#define MEMORY_CASE(Word, Dst, Base, Index, Scratch, IndexBytes)               \
  {Word, Dst, Base, Index, Scratch, IndexBytes},
#include "AArch64AddressMemoryFixtures.def"
};

struct MemoryFamilyCase {
  uint32_t Word;
  uint32_t Scratch;
  bool Load;
  unsigned Access;
  unsigned Result;
  bool Signed;
  unsigned IndexBytes;
  unsigned Shift;
  uint64_t Expected;
};
constexpr MemoryFamilyCase FamilyCases[] = {
#define MEMORY_FAMILY_CASE(Word, Scratch, Load, Access, Result, Signed,        \
                           IndexBytes, Shift, Expected)                        \
  {Word, Scratch, Load, Access, Result, Signed, IndexBytes, Shift, Expected},
#include "AArch64AddressMemoryFixtures.def"
};

struct MemoryABICase {
  uint32_t Word;
  uint32_t Scratch;
  unsigned Access;
  bool SignedIndex;
};
constexpr MemoryABICase ABICases[] = {
#define MEMORY_ABI_CASE(Word, Scratch, Access, SignedIndex)                    \
  {Word, Scratch, Access, SignedIndex},
#include "AArch64AddressMemoryFixtures.def"
};

struct MemorySpecialCase {
  uint32_t Word;
  bool Load;
  unsigned Access;
  unsigned Destination;
  unsigned Base;
  unsigned Index;
};
constexpr MemorySpecialCase SpecialCases[] = {
#define MEMORY_SPECIAL_CASE(Word, Load, Access, Destination, Base, Index)      \
  {Word, Load, Access, Destination, Base, Index},
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

LowOp originalMemoryOperation(const std::vector<LowOp> &Ops) {
  const auto It =
      std::find_if(Ops.begin(), Ops.end(), lowAArch64AddressMemoryShapeIsValid);
  EXPECT_NE(It, Ops.end());
  return It == Ops.end() ? LowOp{} : *It;
}

MedFunc memoryFunction(uint32_t Word, bool Discard = false) {
  const auto Instruction = *aarch64AddressMemoryInstruction(Word);
  MedFunc Function;
  Function.Name = kName;
  Function.Entry = kEntry;
  Function.ReturnType = NdType::makeInt(kScalarBytes, false);
  Function.ReturnValueEvidence = MedReturnValueEvidence::ReturnsInteger;
  for (unsigned I = 0; I != (Instruction.isLoad() ? 2U : 3U); ++I) {
    MedVar Parameter;
    Parameter.Kind = MedVar::Param;
    Parameter.TheArch = Arch::AArch64;
    Parameter.Id = I;
    Parameter.RegOff = a64reg::X0 + I * kScalarBytes;
    Parameter.Size = I == 0   ? kScalarBytes
                     : I == 1 ? Instruction.indexBytes()
                              : Instruction.accessBytes();
    Function.Params.push_back(Parameter);
  }
  MedOp Load;
  Load.Opcode = NdOp::INTRINSIC;
  Load.Output.Kind = Discard ? MedVar::Temp : MedVar::Reg;
  Load.Output.TheArch = Arch::AArch64;
  Load.Output.Id = 2;
  Load.Output.SSAVer = 1;
  Load.Output.Size = Instruction.accessBytes();
  Load.Output.RegOff = a64reg::X0;
  Load.addInput(MedVar::makeConst(
      static_cast<uint64_t>(Instruction.operation().Id), kIdBytes));
  Load.addInput(Function.Params[0]);
  Load.addInput(aarch64AddressMemoryInstruction(Word)->index() == kZeroRegister
                    ? MedVar::makeConst(0, Instruction.indexBytes())
                    : Function.Params[1]);
  Load.addInput(MedVar::makeConst(Word, kWordBytes));
  if (!Instruction.isLoad()) {
    Load.addInput(Instruction.destination() == kZeroRegister
                      ? MedVar::makeConst(0, Instruction.accessBytes())
                      : Function.Params[2]);
    Load.Output = MedVar{};
    Load.Output.Size = 0;
    Load.Output.Id = -1;
  }
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Function.Blocks.emplace_back();
  Function.Blocks.front().Id = 0;
  Function.Blocks.front().StartAddr = kEntry;
  Function.Blocks.front().Ops = {Load};
  if (Instruction.isLoad() && Instruction.accessBytes() != kScalarBytes &&
      !Discard) {
    Function.Blocks.front().Ops.front().Output.Kind = MedVar::Temp;
    MedOp Extend;
    Extend.Output = Load.Output;
    Extend.Output.Kind = MedVar::Reg;
    Extend.Output.Id = 3;
    Extend.Output.Size = kScalarBytes;
    Extend.addInput(Function.Blocks.front().Ops.front().Output);
    if (Instruction.operation().Signed &&
        Instruction.operation().ResultBytes < kScalarBytes) {
      auto Signed = Extend;
      Signed.Opcode = NdOp::INT_SEXT;
      Signed.Output.Kind = MedVar::Temp;
      Signed.Output.Id = 4;
      Signed.Output.Size = Instruction.operation().ResultBytes;
      Function.Blocks.front().Ops.push_back(Signed);
      Extend.Opcode = NdOp::INT_ZEXT;
      Extend.Inputs[0] = Signed.Output;
    } else {
      Extend.Opcode =
          Instruction.operation().Signed ? NdOp::INT_SEXT : NdOp::INT_ZEXT;
    }
    Function.Blocks.front().Ops.push_back(Extend);
  }
  if (Discard || !Instruction.isLoad()) {
    MedOp Zero;
    Zero.Opcode = NdOp::COPY;
    Zero.Output = Load.Output;
    Zero.Output.Kind = MedVar::Reg;
    Zero.Output.TheArch = Arch::AArch64;
    Zero.Output.Size = kScalarBytes;
    Zero.Output.RegOff = a64reg::X0;
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
    const std::function<void(llvm::MemoryBufferRef)> &Inspect = {},
    llvm::StringRef ObjectPath = {}) {
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
    if (!ObjectPath.empty())
      Args.push_back(ObjectPath);
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
        EXPECT_EQ(Instruction->indexBytes(), Case.IndexBytes);
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
        EXPECT_EQ(Load.Inputs[2].Size, Case.IndexBytes);
        EXPECT_EQ(Load.Inputs[3].Offset, Case.Word);
        EXPECT_FALSE(lowIntrinsicHasOnlyValueEffects(Load));
        EXPECT_FALSE(lowIntrinsicMemoryOperands(Load).Complete);
        EXPECT_TRUE(isSideeffectIntrinsic(Intrinsic::A64_Load64RegisterOffset));
      }
  for (uint64_t Word :
       {uint64_t(kInvalidIndex), uint64_t(kImmediate), kOversized,
        uint64_t(kPrefetch), uint64_t(kReservedLoad), uint64_t(kVectorLoad)})
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

TEST(AArch64AddressMemory, CompleteNarrowRangesNeedNoBranchOrMappingEvidence) {
  for (const auto &Case : FamilyCases) {
    SCOPED_TRACE(Case.Word);
    const auto Instruction = aarch64AddressMemoryInstruction(Case.Word);
    ASSERT_TRUE(Instruction);
    const bool SignedWord =
        Case.IndexBytes == kWordBytes && (Case.Word & kFixtureSignedIndexBit);
    EXPECT_EQ(Instruction->controlIndependentAddressRange(kEntry,
                                                          kRangeNarrowMaximum),
              !SignedWord);
    EXPECT_TRUE(Instruction->controlIndependentAddressRange(
        kEntry, kRangePositiveMaximum));
    ASSERT_TRUE(Instruction->controlIndependentAddressRange(
        kEntry, kRangeExhaustiveMaximum));
    for (uint64_t Index = 0; Index <= kRangeExhaustiveMaximum; ++Index) {
      EXPECT_EQ(Instruction->controlIndependentAddress(kEntry, Index),
                kEntry + (Index << Case.Shift));
    }
    EXPECT_FALSE(Instruction->controlIndependentAddressRange(
        kAccessBoundary, kRangeNarrowMaximum));
    EXPECT_FALSE(
        Instruction->controlIndependentAddressRange(kEntry, kRangeFullMaximum));
  }
}

TEST(AArch64AddressMemory, AddressRangesRejectWrapStackAndMalformedEncodings) {
  const auto Scaled = *aarch64AddressMemoryInstruction(kScaled);
  EXPECT_TRUE(Scaled.controlIndependentAddressRange(kRangeTailBase, 1));
  EXPECT_FALSE(Scaled.controlIndependentAddressRange(kRangeTailBase, 2));
  EXPECT_TRUE(Scaled.controlIndependentAddressRange(kRangeWrapBase, 0));
  EXPECT_FALSE(Scaled.controlIndependentAddressRange(kRangeWrapBase, 1));
  const auto Zero = *aarch64AddressMemoryInstruction(kZeroIndex);
  EXPECT_TRUE(Zero.controlIndependentAddressRange(kEntry, 0));
  EXPECT_FALSE(Zero.controlIndependentAddressRange(kEntry, 1));
  EXPECT_FALSE(aarch64AddressMemoryInstruction(kStackBase)
                   ->controlIndependentAddressRange(kEntry, 0));
  for (uint32_t Invalid : {kInvalidIndex, kPrefetch, kReservedLoad})
    EXPECT_FALSE(
        AArch64AddressMemoryInstruction{Invalid}.controlIndependentAddressRange(
            kEntry, 0));
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

TEST(AArch64AddressMemory, DispatchUsesTheAuthenticatedLoadInputLifetime) {
  for (const uint32_t Word : {kAliasedIndex, kScaledZeroIndex}) {
    SCOPED_TRACE(Word);
    auto Function = memoryFunction(Word);
    auto &Block = Function.Blocks.front();
    auto &Load = Block.Ops.front();
    Load.Addr = kEntry;
    Load.OriginSeq = 0;
    MedOp &Branch = Block.Ops.back();
    Branch.Opcode = NdOp::INDIR_BR;
    Branch.Addr = kEntry + kWordBytes;
    Branch.addInput(Load.Output);
    JumpTable Table;
    Table.InsnAddr = Branch.Addr;
    Table.EntrySize = Table.EntryStride = kScalarBytes;
    Table.HasDispatchSlotMap = true;
    Table.AuthenticatedTableLoads = {
        {Load.Addr, Load.OriginSeq, Load.Output.Size}};
    Function.JumpTables.push_back(Table);
    BinaryImage Image;
    Image.Arch = Arch::AArch64;
    MedDispatchContext Context;
    Context.Func = &Function;
    Context.Img = &Image;
    const auto Find = [&] {
      return findTableSwitchIndex(Context, Block, Branch.Inputs[0]);
    };
    const auto Index = Find();
    ASSERT_TRUE(Index);
    const auto &Expected = Load.Inputs[kAddressMemoryIndexOperand + 1];
    EXPECT_EQ(Index->Kind, Expected.Kind);
    EXPECT_EQ(Index->Id, Expected.Id);
    EXPECT_EQ(Index->SSAVer, Expected.SSAVer);
    EXPECT_EQ(Index->ConstVal, Expected.ConstVal);
    EXPECT_EQ(Index->Size, Expected.Size);

    // Neither a stale occurrence nor two matching tables can identify one
    // selector. A valid shape alone is not a resolver certificate.
    ++Function.JumpTables.front().AuthenticatedTableLoads.front().Seq;
    EXPECT_FALSE(Find());
    Function.JumpTables.front() = Table;
    Function.JumpTables.front().AuthenticatedTableLoads.push_back(
        Table.AuthenticatedTableLoads.front());
    EXPECT_FALSE(Find());
    Function.JumpTables.front() = Table;
    Function.JumpTables.front().MutatedUnsafe = true;
    EXPECT_FALSE(Find());
    Function.JumpTables.front() = Table;
    Function.JumpTables.push_back(Table);
    EXPECT_FALSE(Find());
    Function.JumpTables.clear();
    EXPECT_FALSE(Find());
    Function.JumpTables.push_back(Table);
    Image.Arch = Arch::X64;
    EXPECT_FALSE(Find());
    Image.Arch = Arch::AArch64;
    Block.Ops.insert(Block.Ops.begin(), Load);
    EXPECT_FALSE(
        findTableSwitchIndex(Context, Block, Block.Ops.back().Inputs[0]));
  }
}

TEST(AArch64AddressMemory, AllScalarRegisterOffsetFamiliesKeepTheirContracts) {
  EXPECT_EQ(std::size(FamilyCases), kScalarFamilyCombinations);
  for (const auto &Case : FamilyCases) {
    SCOPED_TRACE(Case.Word);
    const auto Instruction = aarch64AddressMemoryInstruction(Case.Word);
    ASSERT_TRUE(Instruction);
    EXPECT_EQ(Instruction->isLoad(), Case.Load);
    EXPECT_EQ(Instruction->accessBytes(), Case.Access);
    EXPECT_EQ(Instruction->operation().ResultBytes, Case.Result);
    EXPECT_EQ(Instruction->operation().Signed, Case.Signed);
    EXPECT_EQ(Instruction->indexBytes(), Case.IndexBytes);
    EXPECT_EQ(Instruction->indexShift(), Case.Shift);
    EXPECT_EQ(Instruction->scratchWord(), Case.Scratch);
    EXPECT_EQ(bool(Instruction->controlIndependentAddress(kAccessBoundary, 0)),
              Case.Access <= kWordBytes);
    for (bool Fast : {false, true})
      for (bool Strict : {false, true}) {
        const auto Ops = liftMemory(Case.Word, Fast, Strict);
        ASSERT_FALSE(Ops.empty());
        const auto Op = originalMemoryOperation(Ops);
        ASSERT_TRUE(lowAArch64AddressMemoryShapeIsValid(Op));
        EXPECT_EQ(lowAArch64AddressLoadShapeIsValid(Op), Case.Load);
        EXPECT_EQ(Op.Inputs[2].Size, Case.IndexBytes);
        EXPECT_EQ(Op.Output.Size, Case.Load ? Case.Access : 0);
        EXPECT_EQ(Ops.back().Output.Size, Case.Load ? kScalarBytes : 0);
        EXPECT_FALSE(lowIntrinsicHasOnlyValueEffects(Op));
        EXPECT_FALSE(lowIntrinsicMemoryOperands(Op).Complete);
        if (!Case.Load) {
          ASSERT_EQ(Op.NumInputs, 5);
          EXPECT_EQ(Op.Inputs[4].Size, Case.Access);
          if (Case.Access < kWordBytes) {
            EXPECT_TRUE(Op.Inputs[4].isTemp());
            const auto &Slice = Ops.front();
            EXPECT_EQ(Slice.Opcode, NdOp::SUBBYTES);
            EXPECT_EQ(Slice.Inputs[0].Offset, a64reg::X0);
            EXPECT_EQ(Slice.Inputs[0].Size, kWordBytes);
            EXPECT_EQ(Slice.Output, Op.Inputs[4]);
          } else
            EXPECT_EQ(Op.Inputs[4].Offset, a64reg::X0);
        }
      }
    const auto Function = memoryFunction(Case.Word);
    EXPECT_EQ(endsCallArgumentSetup(Function.Blocks.front().Ops.front()),
              !Case.Load);
    EXPECT_TRUE(verifyMedFunc(Function, kName));
  }
}

TEST(AArch64AddressMemory,
     ScalarFamiliesExecuteBothCProjectionsAgainstNativeWords) {
  for (const auto &Case : FamilyCases) {
    SCOPED_TRACE(Case.Word);
    const auto Function = memoryFunction(Case.Word);
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Function}, Context, kName, Arch::AArch64);
    ASSERT_TRUE(Module);
    optimizeMemory(*Module);
    ASSERT_FALSE(llvm::verifyModule(*Module));
    unsigned Calls = 0;
    for (const auto &Block : *Module->getFunction(kName))
      for (const auto &Insn : Block)
        if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Insn)) {
          ASSERT_TRUE(validateAArch64AddressMemoryCall(Arch::AArch64, *Call));
          EXPECT_EQ(Call->getType()->isVoidTy(), !Case.Load);
          ++Calls;
        }
    EXPECT_EQ(Calls, 1);
    const auto Object =
        Codegen().compile(*Module, Arch::AArch64, BinaryFormat::MachO);
    ASSERT_TRUE(Object.Success);
    inspectMemoryObject(
        llvm::MemoryBufferRef(llvm::StringRef(reinterpret_cast<const char *>(
                                                  Object.ObjectData.data()),
                                              Object.ObjectData.size()),
                              kObject),
        Case.Scratch);
#if defined(__APPLE__) && defined(__aarch64__)
    llvm::SmallString<128> ObjectPath;
    ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(
        kTempPrefix, kObjectExtension, ObjectPath));
    llvm::FileRemover RemoveObject(ObjectPath);
    std::error_code ObjectError;
    {
      llvm::raw_fd_ostream Out(ObjectPath, ObjectError);
      ASSERT_FALSE(ObjectError);
      Out.write(reinterpret_cast<const char *>(Object.ObjectData.data()),
                Object.ObjectData.size());
    }
    const auto IndexType =
        llvm::formatv(kNativeUnsignedType, Case.IndexBytes * CHAR_BIT).str();
    const auto DataType = llvm::formatv(kNativeObjectDataParameter,
                                        llvm::formatv(kNativeUnsignedType,
                                                      Case.Access * CHAR_BIT))
                              .str();
    compileMemoryC(
        llvm::formatv(kNativeObjectDeclaration, kName, IndexType,
                      Case.Load ? kEmptyNativeArgument : DataType)
                .str() +
            llvm::formatv(kFamilyNativeHarness, kName, Case.Scratch,
                          Case.Shift ? 1 : Case.Access,
                          unsigned(Case.IndexBytes == kScalarBytes ||
                                   (Case.Word & kFixtureSignedIndexBit)),
                          unsigned(Case.IndexBytes == kWordBytes),
                          Case.Load ? kEmptyNativeArgument
                                    : kStoreNativeArgument,
                          unsigned(Case.Load))
                .str(),
        true, {}, ObjectPath);
#endif
    for (bool LLVMRoute : {false, true}) {
      std::string Source = memorySource(Function, *Module, LLVMRoute);
#if defined(__APPLE__) && defined(__aarch64__)
      Source +=
          llvm::formatv(kFamilyNativeHarness, kName, Case.Scratch,
                        Case.Shift ? 1 : Case.Access,
                        unsigned(Case.IndexBytes == kScalarBytes ||
                                 (Case.Word & kFixtureSignedIndexBit)),
                        unsigned(Case.IndexBytes == kWordBytes),
                        Case.Load ? kEmptyNativeArgument : kStoreNativeArgument,
                        unsigned(Case.Load))
              .str();
      compileMemoryC(Source, true);
#else
      compileMemoryC(Source, false, [&](llvm::MemoryBufferRef Buffer) {
        inspectMemoryObject(Buffer, Case.Scratch);
      });
#endif
    }
  }
}

TEST(AArch64AddressMemory,
     ScalarTracerReadsExactWidthsAndPublishesWholeXRegisters) {
  for (const auto &Case : FamilyCases) {
    SCOPED_TRACE(Case.Word);
    BinaryImage Image;
    Image.Arch = Arch::AArch64;
    Segment Data;
    Data.VA = kEntry;
    Data.Size = Data.FileSz = 3 * kScalarBytes;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.Data.resize(Data.Size);
    writeLE<uint64_t>(Data.Data.data() + kScalarBytes,
                      Case.Load ? kLoadedValue : 0);
    Image.Segments.push_back(Data);
    for (bool Strict : {false, true}) {
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(Strict);
      Emulator.setLoadCollect(true);
      Emulator.setRegister(a64reg::X0, kLoadedValue);
      Emulator.setRegister(a64reg::X1, kEntry + kScalarBytes);
      Emulator.setRegister(
          a64reg::X2, Case.IndexBytes == kWordBytes ? kNarrowHighGarbage : 0);
      const auto Ops = liftMemory(Case.Word, false);
      const auto Op = originalMemoryOperation(Ops);
      for (const auto &Step : Ops)
        ASSERT_TRUE(Emulator.step(Step));
      if (Case.Load) {
        EXPECT_EQ(Emulator.getRegister(a64reg::X0), Case.Expected);
        ASSERT_EQ(Emulator.getLoadRecords().size(), 1);
        EXPECT_EQ(Emulator.getLoadRecords().front().Size, Case.Access);
      } else {
        EXPECT_EQ(Emulator.getRegister(a64reg::X0), kLoadedValue);
        auto Read = liftMemory(kFirst, false).front();
        Emulator.setRegister(a64reg::X2, 0);
        ASSERT_TRUE(Emulator.step(Read));
        EXPECT_EQ(Emulator.getRegister(a64reg::X0), Case.Expected);
        Image.Segments.front().Flags = SegmentFlags::Readable;
        NdOpEmulator Refused(Image);
        Refused.setStrictMode(Strict);
        Refused.setRegister(a64reg::X0, ~kLoadedValue);
        Refused.setRegister(a64reg::X1, kEntry + kScalarBytes);
        Refused.setRegister(a64reg::X2, 0);
        for (const auto &Step : Ops)
          if (Step.Opcode != NdOp::INTRINSIC)
            ASSERT_TRUE(Refused.step(Step));
        EXPECT_FALSE(Refused.step(Op));
        ASSERT_TRUE(Refused.step(Read));
        EXPECT_EQ(Refused.getRegister(a64reg::X0), 0);
        Image.Segments.front().Flags =
            SegmentFlags::Readable | SegmentFlags::Writable;
      }
    }
  }
}

TEST(AArch64AddressMemory, NarrowIndicesNeedOnlyTheirLowWordBeforeExtension) {
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
  for (bool Strict : {false, true}) {
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(Strict);
    Emulator.setRegister(a64reg::X1, kEntry);
    std::array<uint8_t, kWordBytes> Narrow{};
    writeLE<uint32_t>(Narrow.data(), kScalarBytes);
    Emulator.setRegisterBytes(a64reg::X2, Narrow);
    ASSERT_TRUE(Emulator.step(liftMemory(kNarrowUnsigned, false).front()));
    EXPECT_EQ(Emulator.getRegister(a64reg::X0), ~kLoadedValue);
    EXPECT_FALSE(Emulator.step(liftMemory(kFirst, false).front()));
    Emulator.setRegister(a64reg::X1, kEntry + kScalarBytes);
    Emulator.setRegister(a64reg::X2, kNarrowNegative);
    ASSERT_TRUE(Emulator.step(liftMemory(kNarrowSigned, false).front()));
    EXPECT_EQ(Emulator.getRegister(a64reg::X0), kLoadedValue);
  }
}

TEST(AArch64AddressMemory, NarrowStoresRequireExactlyTheirKnownDataSlice) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Segment Data;
  Data.VA = kEntry;
  Data.Size = Data.FileSz = 2 * kScalarBytes;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(Data.Size);
  Image.Segments.push_back(Data);
  std::array<uint8_t, kScalarBytes> Bytes;
  writeLE<uint64_t>(Bytes.data(), kLoadedValue);
  for (const auto &Case : FamilyCases) {
    if (Case.Load || Case.Access >= kWordBytes)
      continue;
    SCOPED_TRACE(Case.Word);
    const auto Ops = liftMemory(Case.Word, false);
    ASSERT_GT(Ops.size(), 1);
    for (bool Strict : {false, true}) {
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(Strict);
      Emulator.setRegister(a64reg::X1, kEntry);
      Emulator.setRegister(a64reg::X2, 0);
      EXPECT_FALSE(Emulator.step(Ops.front()));
      ASSERT_TRUE(Emulator.step(liftMemory(kFirst, false).front()));
      EXPECT_EQ(Emulator.getRegister(a64reg::X0), 0);
      Emulator.setRegisterBytes(
          a64reg::X0, llvm::ArrayRef<uint8_t>(Bytes).take_front(Case.Access));
      for (const auto &Step : Ops)
        ASSERT_TRUE(Emulator.step(Step));
      ASSERT_TRUE(Emulator.step(liftMemory(kFirst, false).front()));
      EXPECT_EQ(Emulator.getRegister(a64reg::X0), Case.Expected);
    }
  }
}

TEST(AArch64AddressMemory, UnobservedStorageCannotCaptureAnObservedRead) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Image.IsRelocatable = true;
  Segment Text;
  Text.Name = kStorageTextName;
  Text.VA = kEntry;
  Text.Size = Text.FileSz = kStorageFunctionCount * kScalarBytes;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size);
  for (unsigned I = 0; I < kStorageFunctionCount; ++I) {
    writeLE<uint32_t>(Text.Data.data() + I * kScalarBytes,
                      kStorageMoveZeroWord);
    writeLE<uint32_t>(Text.Data.data() + I * kScalarBytes + kWordBytes,
                      kReturn);
  }
  Image.Segments.push_back(Text);
  Segment Data;
  Data.Name = kStorageTableName;
  Data.VA = kStorageBase;
  Data.Size = Data.FileSz = 2 * kScalarBytes;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(Data.Size);
  const va_t Target = kEntry + kScalarBytes;
  for (unsigned I = 0; I < 2; ++I) {
    writeLE<uint64_t>(Data.Data.data() + I * kScalarBytes, Target);
    Image.CodePtrRelocSlots.insert(kStorageBase + I * kScalarBytes);
  }
  Image.Segments.push_back(Data);
  Symbol Table;
  Table.Name = kStorageTableName;
  Table.Addr = kStorageBase;
  Table.Size = Data.Size;
  Image.Symbols.push_back(Table);
  for (bool SharedCall : {false, true})
    for (bool SeparateFunctions : {false, true}) {
      const auto Base = HighExpr::makeConst(
          kStorageBase, kScalarBytes, ConstantAddressProvenance::DataAddress);
      const auto Read = [&] {
        auto Call = HighExpr::makeCall(
            intrinsicName(Intrinsic::A64_Load64RegisterOffset), 0,
            {Base, HighExpr::makeConst(0, kScalarBytes),
             HighExpr::makeConst(kFirst, kWordBytes)});
        Call->IntrinsicId = Intrinsic::A64_Load64RegisterOffset;
        Call->Type = NdType::makeInt(kScalarBytes, false);
        return Call;
      };
      const auto Unobserved = Read();
      const auto Observed = SharedCall ? Unobserved : Read();
      HighFunc Reader;
      Reader.Name = kName;
      Reader.Entry = kEntry;
      Reader.ReturnType = NdType::makeInt(kScalarBytes, false);
      HighStmt Effect;
      Effect.Kind = StmtKind::Call;
      Effect.CallExpr = Unobserved;
      HighStmt Return;
      Return.Kind = StmtKind::Return;
      Return.RetVal =
          SeparateFunctions ? HighExpr::makeConst(0, kScalarBytes) : Observed;
      Reader.Body = {Effect, Return};
      HighFunc Callee;
      Callee.Name = kStorageTargetName;
      Callee.Entry = Target;
      Callee.ReturnType = Reader.ReturnType;
      Return.RetVal = HighExpr::makeConst(0, kScalarBytes);
      Callee.Body = {Return};
      std::vector<HighFunc> Functions{Reader, Callee};
      if (SeparateFunctions) {
        HighFunc Other = Reader;
        Other.Name = kStorageOtherName;
        Other.Entry = Target + kScalarBytes;
        Return.RetVal = Observed;
        Other.Body = {Return};
        Functions.push_back(Other);
      }
      CEmitterOptions Options;
      Options.TheArch = Image.Arch;
      Options.Format = Image.Format;
      Options.Image = &Image;
      std::string Source;
      llvm::raw_string_ostream Out(Source);
      ASSERT_TRUE(HighCEmitter().emit(Functions, Out, Options));
      EXPECT_EQ(Source.find(kOriginalStoragePrefix) != std::string::npos,
                !SharedCall || SeparateFunctions)
          << Source;
      Source += llvm::formatv(kStorageIdentityHarness,
                              SeparateFunctions ? kStorageOtherName : kName,
                              kStorageTargetName,
                              SeparateFunctions ? kName : kStorageTargetName)
                    .str();
#if defined(__APPLE__) && defined(__aarch64__)
      compileMemoryC(Source, true);
#else
      compileMemoryC(Source, false);
#endif
    }
}

TEST(AArch64AddressMemory, StoreShapesCannotInventOutputsOrUnusedHighBits) {
  for (const auto &Case : FamilyCases) {
    if (Case.Load)
      continue;
    SCOPED_TRACE(Case.Word);
    auto Function = memoryFunction(Case.Word);
    auto &Med = Function.Blocks.front().Ops.front();
    auto Low = originalMemoryOperation(liftMemory(Case.Word, false));
    ASSERT_TRUE(aarch64AddressMemoryMedShapeIsValid(Med));
    ASSERT_TRUE(lowAArch64AddressMemoryShapeIsValid(Low));
    Med.Output = Function.Params[0];
    Low.Output = Low.Inputs[1];
    EXPECT_FALSE(aarch64AddressMemoryMedShapeIsValid(Med));
    EXPECT_FALSE(lowAArch64AddressMemoryShapeIsValid(Low));
    Med.Output.Size = Low.Output.Size = 0;
    Med.Inputs[4].Size = Low.Inputs[4].Size = Case.Access + 1;
    EXPECT_FALSE(aarch64AddressMemoryMedShapeIsValid(Med));
    EXPECT_FALSE(lowAArch64AddressMemoryShapeIsValid(Low));
  }
}

TEST(AArch64AddressMemory,
     NarrowAliasedIndicesSurviveDecodedSSAAndBothSourceABIs) {
  for (const auto &Case : ABICases) {
    SCOPED_TRACE(Case.Word);
    BinaryImage Image;
    Image.Arch = Arch::AArch64;
    Image.Format = BinaryFormat::MachO;
    Image.Bits = Bitness::Bits64;
    Segment Text;
    Text.VA = kEntry;
    Text.Size = Text.FileSz = 3 * kWordBytes;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(Text.Size);
    writeLE<uint32_t>(Text.Data.data(), Case.Word);
    writeLE<uint32_t>(Text.Data.data() + kWordBytes, kMoveAliasToReturn);
    writeLE<uint32_t>(Text.Data.data() + 2 * kWordBytes, kReturn);
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
    EXPECT_EQ(Function.Params[1].Size, kWordBytes);
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Function}, Context, kName, Arch::AArch64);
    ASSERT_TRUE(Module);
    optimizeMemory(*Module);
    ASSERT_FALSE(llvm::verifyModule(*Module));
    for (bool LLVMRoute : {false, true}) {
      std::string Source = memorySource(Function, *Module, LLVMRoute);
#if defined(__APPLE__) && defined(__aarch64__)
      Source +=
          llvm::formatv(kFamilyNativeHarness, kName, Case.Scratch, 1,
                        unsigned(Case.SignedIndex), 1, kEmptyNativeArgument, 1)
              .str();
      compileMemoryC(Source, true);
#else
      compileMemoryC(Source, false);
#endif
    }
  }
}

TEST(AArch64AddressMemory,
     ScalarFamiliesPreserveZeroStackAndAliasedRegisterFields) {
  for (const auto &Case : SpecialCases) {
    SCOPED_TRACE(Case.Word);
    const auto Instruction = aarch64AddressMemoryInstruction(Case.Word);
    ASSERT_TRUE(Instruction);
    EXPECT_EQ(Instruction->destination(), Case.Destination);
    EXPECT_EQ(Instruction->base(), Case.Base);
    EXPECT_EQ(Instruction->index(), Case.Index);
    for (bool Fast : {false, true})
      for (bool Strict : {false, true}) {
        auto Ops = liftMemory(Case.Word, Fast, Strict);
        ASSERT_FALSE(Ops.empty());
        const auto Memory = originalMemoryOperation(Ops);
        ASSERT_TRUE(lowAArch64AddressMemoryShapeIsValid(Memory));
        EXPECT_EQ(Memory.Inputs[1].Offset, Case.Base == kZeroRegister
                                               ? a64reg::SP
                                               : Case.Base * kScalarBytes);
        EXPECT_EQ(Memory.Inputs[2].Size, kWordBytes);
        EXPECT_EQ(Memory.Inputs[3].Offset, Case.Word);
        if (Case.Load) {
          EXPECT_EQ(Ops.back().Output.Size, kScalarBytes);
          if (Case.Destination == kZeroRegister)
            EXPECT_TRUE(Ops.back().Output.isTemp());
          else
            EXPECT_EQ(Ops.back().Output.Offset,
                      Case.Destination * kScalarBytes);
        } else {
          EXPECT_EQ(Memory.Output.Size, 0);
          EXPECT_EQ(Memory.Inputs[4].Size, Case.Access);
          if (Case.Destination == kZeroRegister) {
            EXPECT_TRUE(Memory.Inputs[4].isConst());
            EXPECT_EQ(Memory.Inputs[4].Offset, 0);
            auto Invalid = Memory;
            Invalid.Inputs[4].Offset = 1;
            EXPECT_FALSE(lowAArch64AddressMemoryShapeIsValid(Invalid));
          } else if (Case.Access < kWordBytes) {
            EXPECT_TRUE(Memory.Inputs[4].isTemp());
            EXPECT_EQ(Ops.front().Opcode, NdOp::SUBBYTES);
            EXPECT_EQ(Ops.front().Inputs[0].Offset,
                      Case.Destination * kScalarBytes);
            EXPECT_EQ(Ops.front().Inputs[0].Size, kWordBytes);
            EXPECT_EQ(Ops.front().Output, Memory.Inputs[4]);
          } else
            EXPECT_EQ(Memory.Inputs[4].Offset, Case.Destination * kScalarBytes);
        }
        if (Case.Index == kZeroRegister) {
          EXPECT_TRUE(Memory.Inputs[2].isConst());
          EXPECT_EQ(Memory.Inputs[2].Offset, 0);
          auto Invalid = Memory;
          Invalid.Inputs[2].Offset = 1;
          EXPECT_FALSE(lowAArch64AddressMemoryShapeIsValid(Invalid));
        } else
          EXPECT_EQ(Memory.Inputs[2].Offset, Case.Index * kScalarBytes);
      }
    if (Case.Base == kZeroRegister) {
      const auto Function = memoryFunction(Case.Word);
      llvm::LLVMContext Context;
      EXPECT_THROW(
          MedLLVMEmitter().emit({Function}, Context, kName, Arch::AArch64),
          std::runtime_error);
    }
  }
}
