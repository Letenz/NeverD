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

Program wordLoop(unsigned Step = 1, bool Alternate = false) {
  Program P;
  P.Function.Blocks[0].Succs = {1};
  P.instruction({op(NdOp::COPY, r(0), {n(0)}), op(NdOp::COPY, r(8), {r(16)}),
                 op(NdOp::BRANCH, {}, {n(0x200)})});
  P.block(1, 0x200, {2, 3});
  P.instruction({op(NdOp::INT_EQUAL, NdVar::tmp(0, 1), {r(8), n(0)}),
                 op(NdOp::COND_BR, {}, {n(0x400), NdVar::tmp(0, 1)})});
  P.block(2, 0x300, {1});
  if (Alternate)
    P.instruction({op(NdOp::INT_NEG2, NdVar::tmp(0, 8), {r(8)}),
                   op(NdOp::INT_SUB, r(0), {r(0), NdVar::tmp(0, 8)})});
  else
    P.instruction({op(NdOp::INT_ADD, r(0), {r(0), r(8)})});
  P.instruction({op(NdOp::INT_SUB, r(8), {r(8), n(Step)}),
                 op(NdOp::BRANCH, {}, {n(0x200)})});
  P.block(3, 0x400);
  P.finish();
  return P;
}

LowIRLoopLocation regLocation(uint64_t Offset, uint16_t Bytes = 8) {
  return {LowIRLoopSpace::Register, Offset, Bytes};
}

LowIRLoopRefinementPlan wordLoopPlan() {
  LowIRLoopCutpoint C;
  C.OriginalAddress = C.CandidateAddress = 0x200;
  C.Inputs = {{LowIRLoopSide::Original, regLocation(0), NdVar::tmp(0, 8)},
              {LowIRLoopSide::Original, regLocation(8), NdVar::tmp(8, 8)}};
  C.OriginalState = C.CandidateState = {{regLocation(0), NdVar::tmp(0, 8)},
                                        {regLocation(8), NdVar::tmp(8, 8)}};
  C.Rank = {NdVar::tmp(8, 8)};
  return {{std::move(C)}};
}

LowIRRefinementResult loopCheck(const Program &A, const Program &B,
                                const LowIRLoopRefinementPlan &Plan,
                                const LowIRRefinementLimits &Limits = {}) {
  return checkLowIRLoopRefinement(A.Function, A.Records, B.Function, A.Contract,
                                  Plan, Witness::LiftedBits, Limits);
}

void loopRefused(const LowIRRefinementResult &R, Status S) {
  EXPECT_EQ(R.Status, S) << R.Diagnostic;
  EXPECT_FALSE(R.proved());
  EXPECT_FALSE(R.Certificate);
}

TEST(LowIRLoopRefinement, ArbitraryWordCountWithDifferentBody) {
  const auto A = wordLoop(), B = wordLoop(1, true);
  const auto Plan = wordLoopPlan();
  const auto R = loopCheck(A, B, Plan);
  ASSERT_TRUE(R.proved()) << R.Diagnostic;
  EXPECT_EQ(R.Certificate->Scope, LowIRRefinementScope::InductiveLowIRLoops);
  ASSERT_TRUE(R.Certificate->LoopPlan);
  EXPECT_EQ(R.LoopInitiations, 1U);
  EXPECT_EQ(R.LoopTransitions, 1U);
  EXPECT_EQ(R.RankingChecks, 1U);
  EXPECT_EQ(R.OriginalPaths, 1U);
  EXPECT_EQ(R.CandidatePaths, 1U);
  EXPECT_EQ(R.OriginalCutpoints, 2U);
  EXPECT_LT(R.Instructions, 20U);
  const auto Finite = A.check(B);
  EXPECT_FALSE(Finite.proved());
  EXPECT_EQ(Finite.Status, Status::BudgetExceeded);
}

TEST(LowIRLoopInference, CountAndAccumulatorNeedNoHandwrittenTemplate) {
  const auto A = wordLoop(), B = wordLoop(1, true);
  const auto Inferred = inferLowIRLoopRefinementPlan(A.Function, A.Contract);
  ASSERT_TRUE(Inferred.inferred()) << Inferred.Diagnostic;
  EXPECT_GT(Inferred.Operations, 0U);
  EXPECT_GT(Inferred.SolverQueries, 0U);
  EXPECT_GT(Inferred.RankCandidates, 0U);
  const auto R = loopCheck(A, B, *Inferred.Plan);
  ASSERT_TRUE(R.proved()) << R.Diagnostic;
  EXPECT_EQ(R.Certificate->Scope, LowIRRefinementScope::InductiveLowIRLoops);
  loopRefused(loopCheck(A, wordLoop(2), *Inferred.Plan), Status::Different);
}

