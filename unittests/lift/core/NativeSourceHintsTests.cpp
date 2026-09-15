#include "gtest/gtest.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/pipeline/NativeSourceHints.h"
#include "neverd/pipeline/Pipeline.h"

#include <algorithm>
#include <array>

using namespace neverd;

namespace {
struct NativeFixture {
  BinaryImage Image;
  MedFunc Med;
  HighFunc High;
  PipelineFunctionAudit Audit;

  explicit NativeFixture(Arch Architecture = Arch::AArch64) {
    Image.Arch = Architecture;
    Image.Format = BinaryFormat::MachO;
    Image.Bits = Bitness::Bits64;
    Segment Text;
    Text.VA = 0x1000;
    Text.Size = Text.FileSz = 0x100;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(0x100);
    Image.Segments.push_back(std::move(Text));
    const auto &TRI = getTargetRegInfo(Architecture);
    Med.Entry = High.Entry = Audit.Entry = 0x1000;
    Med.ReturnType = High.ReturnType = NdType::makeInt(4);
    Med.Blocks.emplace_back();
    Med.Blocks[0].Id = 0;
    for (unsigned Index = 0; Index < 2; ++Index) {
      MedVar Parameter;
      Parameter.Kind = MedVar::Param;
      Parameter.Id = Index;
      Parameter.RegOff = TRI.IntParamRegs[Index];
      Parameter.Size = 4;
      Parameter.TheArch = Architecture;
      Med.Params.push_back(Parameter);
      Med.TypedParams.push_back(
          {"arg" + std::to_string(Index), NdType::makeInt(4)});
      High.Params.push_back(
          {"arg" + std::to_string(Index), NdType::makeInt(4)});
    }
    MedOp Add;
    Add.Opcode = NdOp::INT_ADD;
    Add.Output.Kind = MedVar::Reg;
    Add.Output.Id = 10;
    Add.Output.SSAVer = 1;
    Add.Output.TheArch = Architecture;
    Add.Output.RegOff = TRI.IntReturnReg;
    Add.Output.Size = 4;
    Add.addInput(Med.Params[0]);
    Add.addInput(Med.Params[1]);
    Med.Blocks[0].Ops.push_back(Add);
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Med.Blocks[0].Ops.push_back(Return);
    HighStmt Statement;
    Statement.Kind = StmtKind::Return;
    Statement.RetVal = HighExpr::makeConst(42, 4);
    High.Body.push_back(std::move(Statement));
    Audit.Disposition = PipelineFunctionDisposition::Accepted;
    Audit.HasLowIR = Audit.HasMedIR = Audit.MedIRVerified = true;
    Audit.DecodedInstructions = Audit.LiftedInstructions = 2;
  }

  std::optional<SourceFunctionTypeHint> infer(std::string &Error) const {
    return inferNativeSourceTypeHint(Image, Med, High, Audit, Error);
  }
};

TEST(NativeSourceHints, KeepsObservedIntegerLocationsWithoutUsingNames) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeFixture Fixture(Architecture);
    Fixture.Med.Name = "unrelated_stripped_symbol";
    std::string Error;
    auto Hint = Fixture.infer(Error);
    ASSERT_TRUE(Hint) << Error;
    EXPECT_EQ(Hint->Origin, SourceFunctionTypeHint::OriginKind::NativeAnalysis);
    ASSERT_EQ(Hint->Parameters.size(), 2U);
    const auto &TRI = getTargetRegInfo(Architecture);
    EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, TRI.IntParamRegs[0]);
    EXPECT_EQ(Hint->Parameters[1].Location.RegisterOffset, TRI.IntParamRegs[1]);
    EXPECT_EQ(Hint->ReturnLocation.RegisterOffset, TRI.IntReturnReg);
    EXPECT_EQ(Hint->ReturnLocation.ValueBytes, 4U);
    EXPECT_EQ(Fixture.Med.ReturnValueEvidence, MedReturnValueEvidence::Unknown);
  }
}

TEST(NativeSourceHints, OmittedUnusedArgumentDoesNotShiftPhysicalRegister) {
  NativeFixture Fixture;
  Fixture.Med.Params[0].Id = -1;
  Fixture.Med.Blocks[0].Ops[0].Inputs[0] = MedVar::makeConst(3, 4);
  std::string Error;
  auto Hint = Fixture.infer(Error);
  ASSERT_TRUE(Hint) << Error;
  ASSERT_EQ(Hint->Parameters.size(), 1U);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset,
            getTargetRegInfo(Arch::AArch64).IntParamRegs[1]);
}

