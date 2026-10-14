//===- RegisterPairReturnTests.cpp - Complete high-word return evidence --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Register-pair inference needs a high-word carrier at every return, on
/// both x86 and ARM32, including joins and independent entry components.
///
//===----------------------------------------------------------------------===//

#include "../../../lib/pipeline/PipelineWideIntReturnDetail.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/raw_ostream.h"

using namespace neverd;

namespace {

class RegisterPairReturnTest : public testing::TestWithParam<Arch> {
protected:
  MedVar high(int Version = 1) const {
    MedVar Value;
    Value.Kind = MedVar::Reg;
    Value.TheArch = GetParam();
    Value.RegOff = getTargetRegInfo(GetParam()).IntReturnReg2;
    Value.Id = 1;
    Value.SSAVer = Version;
    Value.Size = 4;
    return Value;
  }

  MedOp defineHigh(int Version = 1) const {
    MedOp Op;
    Op.Opcode = NdOp::COPY;
    Op.Output = high(Version);
    Op.addInput(MedVar::makeConst(0x12345678, 4));
    return Op;
  }

  MedOp returning() const {
    MedOp Op;
    Op.Opcode = NdOp::RETURN;
    Op.Addr = 0x1010;
    return Op;
  }

  MedOp call() const {
    MedOp Op;
    Op.Opcode = NdOp::CALL;
    Op.Addr = 0x1010;
    Op.addInput(MedVar::makeConst(0x2000, 4));
    return Op;
  }