TEST(LowIRLoopInference, AscendingCountersSpillsAndEarlyReturns) {
  for (uint16_t Bytes : {4, 8}) {
    for (bool EarlyReturn : {false, true}) {
      Program P;
      P.frame();
      P.Function.Blocks[0].Succs = {1};
      P.instruction({op(NdOp::COPY, r(0), {n(0)}), op(NdOp::COPY, r(8), {n(0)}),
                     op(NdOp::INT_ZEXT, r(24), {r(16, Bytes)}),
                     op(NdOp::INT_ADD, r(40), {r(32), n(-8)}),
                     op(NdOp::STORE, {}, {r(40), r(8)}),
                     op(NdOp::BRANCH, {}, {n(0x200)})});
      P.block(1, 0x200, {2, 4});
      const auto Condition = NdVar::tmp(0, 1);
      P.instruction({op(NdOp::INT_LESS, Condition, {r(8), r(24)}),
                     op(NdOp::COND_BR, {}, {n(0x300), Condition})});
      P.block(2, 0x300, {3});
      P.instruction(
          {op(NdOp::INT_ADD, r(0), {r(0), r(8)}),
           op(NdOp::INT_ADD, NdVar::tmp(8, Bytes), {r(8, Bytes), n(1, Bytes)}),
           op(NdOp::INT_ZEXT, r(8), {NdVar::tmp(8, Bytes)}),
           op(NdOp::STORE, {}, {r(40), r(8)}),
           op(NdOp::BRANCH, {}, {n(0x400)})});
      P.block(3, 0x400,
              EarlyReturn ? std::vector<int>{4, 1} : std::vector<int>{1});
      if (EarlyReturn)
        P.instruction({op(NdOp::INT_EQUAL, Condition, {r(0), r(48)}),
                       op(NdOp::COND_BR, {}, {n(0x500), Condition})});
      else
        P.instruction({op(NdOp::BRANCH, {}, {n(0x200)})});
      P.block(4, 0x500);
      P.finish();
      const auto R = inferLowIRLoopRefinementPlan(P.Function, P.Contract);
      ASSERT_TRUE(R.inferred()) << "bytes=" << Bytes << " early=" << EarlyReturn
                                << ": " << R.Diagnostic;
      const auto Proof = loopCheck(P, P, *R.Plan);
      ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
    }
  }
}

TEST(LowIRLoopInference, InfiniteAndWrappingTransitionsHaveNoPlan) {
  for (unsigned Step : {0, 2}) {
    const auto A = wordLoop(Step);
    const auto R = inferLowIRLoopRefinementPlan(A.Function, A.Contract);
    EXPECT_FALSE(R.inferred()) << R.Diagnostic;
    EXPECT_FALSE(R.Plan);
  }
}

TEST(LowIRLoopInference, SearchBudgetDoesNotExpandTheProofBudget) {
  const auto A = wordLoop();
  const auto Good = inferLowIRLoopRefinementPlan(A.Function, A.Contract);
  ASSERT_TRUE(Good.inferred()) << Good.Diagnostic;
  for (unsigned Kind = 0; Kind != 8; ++Kind) {
    LowIRLoopInferenceLimits Limits;
    if (Kind == 0)
      Limits.MaxCutpointAttempts = 0;
    if (Kind == 1)
      Limits.MaxWideningRounds = 0;
    if (Kind == 2)
      Limits.Execution.MaxOperations = Good.Operations - 1;
    if (Kind == 3)
      Limits.Execution.MaxSolverQueries = Good.SolverQueries - 1;
    if (Kind == 4)
      Limits.MaxRankCandidates = 0;
    if (Kind == 5)
      Limits.Execution.MaxPaths = 0;
    if (Kind == 6)
      Limits.Execution.MaxSymbolicNodes = 0;
    if (Kind == 7)
      Limits.Execution.MaxObservations = 0;
    const auto R = inferLowIRLoopRefinementPlan(A.Function, A.Contract, Limits);
    EXPECT_EQ(R.Status, LowIRLoopInferenceStatus::BudgetExceeded)
        << R.Diagnostic;
    EXPECT_FALSE(R.Plan);
  }
  LowIRRefinementLimits ProofLimits;
  ProofLimits.Execution.MaxSolverQueries = 0;
  loopRefused(loopCheck(A, A, *Good.Plan, ProofLimits), Status::BudgetExceeded);
}