TEST(NativeSourceHints, ConstantFunctionNeedsNoInventedArguments) {
  NativeFixture Fixture;
  Fixture.Med.Params.clear();
  Fixture.Med.TypedParams.clear();
  Fixture.High.Params.clear();
  auto &Op = Fixture.Med.Blocks[0].Ops[0];
  Op.Opcode = NdOp::COPY;
  Op.NumInputs = 1;
  Op.Inputs[0] = MedVar::makeConst(42, 4);
  std::string Error;
  auto Hint = Fixture.infer(Error);
  ASSERT_TRUE(Hint) << Error;
  EXPECT_TRUE(Hint->Parameters.empty());
}

TEST(NativeSourceHints, RejectsIncompleteAuditAndMismatchedFunctionIdentity) {
  for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
    NativeFixture Fixture;
    if (Mutation == 0)
      Fixture.Audit.TruncatedPaths.push_back(0x1004);
    if (Mutation == 1)
      --Fixture.Audit.LiftedInstructions;
    if (Mutation == 2)
      Fixture.Audit.MedIRVerified = false;
    if (Mutation == 3)
      Fixture.High.Entry += 4;
    if (Mutation == 4)
      Fixture.Image.IsRelocatable = true;
    if (Mutation == 5)
      Fixture.Image.Segments[0].Flags = SegmentFlags::Readable;
    std::string Error;
    EXPECT_FALSE(Fixture.infer(Error)) << Mutation;
    EXPECT_FALSE(Error.empty());
  }
}

TEST(NativeSourceHints, RejectsRegisterBankTypeWidthAndDuplicateAmbiguity) {
  for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
    NativeFixture Fixture;
    if (Mutation == 0)
      Fixture.Med.Params[0].RegOff =
          getTargetRegInfo(Arch::AArch64).FPParamRegs[0];
    if (Mutation == 1)
      Fixture.Med.TypedParams[0].Type = NdType::makeFloat(4);
    if (Mutation == 2)
      Fixture.Med.Params[0].Size = 8;
    if (Mutation == 3)
      Fixture.Med.Params[1].RegOff = Fixture.Med.Params[0].RegOff;
    if (Mutation == 4)
      Fixture.High.ReturnType = NdType::makeInt(8);
    std::string Error;
    EXPECT_FALSE(Fixture.infer(Error)) << Mutation;
  }
}

TEST(NativeSourceHints, UnknownCallsCannotInventResultOrArgumentTypes) {
  NativeFixture Fixture;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.addInput(MedVar::makeConst(0x1020, 8));
  Fixture.Med.Blocks[0].Ops.insert(Fixture.Med.Blocks[0].Ops.begin(), Call);
  std::string Error;
  EXPECT_FALSE(Fixture.infer(Error));
  EXPECT_NE(Error.find("without a source binding"), std::string::npos);
}

TEST(NativeSourceHints, EachReturnRequiresComputedCarrierNotStaleLiveIn) {
  for (unsigned Mutation = 0; Mutation < 3; ++Mutation) {
    NativeFixture Fixture;
    auto &Op = Fixture.Med.Blocks[0].Ops[0];
    if (Mutation == 0) {
      Op.Opcode = NdOp::COPY;
      Op.NumInputs = 1;
      Op.Inputs[0] = Op.Output;
    }
    if (Mutation == 1)
      Op.Output.Size = 2;
    if (Mutation == 2) {
      MedBlock Other;
      Other.Id = 1;
      Other.Ops.push_back(Fixture.Med.Blocks[0].Ops.back());
      Fixture.Med.Blocks.push_back(Other);
    }
    std::string Error;
    EXPECT_FALSE(Fixture.infer(Error)) << Mutation;
  }
}

