//===- LowIRReadTraceTests.cpp - Finite ordinary read relations
//------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent read traces and byte-level overlap with a private frame.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/analysis/LowIRRefinement.h"

#include <initializer_list>
#include <utility>
#include <vector>

using namespace neverd;
using namespace neverd::analysis;

namespace {
NdVar reg(uint64_t Offset, uint16_t Bytes = 8) {
  return NdVar::reg(Offset, Bytes);
}
NdVar constant(uint64_t Value, uint16_t Bytes = 8) {
  return NdVar::scalar(Value, Bytes);
}
LowOp operation(NdOp Code, NdVar Output, std::initializer_list<NdVar> Inputs) {
  LowOp Op;
  Op.Opcode = Code;
  Op.Output = Output;
  for (const auto &Input : Inputs)
    Op.addInput(Input);
  return Op;
}

struct ReadProgram {
  LowFunc Function;
  std::vector<LowIRUndefinedInstruction> Records;
  LowIRIndependenceContract Contract;

  ReadProgram() {
    Function.Entry = 0x100;
    LowBlock Block;
    Block.Id = 0;
    Block.StartAddr = Block.EndAddr = Function.Entry;
    Function.Blocks.push_back(std::move(Block));
    Contract.Frame = LowIRIndependenceFrame{{32, 8}, -16, 8};
    Contract.ReturnRegisters = {{0, 8}};
    Contract.ObserveWrittenFrameBytes = false;
    Contract.ExternalReads = LowIRExternalReadModel::OrderedOrdinaryRAMV1;
  }

  void append(LowOp Op) {
    auto &Block = Function.Blocks.back();
    LowInstructionBoundary Boundary;
    Boundary.Address = Block.EndAddr++;
    Boundary.Size = 1;
    Boundary.FirstOp = Block.Ops.size();
    Boundary.OpCount = 1;
    if (Op.Opcode == NdOp::RETURN) {
      Boundary.Control = LowInstructionControl::Return;
      Boundary.ControlFlags = LowInstructionControlFlag::Return;
    } else if (Op.Opcode == NdOp::COND_BR) {
      Boundary.Control = LowInstructionControl::Branch;
      Boundary.ControlFlags = LowInstructionControlFlag::Branch |
                              LowInstructionControlFlag::Conditional;
      Boundary.Immediate = Op.Inputs[0].Offset;
    }
    Op.Addr = Boundary.Address;
    Op.Seq = 0;
    Block.Ops.push_back(Op);
    Block.InstructionBoundaries.push_back(Boundary);
    LowInstructionUndefinedEffects Effects;
    Effects.Coverage = LowUndefinedCoverage::Complete;
    Effects.OpCount = 1;
    Effects.OperationDigest =
        lowUndefinedOperationDigest({&Block.Ops.back(), 1});
    Records.push_back({Block.Id, Boundary, std::move(Effects)});
  }
  void load(uint64_t AddressRegister = 8, uint16_t Bytes = 8,
            uint64_t OutputRegister = 0) {
    append(operation(NdOp::LOAD, reg(OutputRegister, Bytes),
                     {reg(AddressRegister)}));
  }
  void store(uint64_t Value, uint16_t Bytes = 1) {
    append(operation(NdOp::INT_SUB, reg(48), {reg(32), constant(8)}));
    append(operation(NdOp::STORE, {}, {reg(48), constant(Value, Bytes)}));
  }
  void finish() { append(operation(NdOp::RETURN, {}, {reg(0)})); }
  void block(int Id, va_t Address) {
    LowBlock Block;
    Block.Id = Id;
    Block.StartAddr = Block.EndAddr = Address;
    Function.Blocks.push_back(std::move(Block));
  }
  LowIRRefinementResult
  compare(const ReadProgram &Other,
          const LowIRRefinementLimits &Limits = {}) const {
    return checkLowIRRefinement(Function, Records, Other.Function, Contract,
                                LowIRRefinementWitness::LiftedBits, Limits);
  }
};

TEST(LowIRReadTrace, SharesDefinedInputsWithoutObservingAnAddressValue) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big})
    for (uint16_t Bytes : {1, 2, 4, 8}) {
      SCOPED_TRACE(Bytes);
      ReadProgram Original;
      Original.Contract.ByteOrder = Order;
      Original.load(8, Bytes);
      Original.finish();
      const auto Result = Original.compare(Original);
      EXPECT_TRUE(Result.proved()) << Result.Diagnostic;
      EXPECT_TRUE(Result.Certificate.has_value());
      Original.Contract.ExternalReads = LowIRExternalReadModel::None;
      EXPECT_EQ(Original.compare(Original).Status,
                LowIRRefinementStatus::Unsupported);
    }
}