TEST(LowIRLoopInference, MalformedGraphsAndAbsentCutsHaveNoPlan) {
  const auto Good = wordLoop();
  for (unsigned Kind = 0; Kind != 6; ++Kind) {
    auto P = Good;
    if (Kind == 0)
      P.Function.Entry = 0x999;
    if (Kind == 1)
      P.Function.Blocks[0].Succs.push_back(99);
    if (Kind == 2)
      P.Function.Blocks[1].Id = P.Function.Blocks[0].Id;
    if (Kind == 3)
      P.Function.Blocks[0].InstructionBoundaries[0].FirstOp = SIZE_MAX;
    if (Kind == 4)
      P.Function.Blocks[0].Ops[0].NumInputs = 7;
    if (Kind == 5)
      P.Function.Blocks.clear();
    const auto R = inferLowIRLoopRefinementPlan(P.Function, P.Contract);
    EXPECT_EQ(R.Status, LowIRLoopInferenceStatus::Invalid)
        << "kind=" << Kind << ": " << R.Diagnostic;
    EXPECT_FALSE(R.Plan);
  }
  const va_t Absent = 0x999;
  const auto R =
      inferLowIRLoopRefinementPlan(Good.Function, Good.Contract, {}, {Absent});
  EXPECT_EQ(R.Status, LowIRLoopInferenceStatus::Unsupported);
  EXPECT_FALSE(R.Plan);
  EXPECT_EQ(R.CutpointAttempts, 0U);
}

TEST(LowIRLoopRefinement, PrefixInputRequiresCapturedReachablePrefix) {
  const auto A = wordLoop();
  auto Plan = wordLoopPlan();
  Plan.Cutpoints[0].Inputs.push_back(
      {LowIRLoopSide::OriginalPrefix, regLocation(0), NdVar::tmp(16, 8)});
  loopRefused(loopCheck(A, A, Plan), Status::Invalid);
  Plan.Cutpoints[0].UseEntryPrefix = true;
  ASSERT_TRUE(loopCheck(A, A, Plan).proved());
  Plan.Cutpoints[0].Inputs.back().Side = LowIRLoopSide::CandidatePrefix;
  ASSERT_TRUE(loopCheck(A, A, Plan).proved());
}

TEST(LowIRLoopRefinement, WrongBodyAndInitialStateRefuse) {
  const auto A = wordLoop();
  loopRefused(loopCheck(A, wordLoop(2), wordLoopPlan()), Status::Different);
  auto Plan = wordLoopPlan();
  Plan.Cutpoints[0].CandidateState[0].Value = n(7);
  loopRefused(loopCheck(A, A, Plan), Status::Different);
}

TEST(LowIRLoopRefinement, InfiniteLoopAndUnsignedWraparoundRefuse) {
  for (unsigned Step : {0U, 2U}) {
    const auto A = wordLoop(Step);
    const auto R = loopCheck(A, A, wordLoopPlan());
    loopRefused(R, Status::Different);
    EXPECT_NE(R.Diagnostic.find("rank decrease"), std::string::npos);
  }
  auto Plan = wordLoopPlan();
  Plan.Cutpoints[0].Rank = {n(0)};
  const auto A = wordLoop();
  loopRefused(loopCheck(A, A, Plan), Status::Different);
}

TEST(LowIRLoopRefinement, PredicateCannotNarrowEntryDomain) {
  const auto A = wordLoop();
  auto Plan = wordLoopPlan();
  auto &C = Plan.Cutpoints[0];
  C.Expressions.push_back(
      op(NdOp::INT_EQUAL, NdVar::tmp(16, 1), {NdVar::tmp(8, 8), n(0)}));
  C.Predicate = NdVar::tmp(16, 1);
  const auto R = loopCheck(A, A, Plan);
  loopRefused(R, Status::Different);
  EXPECT_NE(R.Diagnostic.find("invariant predicate"), std::string::npos);
  C.Predicate = n(0, 1);
  loopRefused(loopCheck(A, A, Plan), Status::Different);
  C.Predicate = n(2, 1);
  loopRefused(loopCheck(A, A, Plan), Status::Invalid);
}

TEST(LowIRLoopRefinement, MissingCutDoesNotHideCompletedSibling) {
  auto A = wordLoop();
  // Visit the zero-count exit before exploring the unbounded sibling.
  auto &Header = A.Function.Blocks[1];
  Header.Ops[0].Opcode = NdOp::INT_NOTEQUAL;
  Header.Ops[1].Inputs[0] = n(0x300);
  Header.InstructionBoundaries[0].Immediate = 0x300;
  A.Records[1].Boundary = Header.InstructionBoundaries[0];
  A.Records[1].Effects.OperationDigest =
      lowUndefinedOperationDigest(Header.Ops);
  auto Plan = wordLoopPlan();
  // A real block, but no cycle passes through this proposed cutpoint.
  Plan.Cutpoints[0].OriginalAddress = Plan.Cutpoints[0].CandidateAddress =
      0x400;
  LowIRRefinementLimits Limits;
  Limits.Execution.MaxPaths = 40;
  const auto R = loopCheck(A, A, Plan, Limits);
  loopRefused(R, Status::BudgetExceeded);
  EXPECT_GT(R.OriginalCutpoints, 0U);
  Plan.Cutpoints[0].CandidateAddress = 0x300;
  Plan.Cutpoints[0].OriginalAddress = 0x200;
  loopRefused(loopCheck(A, A, Plan), Status::Different);
}