// The source hint consumes already verified MedIR. These graphs isolate the
// all-path carrier proof; the runtime fixture exercises real lifting and SSA.
void returnDiamond(NativeFixture &Fixture) {
  const auto Compute = Fixture.Med.Blocks[0].Ops.front();
  const auto Return = Fixture.Med.Blocks[0].Ops.back();
  Fixture.Med.Blocks.resize(4);
  for (unsigned I = 0; I < 4; ++I) {
    auto &Block = Fixture.Med.Blocks[I];
    Block.Id = I;
    Block.StartAddr = Fixture.Med.Entry + I * 16;
    Block.Ops.clear();
  }
  auto &Blocks = Fixture.Med.Blocks;
  Blocks[0].Succs = {1, 2};
  for (unsigned I : {1U, 2U}) {
    Blocks[I].Preds = {0};
    Blocks[I].Succs = {3};
    Blocks[I].Ops = {Compute};
  }
  Blocks[3].Preds = {1, 2};
  Blocks[3].Ops = {Return};
}

TEST(NativeSourceHints, ComputedReturnsMergeAcrossEveryPathAndBlockOrder) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (bool Complete : {false, true}) {
      NativeFixture Fixture(Architecture);
      // Isolate computed results from the separately proven entry carrier.
      Fixture.Med.Params[0].Id = -1;
      Fixture.Med.Blocks[0].Ops[0].Inputs[0] = MedVar::makeConst(3, 4);
      returnDiamond(Fixture);
      if (!Complete)
        Fixture.Med.Blocks[2].Ops.clear();
      const auto Blocks = Fixture.Med.Blocks;
      std::array<unsigned, 4> Order{0, 1, 2, 3};
      do {
        for (unsigned I = 0; I < 4; ++I)
          Fixture.Med.Blocks[I] = Blocks[Order[I]];
        std::string Error;
        EXPECT_EQ(bool(Fixture.infer(Error)), Complete) << Error;
        EXPECT_EQ(Fixture.Med.ReturnValueEvidence,
                  MedReturnValueEvidence::Unknown);
      } while (std::next_permutation(Order.begin(), Order.end()));
    }
  }
}

TEST(NativeSourceHints, ReturnLoopsRequireComputationOnTheirEntryPath) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (bool Seeded : {false, true}) {
      NativeFixture Fixture(Architecture);
      Fixture.Med.Params[0].Id = -1;
      Fixture.Med.Blocks[0].Ops[0].Inputs[0] = MedVar::makeConst(3, 4);
      returnDiamond(Fixture);
      auto &Blocks = Fixture.Med.Blocks;
      // Entry -> header -> body -> header, or header -> return. Computing
      // only in the body cannot prove a result on the zero-trip path.
      if (Seeded)
        Blocks[0].Ops = Blocks[2].Ops;
      Blocks[0].Succs = {1};
      Blocks[1].Ops.clear();
      Blocks[1].Preds = {0, 2};
      Blocks[1].Succs = {2, 3};
      Blocks[2].Preds = {1};
      Blocks[2].Succs = {1};
      Blocks[3].Preds = {1};
      std::string Error;
      EXPECT_EQ(bool(Fixture.infer(Error)), Seeded) << Error;
      // A disconnected cycle cannot establish its own incoming fact either.
      Blocks[0].Succs.clear();
      Blocks[1].Preds = {2};
      EXPECT_FALSE(Fixture.infer(Error));
    }
  }
}

TEST(NativeSourceHints, ReturnPathsInvalidateCallsAndPartialCarrierWrites) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
      NativeFixture Fixture(Architecture);
      returnDiamond(Fixture);
      auto &Ops = Fixture.Med.Blocks[2].Ops;
      MedOp Overwrite = Ops.front();
      Overwrite.Opcode = NdOp::COPY;
      Overwrite.NumInputs = 1;
      Overwrite.Inputs[0] = MedVar::makeConst(7, 4);
      if (Mutation == 0)
        Overwrite.Output.Size = 2;
      if (Mutation == 1) {
        ++Overwrite.Output.RegOff;
        Overwrite.Output.Size = 1;
      }
      if (Mutation == 2) {
        Overwrite.Opcode = NdOp::CALL;
        Overwrite.Output = {};
        Overwrite.Inputs[0] = MedVar::makeConst(0x1020, 8);
        auto Hint = std::make_shared<SourceCallTypeHint>();
        Hint->Signature.ReturnType = NdType::makeVoid();
        std::string Error;
        ASSERT_TRUE(
            assignDarwinScalarSourceABI(Hint->Signature, Architecture, Error))
            << Error;
        Overwrite.SourceCallHint = std::move(Hint);
      }
      if (Mutation == 3)
        Overwrite.Inputs[0] = Overwrite.Output; // SSA seed, not a write.
      if (Mutation == 4) {
        Overwrite.Opcode = NdOp::SUBBYTES; // Register view, not a write.
        Overwrite.Output.Size = 1;
        Overwrite.addInput(MedVar::makeConst(0, 4));
      }
      Ops.push_back(Overwrite);
      std::string Error;
      EXPECT_EQ(bool(Fixture.infer(Error)), Mutation >= 3) << Mutation << Error;
      // A subsequent complete computation reestablishes the carrier.
      Ops.push_back(Ops.front());
      EXPECT_TRUE(Fixture.infer(Error)) << Mutation << Error;
    }
  }
}

