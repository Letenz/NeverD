//===- LowIRRefinementTests.cpp - Constructive program relations
//-----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/analysis/LowIRRefinement.h"

using namespace neverd;
using namespace neverd::analysis;

namespace {
using Status = LowIRRefinementStatus;
using Witness = LowIRRefinementWitness;

NdVar r(uint64_t Offset, uint16_t Bytes = 8) {
  return NdVar::reg(Offset, Bytes);
}
NdVar n(uint64_t Value, uint16_t Bytes = 8) {
  return NdVar::scalar(Value, Bytes);
}
LowOp op(NdOp Code, NdVar Output = {},
         std::initializer_list<NdVar> Inputs = {}) {
  LowOp O;
  O.Opcode = Code;
  O.Output = Output;
  for (const auto &Input : Inputs)
    O.addInput(Input);
  return O;
}

struct Program {
  LowFunc Function;
  std::vector<LowIRUndefinedInstruction> Records;
  LowIRIndependenceContract Contract;

  Program() {
    Function.Entry = 0x100;
    Contract.ReturnRegisters = {{0, 8}};
    block(0, 0x100);
  }

  void block(int Id, va_t Address, std::vector<int> Successors = {}) {
    LowBlock B;
    B.Id = Id;
    B.StartAddr = B.EndAddr = Address;
    B.Succs = std::move(Successors);
    Function.Blocks.push_back(std::move(B));
  }

  LowIRUndefinedInstruction &instruction(std::initializer_list<LowOp> Ops) {
    auto &B = Function.Blocks.back();
    LowInstructionBoundary IB;
    IB.Address = B.EndAddr++;
    IB.Size = 1;
    IB.FirstOp = B.Ops.size();
    IB.OpCount = Ops.size();
    for (auto O : Ops) {
      O.Addr = IB.Address;
      O.Seq = B.Ops.size() - IB.FirstOp;
      if (O.Opcode == NdOp::BRANCH || O.Opcode == NdOp::COND_BR) {
        IB.Control = LowInstructionControl::Branch;
        IB.ControlFlags = LowInstructionControlFlag::Branch;
        IB.Immediate = O.Inputs[0].Offset;
        if (O.Opcode == NdOp::COND_BR)
          IB.ControlFlags |= LowInstructionControlFlag::Conditional;
      } else if (O.Opcode == NdOp::RETURN) {
        IB.Control = LowInstructionControl::Return;
        IB.ControlFlags = LowInstructionControlFlag::Return;
      }
      B.Ops.push_back(O);
    }
    B.InstructionBoundaries.push_back(IB);
    LowInstructionUndefinedEffects Effects;
    Effects.Coverage = LowUndefinedCoverage::Complete;
    Effects.OpCount = Ops.size();
    Effects.OperationDigest = lowUndefinedOperationDigest(
        llvm::ArrayRef<LowOp>(B.Ops).slice(IB.FirstOp, IB.OpCount));
    Records.push_back({B.Id, IB, std::move(Effects)});
    return Records.back();
  }