TEST(LowIRLoopRefinement, PrefixWritesStayObservableAfterCut) {
  auto A = wordLoop(), B = wordLoop();
  A.frame();
  auto AddStore = [](Program &P, uint64_t Value) {
    auto &Block = P.Function.Blocks[0];
    auto Store = op(NdOp::STORE, {}, {r(32), n(Value)});
    Store.Addr = Block.Ops[0].Addr;
    Block.Ops.insert(Block.Ops.begin(), Store);
    for (size_t I = 0; I != Block.Ops.size(); ++I)
      Block.Ops[I].Seq = I;
    ++Block.InstructionBoundaries[0].OpCount;
    P.Records[0].Boundary = Block.InstructionBoundaries[0];
    P.Records[0].Effects.OpCount = Block.Ops.size();
    P.Records[0].Effects.OperationDigest =
        lowUndefinedOperationDigest(Block.Ops);
  };
  AddStore(A, 7);
  AddStore(B, 8);
  auto Plan = wordLoopPlan();
  const LowIRLoopLocation Slot{LowIRLoopSpace::Frame, 0, 8};
  Plan.Cutpoints[0].OriginalState.push_back({Slot, n(7)});
  Plan.Cutpoints[0].CandidateState.push_back({Slot, n(8)});
  const auto R = loopCheck(A, B, Plan);
  loopRefused(R, Status::Different);
  EXPECT_NE(R.Diagnostic.find("written frame byte"), std::string::npos);
  A.Contract.ObserveWrittenFrameBytes = false;
  const auto Unobserved = loopCheck(A, B, Plan);
  EXPECT_TRUE(Unobserved.proved()) << Unobserved.Diagnostic;
  A.Contract.PreservedFrameRanges = {{0, 8}};
  loopRefused(loopCheck(A, B, Plan), Status::ContractViolation);
}

TEST(LowIRLoopRefinement, MalformedBindingsAndPlansRefuse) {
  const auto A = wordLoop();
  auto Plan = wordLoopPlan();
  Plan.Cutpoints.push_back(Plan.Cutpoints.front());
  loopRefused(loopCheck(A, A, Plan), Status::Invalid);
  Plan = wordLoopPlan();
  Plan.Cutpoints[0].OriginalAddress++;
  loopRefused(loopCheck(A, A, Plan), Status::Invalid);
  Plan = wordLoopPlan();
  Plan.Cutpoints[0].Inputs[1].Temporary = NdVar::tmp(4, 8);
  loopRefused(loopCheck(A, A, Plan), Status::Invalid);
  Plan = wordLoopPlan();
  Plan.Cutpoints[0].Rank = {NdVar::tmp(31, 8)};
  loopRefused(loopCheck(A, A, Plan), Status::Invalid);
  Plan = wordLoopPlan();
  Plan.Cutpoints[0].Expressions.push_back(
      op(NdOp::LOAD, NdVar::tmp(16, 8), {n(0)}));
  loopRefused(loopCheck(A, A, Plan), Status::Invalid);
  Plan.Cutpoints[0].Expressions[0].NumInputs = 255;
  loopRefused(loopCheck(A, A, Plan), Status::Invalid);
  Plan = wordLoopPlan();
  Plan.Cutpoints[0].OriginalState.push_back(Plan.Cutpoints[0].OriginalState[0]);
  loopRefused(loopCheck(A, A, Plan), Status::Invalid);
  loopRefused(loopCheck(A, A, {}), Status::Invalid);
}

TEST(LowIRLoopRefinement, RankCannotResetThroughUnrepresentedParameter) {
  const auto A = wordLoop();
  auto Plan = wordLoopPlan();
  // Restricting the seed state while retaining an unrelated parameter would
  // let an unsound representation pretend that the ranking value decreased.
  Plan.Cutpoints[0].OriginalState[1].Value = n(0);
  auto Zero = A;
  Zero.Contract.EntryConstants.push_back({r(16), 0});
  const auto R = loopCheck(Zero, Zero, Plan);
  loopRefused(R, Status::Different);
  EXPECT_NE(R.Diagnostic.find("parameter projection"), std::string::npos);
}