TEST(NativeSourceHints, ReturnPathsRejectUnobservedInputAndEpilogueRestores) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeFixture Fixture(Architecture);
    returnDiamond(Fixture);
    for (unsigned I : {1U, 2U}) {
      auto &Op = Fixture.Med.Blocks[I].Ops.front();
      Op.Opcode = NdOp::COPY;
      Op.NumInputs = 1;
      Op.Inputs[0] = Op.Output;
    }
    std::string Error;
    EXPECT_FALSE(Fixture.infer(Error));
  }
  NativeFixture Fixture(Arch::X64);
  returnDiamond(Fixture);
  auto &Restore = Fixture.Med.Blocks[2].Ops.front();
  Restore.Opcode = NdOp::LOAD;
  Restore.NumInputs = 1;
  Restore.Inputs[0] = Restore.Output;
  Restore.Inputs[0].RegOff = getTargetRegInfo(Arch::X64).StackPointer;
  std::string Error;
  EXPECT_FALSE(Fixture.infer(Error));
}

TEST(NativeSourceHints, ObservedIncomingResultsKeepExactWidthsAndLocations) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (uint16_t Width : {1, 2, 4, 8}) {
      NativeFixture Fixture(Architecture);
      Fixture.Med.ReturnType = Fixture.High.ReturnType = NdType::makeInt(Width);
      for (unsigned I = 0; I < 2; ++I) {
        Fixture.Med.Params[I].Size = Width;
        Fixture.Med.TypedParams[I].Type = Fixture.High.Params[I].Type =
            NdType::makeInt(Width);
        Fixture.Med.Blocks[0].Ops[0].Inputs[I] = Fixture.Med.Params[I];
      }
      Fixture.Med.Blocks[0].Ops[0].Output.Size = Width;
      returnDiamond(Fixture);
      Fixture.Med.Blocks[2].Ops.clear();
      const auto Blocks = Fixture.Med.Blocks;
      std::array<unsigned, 4> Order{0, 1, 2, 3};
      do {
        for (unsigned I = 0; I < 4; ++I)
          Fixture.Med.Blocks[I] = Blocks[Order[I]];
        std::string Error;
        const auto Hint = Fixture.infer(Error);
        const auto &TRI = getTargetRegInfo(Architecture);
        EXPECT_EQ(bool(Hint), TRI.IntParamRegs[0] == TRI.IntReturnReg) << Error;
        if (Hint) {
          EXPECT_EQ(Hint->ReturnLocation.ValueBytes, Width);
          EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset,
                    TRI.IntReturnReg);
          EXPECT_EQ(Hint->Parameters[0].Location.ValueBytes, Width);
        }
      } while (std::next_permutation(Order.begin(), Order.end()));
    }
}