  bool accepts(std::vector<MedBlock> Blocks) const {
    MedFunc Func;
    Func.Blocks = std::move(Blocks);
    for (size_t I = 0; I < Func.Blocks.size(); ++I)
      Func.Blocks[I].Id = static_cast<int>(I);
    return returnsRegisterPairI64(Func, getTargetRegInfo(GetParam()));
  }
};

TEST_P(RegisterPairReturnTest, RequiresEveryIndependentReturn) {
  MedBlock First, Second;
  First.Ops = {defineHigh(), returning()};
  Second.Ops = {returning()};
  EXPECT_TRUE(accepts({First}));
  EXPECT_FALSE(accepts({First, Second}));
  Second.Ops.insert(Second.Ops.begin(), defineHigh(2));
  EXPECT_TRUE(accepts({First, Second}));
}

TEST_P(RegisterPairReturnTest, CarriesHighWordThroughTransparentPredecessors) {
  MedBlock First, Middle, Last;
  First.Ops = {defineHigh()};
  First.Succs = {1};
  Middle.Preds = {0};
  Middle.Succs = {2};
  Last.Preds = {1};
  Last.Ops = {returning()};
  EXPECT_TRUE(accepts({First, Middle, Last}));
  Last.Preds.clear();
  EXPECT_FALSE(accepts({First, Middle, Last}));
}

TEST_P(RegisterPairReturnTest, DoesNotUseHighWordBeforeClobberingCall) {
  MedBlock Block;
  Block.Ops = {defineHigh(), call(), returning()};
  EXPECT_FALSE(accepts({Block}));
  Block.Ops[1].PreservesCallerSaved = true;
  EXPECT_TRUE(accepts({Block}));
  Block.Ops = {call(), defineHigh(), returning()};
  EXPECT_TRUE(accepts({Block}));
}

TEST_P(RegisterPairReturnTest, RejectsEntryIdentityUnknownAndPartialWrites) {
  MedBlock Block;
  Block.Ops = {defineHigh(), returning()};
  Block.Ops[0].Inputs[0] = Block.Ops[0].Output;
  EXPECT_FALSE(accepts({Block}));
  Block.Ops[0].Inputs[0] = MedVar::makeUnspecified(4, GetParam());
  EXPECT_FALSE(accepts({Block}));
  Block.Ops[0] = defineHigh();
  Block.Ops[0].Output.Size = 1;
  EXPECT_FALSE(accepts({Block}));
}

TEST_P(RegisterPairReturnTest, RequiresAllPhiInputsIncludingExceptionalEntry) {
  MedBlock Left, Right, Join;
  Left.Ops = {defineHigh(1)};
  Right.Ops = {defineHigh(2)};
  Left.Succs = Right.Succs = {2};
  Join.Preds = {0, 1};
  Join.Ops = {returning()};
  PhiNode Phi;
  Phi.Output = high(3);
  Phi.Args = {{0, high(1)}, {1, high(2)}};
  Join.Phis = {Phi};
  EXPECT_TRUE(accepts({Left, Right, Join}));
  Join.Phis[0].Args[1].second = high(0);
  EXPECT_FALSE(accepts({Left, Right, Join}));
  Join.Phis[0] = Phi;
  Join.Phis[0].ExceptionalEntry = MedVar::makeUnspecified(4, GetParam());
  EXPECT_FALSE(accepts({Left, Right, Join}));
}

TEST_P(RegisterPairReturnTest, PhiCycleCannotInventItsOwnDefinition) {
  MedBlock Block;
  Block.Preds = Block.Succs = {0};
  Block.Ops = {returning()};
  PhiNode Phi;
  Phi.Output = high(1);
  Phi.Args = {{0, high(1)}};
  Block.Phis = {Phi};
  EXPECT_FALSE(accepts({Block}));
}

TEST_P(RegisterPairReturnTest, KnownTailCallDoesNotJustifyAnotherReturn) {
  MedFunc Func;
  MedBlock Block;
  Block.Id = 0;
  Block.Ops = {call(), returning()};
  Func.Blocks = {Block};
  const auto &TRI = getTargetRegInfo(GetParam());
  auto Known = [](const MedOp &Call) {
    return Call.NumInputs == 1 && Call.Inputs[0].ConstVal == 0x2000;
  };
  EXPECT_TRUE(returnsRegisterPairI64(Func, TRI, Known));
  EXPECT_FALSE(returnsRegisterPairI64(Func, TRI));
  Block.Id = 1;
  Block.Ops = {returning()};
  Func.Blocks.push_back(Block);
  EXPECT_FALSE(returnsRegisterPairI64(Func, TRI, Known));
}

TEST(RegisterPairReturnPipeline, CallerLivenessCannotWidenAnIncompleteCallee) {
  for (bool ImportThunk : {false, true}) {
    SCOPED_TRACE(ImportThunk);
    BinaryImage Image;
    Image.Arch = Arch::X86;
    Image.Bits = Bitness::Bits32;
    Image.Format = BinaryFormat::COFF;
    Image.Entry = 0x1000;
    Segment Text;
    Text.Name = ".text";
    Text.VA = 0x1000;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    // call 0x1100; xor eax, edx; ret -- reads both prospective halves.
    Text.Data = {0xe8, 0xfb, 0, 0, 0, 0x31, 0xd0, 0xc3};
    Text.Data.resize(0x100, 0xcc);
    const std::vector<uint8_t> Callee =
        ImportThunk
            ? std::vector<uint8_t>{0xff, 0x25, 0, 0x20, 0, 0}
            // test ecx, ecx; je low_only; mov edx, 1; low_only: mov eax, 2; ret
            : std::vector<uint8_t>{0x85, 0xc9, 0x74, 5, 0xba, 1, 0,   0,
                                   0,    0xb8, 2,    0, 0,    0, 0xc3};
    Text.Data.insert(Text.Data.end(), Callee.begin(), Callee.end());
    Text.Size = Text.FileSz = Text.Data.size();
    Image.Segments.push_back(std::move(Text));
    if (ImportThunk) {
      Import Import;
      Import.Name = "memset";
      Import.Module = "msvcrt.dll";
      Import.IATAddr = 0x2000;
      Image.Imports.push_back(std::move(Import));
      Segment Data;
      Data.Name = ".idata";
      Data.VA = 0x2000;
      Data.Size = Data.FileSz = 4;
      Data.Flags = SegmentFlags::Readable;
      Data.Data.resize(4);
      Image.Segments.push_back(std::move(Data));
    }
    for (va_t Entry : {0x1000, 0x1100}) {
      Symbol Sym = Symbol::makeFunc(Entry);
      Sym.Name = Entry == 0x1000 ? "caller" : "target";
      Image.Symbols.push_back(std::move(Sym));
    }
    PipelineOptions Options;
    Options.NoOpt = true;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries = {0x1000, 0x1100};
    llvm::LLVMContext Context;
    const auto Result = Pipeline().run(Image, Context, Options);
    ASSERT_TRUE(Result.Success) << Result.Error;
    bool Found = false;
    for (const MedFunc &Func : Result.MedFuncs)
      if (Func.Entry == 0x1100) {
        ASSERT_TRUE(Func.ReturnType);
        EXPECT_EQ(Func.ReturnType->Size, 4);

        Found = true;
      }
    ASSERT_TRUE(Found);
    std::string Source;
    llvm::raw_string_ostream Stream(Source);
    CEmitterOptions COptions;
    COptions.TheArch = Image.Arch;
    COptions.Format = Image.Format;
    COptions.Image = &Image;
    ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Stream, COptions));
    EXPECT_EQ(Source.find("unknown return value"), std::string::npos) << Source;
    if (ImportThunk)
      EXPECT_NE(Source.find("memset("), std::string::npos) << Source;
  }
}

INSTANTIATE_TEST_SUITE_P(Targets, RegisterPairReturnTest,
                         testing::Values(Arch::X86, Arch::ARM),
                         [](const testing::TestParamInfo<Arch> &Info) {
                           return Info.param == Arch::X86 ? "X86" : "ARM32";
                         });

} // namespace