TEST(LowIRLoopRefinement, AllStagesShareBudgetsAndDigestBindsPlan) {
  const auto A = wordLoop();
  auto Plan = wordLoopPlan();
  const auto R = loopCheck(A, A, Plan);
  ASSERT_TRUE(R.proved()) << R.Diagnostic;
  LowIRRefinementLimits Limits;
  Limits.Execution.MaxInstructions = R.Instructions - 1;
  loopRefused(loopCheck(A, A, Plan, Limits), Status::BudgetExceeded);
  Limits = {};
  Limits.MaxTerminalPairs = R.TerminalPairs - 1;
  loopRefused(loopCheck(A, A, Plan, Limits), Status::BudgetExceeded);
  Limits = {};
  Limits.Execution.MaxSolverQueries = R.SolverQueries - 1;
  loopRefused(loopCheck(A, A, Plan, Limits), Status::BudgetExceeded);
  Plan.Cutpoints[0].Rank.insert(Plan.Cutpoints[0].Rank.begin(), n(42));
  const auto Other = loopCheck(A, A, Plan);
  ASSERT_TRUE(Other.proved()) << Other.Diagnostic;
  EXPECT_NE(R.Certificate->InputDigest, Other.Certificate->InputDigest);
}

TEST(LowIRLoopRefinement, EntryPrefixIsCheckedRatherThanAssumed) {
  const auto A = wordLoop();
  auto Plan = wordLoopPlan();
  Plan.Cutpoints[0].UseEntryPrefix = true;
  const auto R = loopCheck(A, wordLoop(1, true), Plan);
  ASSERT_TRUE(R.proved()) << R.Diagnostic;
  const auto Explicit = loopCheck(A, A, wordLoopPlan());
  ASSERT_TRUE(Explicit.proved());
  EXPECT_NE(R.Certificate->InputDigest, Explicit.Certificate->InputDigest);
  // Keeping the entry accumulator fixed at its first arrival cannot prove
  // later iterations. A prefix snapshot is not itself an invariant.
  Plan.Cutpoints[0].OriginalState.erase(
      Plan.Cutpoints[0].OriginalState.begin());
  Plan.Cutpoints[0].CandidateState.erase(
      Plan.Cutpoints[0].CandidateState.begin());
  loopRefused(loopCheck(A, A, Plan), Status::Different);
}

Program nestedWordLoops() {
  Program A;
  A.Function.Blocks[0].Succs = {1};
  A.instruction({op(NdOp::COPY, r(0), {n(0)}), op(NdOp::COPY, r(8), {r(16)}),
                 op(NdOp::COPY, r(24), {n(0)}),
                 op(NdOp::BRANCH, {}, {n(0x200)})});
  A.block(1, 0x200, {2, 6});
  A.instruction({op(NdOp::INT_EQUAL, NdVar::tmp(0, 1), {r(8), n(0)}),
                 op(NdOp::COND_BR, {}, {n(0x700), NdVar::tmp(0, 1)})});
  A.block(2, 0x300, {3});
  A.instruction(
      {op(NdOp::COPY, r(24), {r(32)}), op(NdOp::BRANCH, {}, {n(0x400)})});
  A.block(3, 0x400, {4, 5});
  A.instruction({op(NdOp::INT_EQUAL, NdVar::tmp(0, 1), {r(24), n(0)}),
                 op(NdOp::COND_BR, {}, {n(0x600), NdVar::tmp(0, 1)})});
  A.block(4, 0x500, {3});
  A.instruction({op(NdOp::INT_ADD, r(0), {r(0), r(24)}),
                 op(NdOp::INT_SUB, r(24), {r(24), n(1)}),
                 op(NdOp::BRANCH, {}, {n(0x400)})});
  A.block(5, 0x600, {1});
  A.instruction({op(NdOp::INT_SUB, r(8), {r(8), n(1)}),
                 op(NdOp::BRANCH, {}, {n(0x200)})});
  A.block(6, 0x700);
  A.finish();
  return A;
}