TEST(NativeSourceHints, IncomingResultsRejectUnobservedNarrowAndLaterVersions) {
  for (unsigned Mutation = 0; Mutation < 7; ++Mutation) {
    NativeFixture Fixture;
    returnDiamond(Fixture);
    Fixture.Med.Blocks[2].Ops.clear();
    auto &Use = Fixture.Med.Blocks[1].Ops[0].Inputs[0];
    if (Mutation == 0)
      Use = MedVar::makeConst(3, 4);
    if (Mutation == 1)
      Use.Size = 2;
    if (Mutation == 2) {
      Use.Kind = MedVar::Reg;
      Use.SSAVer = 1;
    }
    if (Mutation == 3) {
      Fixture.Med.Params[0].Size = 2;
      Fixture.Med.TypedParams[0].Type = Fixture.High.Params[0].Type =
          NdType::makeInt(2);
      Use = Fixture.Med.Params[0];
    }
    if (Mutation == 4) {
      // A matching parameter listed only in an unreachable component does
      // not make the actual entry register an observed input.
      Fixture.Med.Blocks[0].Succs = {2};
      Fixture.Med.Blocks[1].Preds.clear();
    }
    if (Mutation == 5 || Mutation == 6) {
      Use.Kind = MedVar::Reg;
      Use.Id = 60;
      Use.SSAVer = 0;
      // An ordinary definition or a PHI may own the first SSA version.
      // Neither is evidence that the parameter's incoming bytes were used.
      if (Mutation == 5) {
        MedOp Internal;
        Internal.Opcode = NdOp::COPY;
        Internal.Output = Use;
        Internal.addInput(MedVar::makeConst(42, 4));
        auto &Ops = Fixture.Med.Blocks[1].Ops;
        Ops.insert(Ops.begin(), Internal);
      } else {
        PhiNode Phi;
        Phi.Output = Use;
        Phi.Args = {{0, MedVar::makeConst(42, 4)}};
        Fixture.Med.Blocks[1].Phis.push_back(std::move(Phi));
      }
    }
    std::string Error;
    EXPECT_FALSE(Fixture.infer(Error)) << Mutation;
  }
}

TEST(NativeSourceHints, IncomingResultFactsInvalidateNarrowedSelfCopies) {
  for (uint16_t WriteWidth : {2, 4}) {
    NativeFixture Fixture;
    returnDiamond(Fixture);
    auto &Ops = Fixture.Med.Blocks[2].Ops;
    MedOp Copy = Ops.front();
    Copy.Opcode = NdOp::COPY;
    Copy.NumInputs = 1;
    Copy.Inputs[0] = Copy.Output;
    Copy.Output.Size = WriteWidth;
    Ops = {Copy};
    std::string Error;
    EXPECT_EQ(bool(Fixture.infer(Error)), WriteWidth == 4) << Error;
  }
}

TEST(NativeSourceHints, IncomingResultBackedgesMeetInitialAndClobberedStates) {
  for (bool Clobber : {false, true}) {
    NativeFixture Fixture;
    const auto Return = Fixture.Med.Blocks[0].Ops.back();
    MedOp Observe;
    Observe.Opcode = NdOp::COPY;
    Observe.Output.Kind = MedVar::Temp;
    Observe.Output.Id = 50;
    Observe.Output.Size = 4;
    Observe.addInput(Fixture.Med.Params[0]);
    Fixture.Med.Blocks.resize(3);
    for (unsigned I = 0; I < 3; ++I) {
      Fixture.Med.Blocks[I].Id = I;
      Fixture.Med.Blocks[I].StartAddr = Fixture.Med.Entry + I * 16;
    }
    auto &Blocks = Fixture.Med.Blocks;
    Blocks[0].Ops = {Observe};
    Blocks[0].Preds = {1};
    Blocks[0].Succs = {1, 2};
    Blocks[1].Preds = {0};
    Blocks[1].Succs = {0};
    Blocks[2].Preds = {0};
    Blocks[2].Ops = {Return};
    if (Clobber) {
      MedOp Call;
      Call.Opcode = NdOp::CALL;
      Call.addInput(MedVar::makeConst(0x1020, 8));
      auto Hint = std::make_shared<SourceCallTypeHint>();
      Hint->Signature.ReturnType = NdType::makeVoid();
      std::string Error;
      ASSERT_TRUE(
          assignDarwinScalarSourceABI(Hint->Signature, Arch::AArch64, Error));
      Call.SourceCallHint = std::move(Hint);
      Blocks[1].Ops = {Call};
    }
    std::string Error;
    EXPECT_EQ(bool(Fixture.infer(Error)), !Clobber) << Error;
  }
}

TEST(NativeSourceHints, ReturnProofRejectsMalformedOrUnboundedControlFlow) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation < 10; ++Mutation) {
      NativeFixture Fixture(Architecture);
      returnDiamond(Fixture);
      auto &Blocks = Fixture.Med.Blocks;
      if (Mutation == 0)
        Blocks[0].Succs.push_back(99);
      if (Mutation == 1)
        Blocks[0].Succs.push_back(1);
      if (Mutation == 2)
        Blocks[1].Preds.push_back(0);
      if (Mutation == 3)
        Blocks[1].Preds.clear();
      if (Mutation == 4)
        Blocks[1].Id = Blocks[0].Id;
      if (Mutation == 5)
        Blocks[0].Id = -1;
      if (Mutation == 6)
        Blocks[1].StartAddr = Fixture.Med.Entry;
      if (Mutation == 7)
        Blocks[0].StartAddr += 4;
      if (Mutation == 8)
        Blocks.resize(16385);
      if (Mutation == 9)
        Blocks[1].Ops.resize(262144, Blocks[1].Ops.front());
      std::string Error;
      EXPECT_FALSE(Fixture.infer(Error)) << Mutation;
      EXPECT_FALSE(Error.empty()) << Mutation;
    }
  }
}