  void finish() { instruction({op(NdOp::RETURN, {}, {r(0)})}); }
  void frame() { Contract.Frame = LowIRIndependenceFrame{{32, 8}, -16, 8}; }
  LowIRRefinementResult check(const Program &Candidate,
                              Witness Choice = Witness::LiftedBits,
                              const LowIRRefinementLimits &Limits = {}) const {
    return checkLowIRRefinement(Function, Records, Candidate.Function, Contract,
                                Choice, Limits);
  }
};

void expect(const Program &Original, const Program &Candidate, Status S,
            Witness W = Witness::LiftedBits,
            const LowIRRefinementLimits &Limits = {}) {
  const auto Result = Original.check(Candidate, W, Limits);
  EXPECT_EQ(Result.Status, S) << Result.Diagnostic;
  EXPECT_EQ(Result.proved(), S == Status::Proved);
  EXPECT_EQ(Result.Certificate.has_value(), S == Status::Proved);
}

TEST(LowIRRefinement, DefinedRewriteAndWrongCandidate) {
  Program A, B;
  A.instruction({op(NdOp::INT_ADD, r(0), {r(8), r(16)})});
  A.finish();
  B.instruction({op(NdOp::INT_ADD, r(0), {r(16), r(8)})});
  B.finish();
  const auto Good = A.check(B);
  ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
  EXPECT_EQ(Good.Certificate->Scope,
            LowIRRefinementScope::CompleteFiniteLowIRPaths);
  EXPECT_EQ(Good.OriginalPaths, 1U);
  EXPECT_EQ(Good.CandidatePaths, 1U);
  B.Function.Blocks[0].Ops[0].Opcode = NdOp::INT_SUB;
  expect(A, B, Status::Different);
}

TEST(LowIRRefinement, ExplicitWitnessDoesNotBecomeIndependence) {
  Program A, B, Zero;
  auto &Effect = A.instruction({op(NdOp::COPY, r(0), {r(8)})});
  Effect.Effects.Effects.push_back({1, r(0), 0, 1, {}});
  A.finish();
  B.instruction({op(NdOp::COPY, r(0), {r(8)})});
  B.finish();
  Zero.instruction({op(NdOp::INT_AND, r(0), {r(8), n(~uint64_t{1})})});
  Zero.finish();
  expect(A, B, Status::Proved);
  expect(A, B, Status::Different, Witness::ZeroBits);
  expect(A, Zero, Status::Proved, Witness::ZeroBits);
  expect(A, Zero, Status::Different);
  const auto Independent =
      checkLowIRUndefinedIndependence(A.Function, A.Records, A.Contract);
  EXPECT_EQ(Independent.Status, LowIRIndependenceStatus::Dependent);
  EXPECT_FALSE(Independent.Certificate);
}

TEST(LowIRRefinement, GuardActivationAndMalformedBoolean) {
  Program A, B;
  auto &Effect =
      A.instruction({op(NdOp::INT_AND, NdVar::tmp(0, 1), {r(16, 1), n(1, 1)}),
                     op(NdOp::COPY, r(0), {r(8)})});
  Effect.Effects.Effects.push_back({2, r(0), 0, 1, NdVar::tmp(0, 1)});
  A.finish();
  B.instruction(
      {op(NdOp::INT_AND, NdVar::tmp(0, 8), {r(16), n(1)}),
       op(NdOp::INT_XOR, NdVar::tmp(8, 8), {NdVar::tmp(0, 8), n(UINT64_MAX)}),
       op(NdOp::INT_AND, r(0), {r(8), NdVar::tmp(8, 8)})});
  B.finish();
  expect(A, B, Status::Proved, Witness::ZeroBits);
  A.Function.Blocks[0].Ops[0] = op(NdOp::COPY, NdVar::tmp(0, 1), {n(2, 1)});
  A.Function.Blocks[0].Ops[0].Addr = A.Records[0].Boundary.Address;
  A.Function.Blocks[0].Ops[0].Seq = 0;
  A.Records[0].Effects.OperationDigest = lowUndefinedOperationDigest(
      llvm::ArrayRef<LowOp>(A.Function.Blocks[0].Ops).take_front(2));
  expect(A, B, Status::Invalid);
}

TEST(LowIRRefinement, CopiesAndSpillsCannotChooseDifferentValues) {
  Program A, B;
  A.frame();
  A.Contract.ReturnRegisters.push_back({16, 8});
  auto &E = A.instruction({op(NdOp::COPY, r(0), {r(8)})});
  E.Effects.Effects.push_back({1, r(0), 0, 64, {}});
  A.instruction(
      {op(NdOp::COPY, r(16), {r(0)}), op(NdOp::STORE, {}, {r(32), r(0)})});
  A.finish();
  B.instruction({op(NdOp::COPY, r(0), {n(0)}), op(NdOp::COPY, r(16), {r(0)}),
                 op(NdOp::STORE, {}, {r(32), r(0)})});
  B.finish();
  expect(A, B, Status::Proved, Witness::ZeroBits);
  B.Function.Blocks[0].Ops[1].Inputs[0] = n(1);
  expect(A, B, Status::Different, Witness::ZeroBits);
  B.Function.Blocks[0].Ops[1].Inputs[0] = n(0);
  B.Function.Blocks[0].Ops[2].Inputs[1] = n(1);
  expect(A, B, Status::Different, Witness::ZeroBits);
}

TEST(LowIRRefinement, ObservesUnionOfWritesAndReturnOperand) {
  Program A, B;
  A.frame();
  A.Contract.ReturnRegisters.clear();
  A.instruction({op(NdOp::COPY, r(0), {n(7)})});
  A.finish();
  B.instruction(
      {op(NdOp::COPY, r(0), {n(7)}), op(NdOp::STORE, {}, {r(32), n(9, 1)})});
  B.finish();
  expect(A, B, Status::Different);
  A.Contract.ObserveWrittenFrameBytes = false;
  expect(A, B, Status::Proved);
  B.Function.Blocks[0].Ops[0].Inputs[0] = n(8);
  expect(A, B, Status::Different);
}

TEST(LowIRRefinement, OrdinaryInputsShareOverlappingByteViews) {
  Program A, B;
  A.instruction({op(NdOp::INT_AND, r(0), {r(8), n(255)})});
  A.finish();
  B.instruction({op(NdOp::INT_ZEXT, r(0), {r(8, 1)})});
  B.finish();
  expect(A, B, Status::Proved);
  B.Function.Blocks[0].Ops[0].Inputs[0] = r(9, 1);
  expect(A, B, Status::Different);
}

Program countdown(bool Decrement = true) {
  Program A;
  A.Function.Blocks[0].Succs = {1};
  A.instruction({op(NdOp::INT_AND, r(8), {r(16), n(3)}),
                 op(NdOp::COPY, r(0), {n(0)}),
                 op(NdOp::BRANCH, {}, {n(0x200)})});
  A.block(1, 0x200, {2, 3});
  A.instruction({op(NdOp::INT_EQUAL, NdVar::tmp(0, 1), {r(8), n(0)}),
                 op(NdOp::COND_BR, {}, {n(0x400), NdVar::tmp(0, 1)})});
  A.block(2, 0x300, {1});
  auto &E = A.instruction({op(NdOp::INT_ADD, r(0), {r(0), n(1)}),
                           op(NdOp::COPY, r(24, 1), {n(1, 1)})});
  E.Effects.Effects.push_back({2, r(24, 1), 0, 1, {}});
  A.instruction({op(NdOp::INT_SUB, r(8), {r(8), n(Decrement ? 1 : 0)}),
                 op(NdOp::BRANCH, {}, {n(0x200)})});
  A.block(3, 0x400);
  A.finish();
  return A;
}

TEST(LowIRRefinement, DifferentLoopStructureAndDistinctProducerVisits) {
  auto A = countdown();
  Program B;
  B.instruction({op(NdOp::INT_AND, r(0), {r(16), n(3)})});
  B.finish();
  const auto Good = A.check(B);
  ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
  EXPECT_EQ(Good.OriginalPaths, 4U);
  EXPECT_EQ(Good.CandidatePaths, 1U);
  ASSERT_GT(Good.Certificate->Producers.size(), 1U);
  const auto &P = Good.Certificate->Producers;
  for (size_t I = 1; I != P.size(); ++I) {
    EXPECT_NE(P[I - 1].InstructionVisit, P[I].InstructionVisit);
    EXPECT_EQ(P[I - 1].InstructionAddress, P[I].InstructionAddress);
  }
  expect(B, A, Status::Proved);
  B.Function.Blocks[0].Ops[0].Inputs[1] = n(2);
  expect(A, B, Status::Different);
}

TEST(LowIRRefinement, FinishingSiblingCannotHideAnInfiniteCandidatePath) {
  auto A = countdown();
  auto B = countdown(false);
  // Schedule the zero-iteration return before exploring the infinite sibling.
  B.Function.Blocks[1].Ops[0].Opcode = NdOp::INT_NOTEQUAL;
  B.Function.Blocks[1].Ops[1].Inputs[0] = n(0x300);
  B.Function.Blocks[1].InstructionBoundaries[0].Immediate = 0x300;
  LowIRRefinementLimits Limits;
  Limits.Execution.MaxPaths = 80;
  expect(A, B, Status::BudgetExceeded, Witness::LiftedBits, Limits);
  const auto Partial = A.check(B, Witness::LiftedBits, Limits);
  EXPECT_EQ(Partial.OriginalPaths, 4U);
  EXPECT_EQ(Partial.CandidatePaths, 1U);
  const auto InfiniteOriginal = countdown(false);
  expect(InfiniteOriginal, A, Status::BudgetExceeded, Witness::LiftedBits,
         Limits);
}

TEST(LowIRRefinement, PreservationStillAppliesToBothPrograms) {
  Program A, B;
  A.Contract.PreservedRegisters = {{8, 8}};
  A.instruction({op(NdOp::COPY, r(0), {n(1)})});
  A.finish();
  B.instruction({op(NdOp::COPY, r(8), {n(0)}), op(NdOp::COPY, r(0), {n(1)})});
  B.finish();
  expect(A, B, Status::ContractViolation);
  expect(B, A, Status::Proved);
}

TEST(LowIRRefinement, MissingStaleAndMalformedEvidenceRefuses) {
  Program A, B;
  A.finish();
  B.finish();
  A.Records[0].Effects.OperationDigest = "stale";
  expect(A, B, Status::Invalid);
  A.Records.clear();
  expect(A, B, Status::Unsupported);
  auto C = B;
  C.Function.Blocks[0].InstructionBoundaries[0].OpCount = UINT64_MAX;
  expect(B, C, Status::Invalid);
  C = B;
  C.Function.Blocks[0].Ops[0].NumInputs = 255;
  expect(B, C, Status::Invalid);
  expect(B, B, Status::Invalid, static_cast<Witness>(255));
}

TEST(LowIRRefinement, LiftedWitnessCannotReadAnUnboundTemporary) {
  Program A, B;
  auto &E =
      A.instruction({op(NdOp::NOP), op(NdOp::COPY, r(0), {NdVar::tmp(0, 8)})});
  E.Effects.Effects.push_back({1, NdVar::tmp(0, 8), 0, 64, {}});
  A.finish();
  B.instruction({op(NdOp::COPY, r(0), {n(0)})});
  B.finish();
  expect(A, B, Status::Invalid);
  expect(A, B, Status::Proved, Witness::ZeroBits);
  expect(B, A, Status::Invalid);
}

TEST(LowIRRefinement, BudgetsCoverBothExecutionsAndFinalRelation) {
  Program A;
  A.instruction({op(NdOp::COPY, r(0), {n(2)})});
  A.finish();
  const auto Good = A.check(A);
  ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
  for (unsigned Kind = 0; Kind != 5; ++Kind) {
    LowIRRefinementLimits L;
    if (Kind == 0)
      L.Execution.MaxOperations = Good.Operations - 1;
    if (Kind == 1)
      L.Execution.MaxInstructions = Good.Instructions - 1;
    if (Kind == 2)
      L.Execution.MaxObservations = Good.Observations - 1;
    if (Kind == 3)
      L.Execution.MaxSolverQueries = Good.SolverQueries - 1;
    if (Kind == 4)
      L.MaxTerminalPairs = 0;
    expect(A, A, Status::BudgetExceeded, Witness::LiftedBits, L);
  }
  LowIRRefinementLimits L;
  ++L.MaxTerminalPairs;
  const auto Changed = A.check(A, Witness::LiftedBits, L);
  ASSERT_TRUE(Changed.proved());
  EXPECT_NE(Good.Certificate->InputDigest, Changed.Certificate->InputDigest);
  const auto Zero = A.check(A, Witness::ZeroBits);
  ASSERT_TRUE(Zero.proved());
  EXPECT_NE(Good.Certificate->InputDigest, Zero.Certificate->InputDigest);
}

TEST(LowIRRefinement, MemoryScratchCannotOverwriteInputValues) {
  for (uint64_t Offset : {UINT64_MAX - 7, UINT64_MAX - 10}) {
    Program A, B;
    A.frame();
    const auto Scratch = NdVar::tmp(Offset, 8);
    auto &E = A.instruction({op(NdOp::COPY, Scratch, {n(1)}),
                             op(NdOp::STORE, {}, {r(32), Scratch}),
                             op(NdOp::COPY, r(0), {n(0)})});
    E.Effects.Effects.push_back({1, Scratch, 0, 64, {}});
    A.finish();
    B = A;
    B.Function.Blocks[0].Ops[0].Inputs[0] = n(2);
    expect(A, B, Status::Invalid);
    const auto Strict =
        checkLowIRUndefinedIndependence(A.Function, A.Records, A.Contract);
    EXPECT_EQ(Strict.Status, LowIRIndependenceStatus::Invalid);
    EXPECT_FALSE(Strict.Certificate);
  }
}

TEST(LowIRRefinement, CandidateReturnArityAndEntryDomainAreChecked) {
  Program A, B;
  A.frame();
  A.finish();
  B.instruction({op(NdOp::RETURN)});
  expect(A, B, Status::Different);
  A.Contract.EntryConstants.push_back({r(32), 0});
  expect(A, B, Status::InfeasibleEntry);
}
} // namespace