TEST(LowIRLoopRefinement, NestedLoopsRequireLexicographicPhaseAndCount) {
  const auto A = nestedWordLoops();
  auto Plan = wordLoopPlan();
  auto &Outer = Plan.Cutpoints[0];
  Outer.Rank = {NdVar::tmp(8, 8), n(1, 1), n(0)};
  Outer.OriginalState.push_back({regLocation(24), n(0)});
  Outer.CandidateState = Outer.OriginalState;
  auto Inner = Outer;
  Inner.OriginalAddress = Inner.CandidateAddress = 0x400;
  Inner.Inputs.push_back(
      {LowIRLoopSide::Original, regLocation(24), NdVar::tmp(16, 8)});
  Inner.OriginalState.back().Value = NdVar::tmp(16, 8);
  Inner.CandidateState = Inner.OriginalState;
  Inner.Expressions = {
      op(NdOp::INT_NOTEQUAL, NdVar::tmp(24, 1), {NdVar::tmp(8, 8), n(0)})};
  Inner.Predicate = NdVar::tmp(24, 1);
  Inner.Rank = {NdVar::tmp(8, 8), n(0, 1), NdVar::tmp(16, 8)};
  Plan.Cutpoints.push_back(Inner);
  const auto Good = loopCheck(A, A, Plan);
  ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
  EXPECT_EQ(Good.RankingChecks, 3U);
  EXPECT_EQ(Good.LoopTransitions, 3U);
  // Dropping the nonzero outer-domain obligation admits underflow on the
  // inner exit, even though finite runs of small positive counts look fine.
  Plan.Cutpoints[1].Predicate = n(1, 1);
  loopRefused(loopCheck(A, A, Plan), Status::Different);
  Plan.Cutpoints[1].Predicate = Inner.Predicate;
  Plan.Cutpoints[1].Rank[1] = n(0, 8);
  loopRefused(loopCheck(A, A, Plan), Status::Invalid);
  Plan.Cutpoints[1].Rank = Inner.Rank;
  Plan.Cutpoints[1].UseEntryPrefix = true;
  // An independently reached inner prefix requires a positive entry count.
  // Unconstrained outer parameters must not invent reachability when the
  // original entry count is zero.
  loopRefused(loopCheck(A, A, Plan), Status::Different);
  for (auto &Cut : Plan.Cutpoints) {
    Cut.Inputs.push_back(
        {LowIRLoopSide::Entry, regLocation(16), NdVar::tmp(32, 8)});
    Cut.Expressions.push_back(op(NdOp::INT_LESSEQUAL, NdVar::tmp(40, 1),
                                 {Cut.Rank[0], NdVar::tmp(32, 8)}));
    Cut.Expressions.push_back(op(NdOp::BOOL_AND, NdVar::tmp(48, 1),
                                 {Cut.Predicate, NdVar::tmp(40, 1)}));
    Cut.Predicate = NdVar::tmp(48, 1);
  }
  const auto Prefix = loopCheck(A, A, Plan);
  ASSERT_TRUE(Prefix.proved()) << Prefix.Diagnostic;
  EXPECT_GT(Prefix.LoopInitiations, Good.LoopInitiations);
  EXPECT_EQ(Prefix.RankingChecks, Good.RankingChecks);
  EXPECT_NE(Prefix.Certificate->InputDigest, Good.Certificate->InputDigest);
  LowIRRefinementLimits Limits;
  Limits.Execution.MaxOperations = Prefix.Operations - 1;
  loopRefused(loopCheck(A, A, Plan, Limits), Status::BudgetExceeded);
}

TEST(LowIRLoopInference, NestedCountersNeedNoHandwrittenPhases) {
  const auto A = nestedWordLoops();
  for (const auto &Eligible :
       {std::vector<va_t>{}, std::vector<va_t>{0x300, 0x500}}) {
    const auto Inferred =
        inferLowIRLoopRefinementPlan(A.Function, A.Contract, {}, Eligible);
    ASSERT_TRUE(Inferred.inferred())
        << Inferred.Diagnostic << " cuts=" << Inferred.CutpointAttempts
        << " rounds=" << Inferred.WideningRounds;
    if (!Eligible.empty())
      ASSERT_GT(Inferred.Plan->Cutpoints.size(), 1U);
    EXPECT_GT(Inferred.Plan->Cutpoints[0].Rank.size(), 1U);
    const auto Proof = loopCheck(A, A, *Inferred.Plan);
    ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
    auto Wrong = A;
    Wrong.Function.Blocks[4].Ops[0].Opcode = NdOp::INT_SUB;
    loopRefused(loopCheck(A, Wrong, *Inferred.Plan), Status::Different);
    LowIRLoopInferenceLimits Limits;
    Limits.MaxCutpointAttempts = Inferred.Plan->Cutpoints.size() - 1;
    const auto TooFew =
        inferLowIRLoopRefinementPlan(A.Function, A.Contract, Limits, Eligible);
    EXPECT_EQ(TooFew.Status, LowIRLoopInferenceStatus::BudgetExceeded);
    EXPECT_FALSE(TooFew.Plan);
  }
}