TEST(NativeSourceHints, KeepsCompleteStackSlotAndRejectsPackedSlices) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeFixture Fixture(Architecture);
    auto &Parameter = Fixture.Med.Params[1];
    Parameter.RegOff = kNoParamReg;
    Parameter.Id = getTargetRegInfo(Architecture).IntParamRegs.size() + 2;
    Parameter.Size = 8;
    Fixture.Med.TypedParams[1].Type = NdType::makeInt(8);
    Fixture.Med.Blocks[0].Ops[0].Inputs[1] = Parameter;
    std::string Error;
    auto Hint = Fixture.infer(Error);
    ASSERT_TRUE(Hint) << Error;
    EXPECT_EQ(Hint->Parameters[1].Location.EntryStackOffset,
              Architecture == Arch::X64 ? 24 : 16);
    auto &Op = Fixture.Med.Blocks[0].Ops[0];
    Op.Opcode = NdOp::SUBBYTES;
    Op.Inputs[0] = Parameter;
    Op.Inputs[1] = MedVar::makeConst(4, 4);
    EXPECT_FALSE(Fixture.infer(Error));
    EXPECT_NE(Error.find("partial"), std::string::npos);
  }
}

TEST(NativeSourceHints,
     GenericScalarAllocatorDoesNotAddObjectiveCHiddenValues) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    SourceFunctionTypeHint Hint;
    Hint.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Hint.ReturnType = NdType::makeFloat(8);
    Hint.Parameters = {{"integer", NdType::makeInt(4)},
                       {"floating", NdType::makeFloat(8)}};
    std::string Error;
    ASSERT_TRUE(assignDarwinScalarSourceABI(Hint, Architecture, Error))
        << Error;
    const auto &TRI = getTargetRegInfo(Architecture);
    EXPECT_EQ(Hint.Parameters[0].Location.RegisterOffset, TRI.IntParamRegs[0]);
    EXPECT_EQ(Hint.Parameters[1].Location.RegisterOffset, TRI.FPParamRegs[0]);
    EXPECT_FALSE(assignDarwinObjCSourceABI(Hint, Architecture, Error));
  }
}

TEST(NativeSourceHints, InferenceSurvivesRealLowMedHighScalarPipeline) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    NativeFixture Fixture(Architecture);
    const auto &TRI = getTargetRegInfo(Architecture);
    LowFunc Low;
    Low.Entry = Fixture.Med.Entry;
    Low.Name = "scalar_helper";
    LowBlock Block;
    Block.Id = 0;
    Block.StartAddr = Low.Entry;
    Block.EndAddr = Low.Entry + 8;
    LowOp Add;
    Add.Opcode = NdOp::INT_ADD;
    Add.Addr = Low.Entry;
    Add.Output = NdVar::reg(TRI.IntReturnReg, 4);
    Add.addInput(NdVar::reg(TRI.IntParamRegs[0], 4));
    Add.addInput(NdVar::reg(TRI.IntParamRegs[1], 4));
    Block.Ops.push_back(Add);
    LowOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = Low.Entry + 4;
    Return.addInput(NdVar::reg(TRI.IntReturnReg, 4));
    Block.Ops.push_back(Return);
    Low.Blocks.push_back(Block);
    Fixture.Med =
        LowToMedConverter().convert(Low, Architecture, BinaryFormat::MachO);
    inferMedTypes(Fixture.Med, Architecture);
    Fixture.High = MedToHighConverter().convert(Fixture.Med, Architecture);
    std::string Error;
    auto Hint = Fixture.infer(Error);
    ASSERT_TRUE(Hint) << Error;
    ASSERT_EQ(Hint->Parameters.size(), 2U);
    Fixture.Med.SourceTypeHint = *Hint;
    inferMedTypes(Fixture.Med, Architecture);
    ASSERT_TRUE(Fixture.Med.SourceTypeHint);
    ASSERT_TRUE(Fixture.Med.SourceParametersBound);
    Fixture.High = MedToHighConverter().convert(Fixture.Med, Architecture);
    ASSERT_EQ(Fixture.High.Params.size(), 2U);
    EXPECT_EQ(Fixture.High.Params[0].Name, "native_arg0");
    EXPECT_EQ(Fixture.High.Params[1].Name, "native_arg1");
  }
}