TEST(LowIRReadTrace, AddressWidthOrderAndCountRemainObservations) {
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    SCOPED_TRACE(Mutation);
    ReadProgram Original, Candidate;
    Original.load(8, 1, 16);
    Original.load(24, 1, 16);
    if (Mutation == 0) {
      Candidate.load(40, 1, 16);
      Candidate.load(24, 1, 16);
    } else if (Mutation == 1) {
      Candidate.load(8, 2, 16);
      Candidate.load(24, 1, 16);
    } else if (Mutation == 2) {
      Candidate.load(24, 1, 16);
      Candidate.load(8, 1, 16);
    } else {
      Candidate.load(8, 1, 16);
    }
    Original.finish();
    Candidate.finish();
    const auto Result = Original.compare(Candidate);
    EXPECT_EQ(Result.Status, LowIRRefinementStatus::Different)
        << Result.Diagnostic;
    EXPECT_FALSE(Result.Certificate);
  }
}

TEST(LowIRReadTrace, EveryFeasiblePathRetainsItsOrderedReads) {
  const auto Make = [](unsigned Mutation) {
    ReadProgram P;
    P.load(8, 1, 16);
    P.append(
        operation(NdOp::INT_EQUAL, reg(56, 1), {reg(16, 1), constant(0, 1)}));
    P.Function.Blocks.front().Succs = {1, 2};
    P.append(operation(NdOp::COND_BR, {}, {constant(0x200), reg(56, 1)}));
    P.block(1, 0x200);
    P.load(24, 8, 16);
    P.append(operation(NdOp::COPY, reg(0), {constant(7)}));
    P.finish();
    P.block(2, 0x300);
    if (Mutation != 2)
      P.load(Mutation == 1 ? 40 : 24, 8, 16);
    P.append(operation(NdOp::COPY, reg(0), {constant(7)}));
    P.finish();
    return P;
  };
  const auto Original = Make(0);
  auto Reordered = Make(0);
  std::swap(Reordered.Function.Blocks[1], Reordered.Function.Blocks[2]);
  const auto Same = Original.compare(Reordered);
  EXPECT_TRUE(Same.proved()) << Same.Diagnostic;
  for (unsigned Mutation : {1, 2}) {
    const auto Different = Original.compare(Make(Mutation));
    EXPECT_EQ(Different.Status, LowIRRefinementStatus::Different)
        << Different.Diagnostic;
    EXPECT_FALSE(Different.Certificate);
  }
}

TEST(LowIRReadTrace, ReadsCanOverlapEarlierFrameWrites) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    ReadProgram Original, Changed;
    Original.Contract.ByteOrder = Order;
    Original.store(0x11223344, 4);
    Original.load();
    Original.finish();
    Changed.store(0x11225544, 4);
    Changed.load();
    Changed.finish();
    const auto Same = Original.compare(Original);
    EXPECT_TRUE(Same.proved()) << Same.Diagnostic;
    const auto Different = Original.compare(Changed);
    EXPECT_EQ(Different.Status, LowIRRefinementStatus::Different)
        << Different.Diagnostic;
    EXPECT_FALSE(Different.Certificate);
  }
}

TEST(LowIRReadTrace, RepeatedReadsAreConservativeIndependentOccurrences) {
  ReadProgram Original, Candidate;
  for (auto *Program : {&Original, &Candidate}) {
    Program->load(8, 8, 0);
    Program->load(8, 8, 16);
  }
  Original.append(operation(NdOp::INT_SUB, reg(0), {reg(0), reg(16)}));
  Candidate.append(operation(NdOp::COPY, reg(0), {constant(0)}));
  Original.finish();
  Candidate.finish();
  EXPECT_TRUE(Original.compare(Original).proved());
  EXPECT_EQ(Original.compare(Candidate).Status,
            LowIRRefinementStatus::Different);
}