Program deeperWordLoops(unsigned Depth, bool Ascending, unsigned Step = 1) {
  Program P;
  const auto Address = [](unsigned Id) { return n(0x100 + 0x100 * Id); };
  const auto Counter = [](unsigned Level) { return r(8 + 8 * Level); };
  const auto Bound = [](unsigned Level) { return r(64 + 8 * Level); };
  const unsigned Exit = 1 + 3 * Depth;
  P.Function.Blocks[0].Succs = {1};
  P.instruction({op(NdOp::COPY, r(0), {n(0)}),
                 op(NdOp::COPY, Counter(0), {Ascending ? n(0) : Bound(0)}),
                 op(NdOp::BRANCH, {}, {Address(1)})});
  for (unsigned Level = 0; Level != Depth; ++Level) {
    const unsigned Header = 1 + 3 * Level, Body = Header + 1, Tail = Header + 2;
    const unsigned Done = Level ? 3 * Level : Exit;
    P.block(Header, Address(Header).Offset,
            {static_cast<int>(Body), static_cast<int>(Done)});
    P.instruction(
        {op(Ascending ? NdOp::INT_LESSEQUAL : NdOp::INT_EQUAL, NdVar::tmp(0, 1),
            {Ascending ? Bound(Level) : Counter(Level),
             Ascending ? Counter(Level) : n(0)}),
         op(NdOp::COND_BR, {}, {Address(Done), NdVar::tmp(0, 1)})});
    const unsigned Next = Level + 1 == Depth ? Tail : Header + 3;
    P.block(Body, Address(Body).Offset, {static_cast<int>(Next)});
    if (Level + 1 == Depth)
      P.instruction({op(NdOp::INT_ADD, r(0), {r(0), Counter(Level)}),
                     op(NdOp::BRANCH, {}, {Address(Next)})});
    else
      P.instruction({op(NdOp::COPY, Counter(Level + 1),
                        {Ascending ? n(0) : Bound(Level + 1)}),
                     op(NdOp::BRANCH, {}, {Address(Next)})});
    P.block(Tail, Address(Tail).Offset, {static_cast<int>(Header)});
    P.instruction({op(Ascending ? NdOp::INT_ADD : NdOp::INT_SUB, Counter(Level),
                      {Counter(Level), n(Step)}),
                   op(NdOp::BRANCH, {}, {Address(Header)})});
  }
  P.block(Exit, Address(Exit).Offset);
  P.finish();
  return P;
}

TEST(LowIRLoopInference, ThreeLevelsAndOppositeCounterDirections) {
  for (bool Ascending : {false, true}) {
    const auto P = deeperWordLoops(3, Ascending);
    const auto Inferred = inferLowIRLoopRefinementPlan(P.Function, P.Contract);
    ASSERT_TRUE(Inferred.inferred())
        << "ascending=" << Ascending << ": " << Inferred.Diagnostic;
    const auto Proof = loopCheck(P, P, *Inferred.Plan);
    ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
    EXPECT_GE(Inferred.Plan->Cutpoints.front().Rank.size(), 3U);
  }
}

TEST(LowIRLoopRefinement, UnreachableNestedPrefixCannotSeedInduction) {
  auto A = wordLoop();
  A.Contract.EntryConstants.push_back({r(16), 0});
  auto Plan = wordLoopPlan();
  auto Body = Plan.Cutpoints[0];
  Body.OriginalAddress = Body.CandidateAddress = 0x300;
  Body.UseEntryPrefix = true;
  Plan.Cutpoints.push_back(Body);
  const auto Proof = loopCheck(A, A, Plan);
  loopRefused(Proof, Status::Unsupported);
  EXPECT_NE(Proof.Diagnostic.find("reachable entry prefix"), std::string::npos);
}

TEST(LowIRLoopInference, NestedInfinitePathsAndSharedBudgetsRefuse) {
  const auto Good = deeperWordLoops(2, false);
  const auto Inferred =
      inferLowIRLoopRefinementPlan(Good.Function, Good.Contract);
  ASSERT_TRUE(Inferred.inferred()) << Inferred.Diagnostic;
  for (unsigned Step : {0, 2}) {
    const auto Bad = deeperWordLoops(2, false, Step);
    const auto Refused =
        inferLowIRLoopRefinementPlan(Bad.Function, Bad.Contract);
    EXPECT_FALSE(Refused.inferred());
    EXPECT_FALSE(Refused.Plan);
    loopRefused(loopCheck(Bad, Bad, *Inferred.Plan), Status::Different);
  }
  for (unsigned Kind = 0; Kind != 4; ++Kind) {
    LowIRLoopInferenceLimits Limits;
    if (Kind == 0)
      Limits.Execution.MaxPaths = Inferred.ScheduledPaths - 1;
    if (Kind == 1)
      Limits.Execution.MaxSolverQueries = Inferred.SolverQueries - 1;
    if (Kind == 2)
      Limits.MaxWideningRounds = Inferred.WideningRounds - 1;
    if (Kind == 3)
      Limits.MaxRankCandidates = Inferred.RankCandidates - 1;
    const auto Refused =
        inferLowIRLoopRefinementPlan(Good.Function, Good.Contract, Limits);
    EXPECT_EQ(Refused.Status, LowIRLoopInferenceStatus::BudgetExceeded)
        << Refused.Diagnostic;
    EXPECT_FALSE(Refused.Plan);
  }
  const auto Proof = loopCheck(Good, Good, *Inferred.Plan);
  ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
  LowIRRefinementLimits Limits;
  Limits.Execution.MaxInstructions = Proof.Instructions - 1;
  loopRefused(loopCheck(Good, Good, *Inferred.Plan, Limits),
              Status::BudgetExceeded);
}