void addPointerCall(NativeFixture &Fixture) {
  for (size_t Index = 0; Index < Fixture.Med.Params.size(); ++Index) {
    Fixture.Med.Params[Index].Size = 8;
    Fixture.Med.TypedParams[Index].Type = NdType::makeInt(8);
    Fixture.High.Params[Index].Type = NdType::makeInt(8);
    Fixture.Med.Blocks[0].Ops[0].Inputs[Index] = MedVar::makeConst(21, 4);
  }
  auto CallHint = std::make_shared<SourceCallTypeHint>();
  CallHint->Signature.Origin =
      SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  CallHint->Signature.ReturnType = NdType::makeVoid();
  CallHint->Signature.Parameters = {
      {"first", NdType::makePtr(NdType::makeVoid())},
      {"second", NdType::makePtr(NdType::makeVoid())}};
  std::string Error;
  ASSERT_TRUE(assignDarwinScalarSourceABI(CallHint->Signature,
                                          Fixture.Image.Arch, Error))
      << Error;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.SourceCallHint = std::move(CallHint);
  Call.addInput(MedVar::makeConst(0x1020, 8));
  Call.addInput(Fixture.Med.Params[0]);
  Call.addInput(Fixture.Med.Params[1]);
  // Generic MedIR keeps the incoming SSA registers until source parameters
  // are bound in the second pipeline run. Their IDs need not match Param IDs.
  for (unsigned Index = 1; Index <= 2; ++Index) {
    Call.Inputs[Index].Kind = MedVar::Reg;
    Call.Inputs[Index].Id += 100;
  }
  Fixture.Med.Blocks[0].Ops.insert(Fixture.Med.Blocks[0].Ops.begin(), Call);
}

TEST(NativeSourceHints,
     PointerUsesFollowWholeValuesWithoutChangingMachineTypes) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Shape = 0; Shape < 4; ++Shape) {
      NativeFixture Fixture(Architecture);
      addPointerCall(Fixture);
      if (Shape == 3) {
        auto &Ops = Fixture.Med.Blocks[0].Ops;
        MedOp SelfCopy;
        SelfCopy.Opcode = NdOp::COPY;
        SelfCopy.Output = Ops[0].Inputs[1];
        SelfCopy.addInput(SelfCopy.Output);
        Ops.insert(Ops.begin(), SelfCopy);
      } else if (Shape) {
        MedOp Copy;
        Copy.Opcode = NdOp::COPY;
        Copy.Output.Kind = MedVar::Temp;
        Copy.Output.Id = 30;
        Copy.Output.SSAVer = 1;
        Copy.Output.Size = 8;
        Copy.addInput(Fixture.Med.Params[0]);
        auto &Entry = Fixture.Med.Blocks[0];
        Entry.Ops[0].Inputs[1] = Copy.Output;
        if (Shape == 1) {
          Entry.Ops.insert(Entry.Ops.begin(), Copy);
        } else {
          // A loop PHI retains the exact incoming value through its backedge.
          MedBlock Exit = std::move(Entry);
          Exit.Id = 1;
          Exit.Preds = {2};
          MedBlock Loop;
          Loop.Id = 2;
          Loop.Preds = {0, 2};
          Loop.Succs = {2, 1};
          PhiNode Phi;
          Phi.Output = Copy.Output;
          ++Phi.Output.Id;
          Phi.Args = {{0, Copy.Output}, {2, Phi.Output}};
          Loop.Phis.push_back(Phi);
          Exit.Ops[0].Inputs[1] = Phi.Output;
          Entry = {};
          Entry.Id = 0;
          Entry.Succs = {2};
          Entry.Ops.push_back(Copy);
          // Deliberately keep the consumer before the producer in block order.
          Fixture.Med.Blocks.push_back(std::move(Exit));
          Fixture.Med.Blocks.push_back(std::move(Loop));
        }
      }
      std::string Error;
      auto Hint = Fixture.infer(Error);
      ASSERT_TRUE(Hint) << Error;
      ASSERT_EQ(Hint->Parameters.size(), 2U);
      for (size_t Index = 0; Index < 2; ++Index) {
        EXPECT_EQ(Hint->Parameters[Index].Type->Kind, NdTypeKind::Ptr);
        EXPECT_EQ(Hint->Parameters[Index].Location.ValueBytes, 8U);
        EXPECT_EQ(Hint->Parameters[Index].Location.RegisterOffset,
                  getTargetRegInfo(Architecture).IntParamRegs[Index]);
        EXPECT_EQ(Fixture.Med.TypedParams[Index].Type->Kind, NdTypeKind::Int);
        EXPECT_EQ(Fixture.High.Params[Index].Type->Kind, NdTypeKind::Int);
      }
      EXPECT_EQ(Fixture.Med.ReturnValueEvidence,
                MedReturnValueEvidence::Unknown);
    }
  }
}