TEST(LowIRReadTrace, RequiresFrameAndNeverAuthorizesExternalStores) {
  ReadProgram Read;
  Read.load();
  Read.finish();
  Read.Contract.Frame.reset();
  EXPECT_EQ(Read.compare(Read).Status, LowIRRefinementStatus::Invalid);
  ReadProgram Store;
  Store.append(operation(NdOp::STORE, {}, {reg(8), constant(0)}));
  Store.finish();
  EXPECT_EQ(Store.compare(Store).Status, LowIRRefinementStatus::Unsupported);
  Store.Contract.ExternalReads = static_cast<LowIRExternalReadModel>(255);
  EXPECT_EQ(Store.compare(Store).Status, LowIRRefinementStatus::Invalid);
}

TEST(LowIRReadTrace, PartialFrameOverlapRespectsByteOrder) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    ReadProgram Original, Candidate;
    Original.Contract.ByteOrder = Order;
    Original.Contract.Frame->Begin = -8;
    const uint64_t Shift = Order == llvm::endianness::little ? 8 : 24;
    const uint64_t Mask = 0xffffffffULL << Shift;
    for (auto *Program : {&Original, &Candidate}) {
      Program->store(0x11223344, 4);
      // The first byte precedes the certified frame, so this access uses
      // the external read model and overlaps four previously written bytes.
      Program->append(operation(NdOp::INT_SUB, reg(8), {reg(32), constant(9)}));
      Program->load();
    }
    Original.append(operation(NdOp::INT_AND, reg(0), {reg(0), constant(Mask)}));
    Candidate.append(
        operation(NdOp::COPY, reg(0), {constant(0x11223344ULL << Shift)}));
    Original.finish();
    Candidate.finish();
    const auto Result = Original.compare(Candidate);
    EXPECT_TRUE(Result.proved()) << Result.Diagnostic;
  }
}

TEST(LowIRReadTrace,
     UndefinedAddressesAndOverlappingStoredValuesStayObservable) {
  for (bool Address : {false, true}) {
    ReadProgram Original;
    const auto Arbitrary = reg(Address ? 8 : 16);
    Original.append(operation(NdOp::COPY, Arbitrary, {constant(0)}));
    Original.Records.back().Effects.Effects.push_back(
        {1, Arbitrary, 0, 64, {}});
    if (!Address) {
      Original.append(
          operation(NdOp::INT_SUB, reg(48), {reg(32), constant(8)}));
      Original.append(operation(NdOp::STORE, {}, {reg(48), Arbitrary}));
    }
    Original.load();
    Original.finish();
    const auto Result = checkLowIRUndefinedIndependence(
        Original.Function, Original.Records, Original.Contract);
    EXPECT_EQ(Result.Status, LowIRIndependenceStatus::Dependent)
        << Result.Diagnostic;
    EXPECT_FALSE(Result.Certificate);
  }
}

TEST(LowIRReadTrace, ObservationsAndContractIdentityAreBounded) {
  ReadProgram Original;
  Original.store(7);
  Original.load();
  Original.finish();
  const auto Result = Original.compare(Original);
  ASSERT_TRUE(Result.proved()) << Result.Diagnostic;
  ASSERT_GT(Result.Observations, 0u);
  LowIRRefinementLimits Limits;
  Limits.Execution.MaxObservations = Result.Observations;
  EXPECT_TRUE(Original.compare(Original, Limits).proved());
  --Limits.Execution.MaxObservations;
  const auto Short = Original.compare(Original, Limits);
  EXPECT_EQ(Short.Status, LowIRRefinementStatus::BudgetExceeded);
  EXPECT_FALSE(Short.Certificate);
  ReadProgram NoReads;
  NoReads.finish();
  const auto Traced = NoReads.compare(NoReads);
  ASSERT_TRUE(Traced.proved());
  NoReads.Contract.ExternalReads = LowIRExternalReadModel::None;
  const auto Default = NoReads.compare(NoReads);
  ASSERT_TRUE(Default.proved());
  EXPECT_NE(Traced.Certificate->InputDigest, Default.Certificate->InputDigest);
}

TEST(LowIRReadTrace, LoopProofAndInferenceRefuseTheFiniteModel) {
  ReadProgram Original;
  Original.finish();
  const auto Inference =
      inferLowIRLoopRefinementPlan(Original.Function, Original.Contract);
  EXPECT_EQ(Inference.Status, LowIRLoopInferenceStatus::Unsupported);
  const auto Proof = checkLowIRLoopRefinement(
      Original.Function, Original.Records, Original.Function, Original.Contract,
      LowIRLoopRefinementPlan{});
  EXPECT_EQ(Proof.Status, LowIRRefinementStatus::Unsupported);
  EXPECT_FALSE(Proof.Certificate);
}
} // namespace