TEST(LowIRLoopRefinement, DisjointPrefixDomainsCannotBePaired) {
  const auto A = wordLoop();
  auto B = A;
  B.Function.Blocks[1].Ops[0].Opcode = NdOp::INT_NOTEQUAL;
  auto Plan = wordLoopPlan();
  auto Body = Plan.Cutpoints[0];
  Body.OriginalAddress = Body.CandidateAddress = 0x300;
  Body.UseEntryPrefix = true;
  Plan.Cutpoints.push_back(Body);
  const auto Proof = loopCheck(A, B, Plan);
  loopRefused(Proof, Status::Unsupported);
  EXPECT_NE(Proof.Diagnostic.find("reachable entry prefix"), std::string::npos);
}

TEST(LowIRLoopRefinement, SelectedUndefinedBitsRemainCorrelatedAcrossSpills) {
  auto A = wordLoop(), B = wordLoop(1, true);
  // Each iteration selects the lifted low accumulator bit. It is copied to a
  // frame slot, whose invariant shares that value with the register carrier.
  A.frame();
  auto Spill = [](Program &P) {
    auto &Body = P.Function.Blocks[2];
    auto &Boundary = Body.InstructionBoundaries[0];
    auto O = op(NdOp::STORE, {}, {r(32), r(0)});
    O.Addr = Boundary.Address;
    O.Seq = Boundary.OpCount;
    Body.Ops.insert(Body.Ops.begin() + Boundary.OpCount, O);
    ++Boundary.OpCount;
    ++Body.InstructionBoundaries[1].FirstOp;
    P.Records[2].Boundary = Boundary;
    P.Records[2].Effects.OpCount = Boundary.OpCount;
    P.Records[2].Effects.OperationDigest = lowUndefinedOperationDigest(
        llvm::ArrayRef<LowOp>(Body.Ops).take_front(Boundary.OpCount));
    P.Records[3].Boundary = Body.InstructionBoundaries[1];
  };
  Spill(A);
  Spill(B);
  A.Records[2].Effects.Effects.push_back({1, r(0), 0, 1, {}});
  auto Plan = wordLoopPlan();
  const LowIRLoopLocation Slot{LowIRLoopSpace::Frame, 0, 8};
  // The first visit predates a spill, so overapproximate the old slot as a
  // shared independent parameter. Every iteration must still compare it.
  Plan.Cutpoints[0].Inputs.push_back(
      {LowIRLoopSide::Original, Slot, NdVar::tmp(16, 8)});
  Plan.Cutpoints[0].OriginalState.push_back({Slot, NdVar::tmp(16, 8)});
  Plan.Cutpoints[0].CandidateState.push_back({Slot, NdVar::tmp(16, 8)});
  const auto R = loopCheck(A, B, Plan);
  ASSERT_TRUE(R.proved()) << R.Diagnostic;
  EXPECT_FALSE(R.Certificate->Producers.empty());
  const auto WrongWitness = checkLowIRLoopRefinement(
      A.Function, A.Records, B.Function, A.Contract, Plan, Witness::ZeroBits);
  loopRefused(WrongWitness, Status::Different);
  Plan.Cutpoints[0].CandidateState.back().Value = n(0);
  loopRefused(loopCheck(A, B, Plan), Status::Different);
}

TEST(LowIRLoopRefinement, OverlappingEntryViewsRemainSharedAtLaterCuts) {
  auto A = wordLoop(), B = wordLoop();
  // This additional byte is first read by the loop template, after the real
  // entry has been snapshotted. A later full-width observation must alias it.
  A.Contract.ReturnRegisters.push_back({40, 8});
  auto Plan = wordLoopPlan();
  Plan.Cutpoints[0].Inputs.push_back(
      {LowIRLoopSide::Entry, regLocation(41, 1), NdVar::tmp(16, 1)});
  Plan.Cutpoints[0].OriginalState.push_back(
      {regLocation(41, 1), NdVar::tmp(16, 1)});
  Plan.Cutpoints[0].CandidateState.push_back(
      {regLocation(41, 1), NdVar::tmp(16, 1)});
  const auto Good = loopCheck(A, B, Plan);
  ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
  Plan.Cutpoints[0].CandidateState.back().Location.Offset = 42;
  loopRefused(loopCheck(A, B, Plan), Status::Different);
}
} // namespace