TEST(NativeSourceHints, ConflictingOrPartialUsesCannotInventPointerParameters) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
      NativeFixture Fixture(Architecture);
      addPointerCall(Fixture);
      auto &Ops = Fixture.Med.Blocks[0].Ops;
      if (Mutation == 0) {
        Ops[1].Inputs[0] = Fixture.Med.Params[0];
      } else if (Mutation == 1) {
        MedOp Copy;
        Copy.Opcode = NdOp::COPY;
        Copy.Output.Kind = MedVar::Temp;
        Copy.Output.Id = 30;
        Copy.Output.Size = 4;
        Copy.addInput(Fixture.Med.Params[0]);
        Ops.insert(Ops.begin(), Copy);
      } else if (Mutation == 2) {
        auto Hint =
            std::make_shared<SourceCallTypeHint>(*Ops[0].SourceCallHint);
        Hint->Signature.Parameters[0].Type = NdType::makeInt(8);
        auto ScalarCall = Ops[0];
        ScalarCall.SourceCallHint = std::move(Hint);
        Ops.insert(Ops.begin(), ScalarCall);
      } else if (Mutation == 3) {
        // A later register version is not the incoming parameter.
        Ops[0].Inputs[1].Kind = MedVar::Reg;
        ++Ops[0].Inputs[1].SSAVer;
      } else {
        Ops[0].Inputs[1].Size = 4;
      }
      std::string Error;
      auto Hint = Fixture.infer(Error);
      ASSERT_TRUE(Hint) << Error;
      EXPECT_EQ(Hint->Parameters[0].Type->Kind, NdTypeKind::Int) << Mutation;
      EXPECT_EQ(Hint->Parameters[1].Type->Kind, NdTypeKind::Ptr) << Mutation;
    }
  }
}

TEST(NativeSourceHints, AmbiguousValuesAndUnboundCallsDoNotSupplyPointerFacts) {
  for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
    NativeFixture Fixture;
    addPointerCall(Fixture);
    auto &Ops = Fixture.Med.Blocks[0].Ops;
    if (Mutation == 0) {
      Ops[0].SourceCallHint.reset();
    } else if (Mutation == 1) {
      auto Hint = std::make_shared<SourceCallTypeHint>(*Ops[0].SourceCallHint);
      Hint->Signature.Parameters[0].Location.ValueBytes = 4;
      Ops[0].SourceCallHint = std::move(Hint);
    } else if (Mutation == 2) {
      const auto Duplicate = Ops[1];
      Ops.insert(Ops.begin(), Duplicate);
    } else if (Mutation == 3) {
      Ops[0].Inputs[1] = Fixture.Med.Params[0];
      Ops[0].Inputs[1].RegOff = Fixture.Med.Params[1].RegOff;
    } else {
      PhiNode Phi;
      Phi.Output = Ops[0].Inputs[1];
      Phi.Args = {{0, MedVar::makeConst(7, 8)}};
      Fixture.Med.Blocks[0].Phis.push_back(Phi);
    }
    std::string Error;
    auto Hint = Fixture.infer(Error);
    if (Mutation < 2) {
      EXPECT_FALSE(Hint);
    } else {
      ASSERT_TRUE(Hint) << Error;
      EXPECT_EQ(Hint->Parameters[0].Type->Kind, NdTypeKind::Int);
      EXPECT_EQ(Hint->Parameters[1].Type->Kind, NdTypeKind::Int);
    }
  }
}
} // namespace
