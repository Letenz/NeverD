//===- LowIRLoopAlignmentTests.cpp - Search paired loop phases ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/analysis/interpreter/LowIRLoopInference.h"
#include "gtest/gtest.h"

#include "neverd/analysis/LowIRRefinement.h"

#include <set>

using namespace neverd;
using namespace neverd::analysis;

namespace {
NdVar n(uint64_t V, uint16_t Bytes = 8) { return NdVar::scalar(V, Bytes); }
NdVar r(uint64_t Offset) { return NdVar::reg(Offset, 8); }
NdVar t(uint64_t Offset, uint16_t Bytes = 8) {
  return NdVar::tmp(Offset, Bytes);
}
LowOp op(NdOp Code, NdVar Output = {},
         std::initializer_list<NdVar> Inputs = {}) {
  LowOp O;
  O.Opcode = Code;
  O.Output = Output;
  for (auto V : Inputs)
    O.addInput(V);
  return O;
}

struct Program {
  LowFunc Function;
  std::vector<LowIRUndefinedInstruction> Records;
  LowIRIndependenceContract Contract;

  Program() {
    Function.Entry = 0x100;
    Contract.Frame = LowIRIndependenceFrame{{32, 8}, 0, 16};
    Contract.ReturnRegisters = {{0, 8}};
  }

  void block(int Id, va_t Address, std::vector<int> Successors) {
    LowBlock B;
    B.Id = Id;
    B.StartAddr = B.EndAddr = Address;
    B.Succs = std::move(Successors);
    Function.Blocks.push_back(std::move(B));
  }

  void instruction(std::initializer_list<LowOp> Ops) {
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
    Effects.OpCount = IB.OpCount;
    Effects.OperationDigest = lowUndefinedOperationDigest(
        llvm::ArrayRef<LowOp>(B.Ops).slice(IB.FirstOp, IB.OpCount));
    Records.push_back({B.Id, IB, std::move(Effects)});
  }

  void add(uint64_t Amount) {
    instruction({op(NdOp::INT_ADD, t(0), {r(32), n(8)}),
                 op(NdOp::LOAD, t(8), {t(0)}),
                 op(NdOp::INT_ADD, t(16), {t(8), n(Amount)}),
                 op(NdOp::STORE, {}, {t(0), t(16)})});
  }
  void decrement() {
    instruction({op(NdOp::LOAD, t(0), {r(32)}),
                 op(NdOp::INT_SUB, t(8), {t(0), n(1)}),
                 op(NdOp::STORE, {}, {r(32), t(8)})});
  }
  void zeroBranch(va_t Address) {
    instruction({op(NdOp::LOAD, t(0), {r(32)}),
                 op(NdOp::INT_EQUAL, t(8, 1), {t(0), n(0)}),
                 op(NdOp::COND_BR, {}, {n(Address), t(8, 1)})});
  }
  void branch(va_t Address) {
    instruction({op(NdOp::BRANCH, {}, {n(Address)})});
  }
  LowIRLoopAlignmentResult check(const Program &B,
                                 const LowIRLoopAlignmentLimits &Limits = {},
                                 LowIRRefinementWitness Witness =
                                     LowIRRefinementWitness::LiftedBits) const {
    return inferAndCheckLowIRLoopRefinement(Function, Records, B.Function,
                                            Contract, Witness, Limits);
  }
};

// Independently authored arithmetic loops: both store a countdown and a sum
// in two frame words, and return 3 * count modulo 2^64. The rotated form tests
// its exit after decrementing and duplicates the final addition. Its preferred
// guarded-body cut is therefore one phase later than the ordinary loop's.
Program counterLoop(bool Rotated = false, uint64_t Amount = 3,
                    bool ClobberCounter = false) {
  Program P;
  P.block(0, 0x100, Rotated ? std::vector<int>{1, 4} : std::vector<int>{1});
  P.instruction({op(NdOp::STORE, {}, {r(32), r(8)}),
                 op(NdOp::INT_ADD, t(0), {r(32), n(8)}),
                 op(NdOp::STORE, {}, {t(0), n(0)})});
  if (Rotated)
    P.zeroBranch(0x500);
  else
    P.branch(0x200);
  P.block(1, 0x200, Rotated ? std::vector<int>{2, 3} : std::vector<int>{2, 4});
  if (Rotated)
    P.decrement();
  P.zeroBranch(Rotated ? 0x400 : 0x500);
  P.block(2, 0x300, {1});
  P.add(Amount);
  if (!Rotated)
    P.decrement();
  P.branch(0x200);
  if (Rotated) {
    P.block(3, 0x400, {4});
    P.add(Amount);
    P.branch(0x500);
  }
  P.block(4, 0x500, {});
  if (ClobberCounter)
    P.instruction({op(NdOp::STORE, {}, {r(32), n(17)})});
  P.instruction({op(NdOp::INT_ADD, t(0), {r(32), n(8)}),
                 op(NdOp::LOAD, r(0), {t(0)}), op(NdOp::RETURN, {}, {r(0)})});
  return P;
}

TEST(LowIRLoopAlignment, AlignedLoopsGetFreshCertificate) {
  const auto A = counterLoop();
  const auto R = A.check(A);
  ASSERT_TRUE(R.proved()) << R.Diagnostic << ": " << R.LastCandidateDiagnostic;
  EXPECT_EQ(R.CandidateAttempts, 1U);
  EXPECT_EQ(R.PairingAttempts, 1U);
  ASSERT_TRUE(R.Refinement.Certificate);
  EXPECT_EQ(R.Refinement.Certificate->Scope,
            LowIRRefinementScope::InductiveLowIRLoops);
}

// A nested loop with a separate reset phase: the inner counter is reset on
// one visit, and the outer counter decreases on the next. All frame words
// remain observable, including the phase flag and both counters.
Program phasedResetLoop(uint64_t Amount = 3, uint64_t OuterStep = 1,
                        bool TrailingLoop = false,
                        bool LocalExitGuards = false) {
  Program P;
  P.Contract.Frame->End = TrailingLoop ? 40 : 32;
  const auto Load = [&](uint64_t Offset, NdVar Output) {
    P.instruction({op(NdOp::INT_ADD, t(0), {r(32), n(Offset)}),
                   op(NdOp::LOAD, Output, {t(0)})});
  };
  const auto Store = [&](uint64_t Offset, NdVar Value) {
    P.instruction({op(NdOp::INT_ADD, t(0), {r(32), n(Offset)}),
                   op(NdOp::STORE, {}, {t(0), Value})});
  };
  const auto Add = [&](uint64_t Offset, uint64_t Step) {
    P.instruction({op(NdOp::INT_ADD, t(0), {r(32), n(Offset)}),
                   op(NdOp::LOAD, t(8), {t(0)}),
                   op(NdOp::INT_ADD, t(16), {t(8), n(Step)}),
                   op(NdOp::STORE, {}, {t(0), t(16)})});
  };
  P.block(0, 0x100, {1});
  Store(0, r(8));
  for (uint64_t Offset : {8, 16, 24})
    Store(Offset, n(0));
  if (TrailingLoop)
    Store(32, n(0));
  P.branch(0x200);
  P.block(1, 0x200,
          LocalExitGuards ? std::vector<int>{2} : std::vector<int>{2, 8});
  if (LocalExitGuards)
    P.branch(0x300);
  else
    P.zeroBranch(0x900);
  P.block(2, 0x300, {3, LocalExitGuards ? 22 : 7});
  P.instruction(
      {op(NdOp::INT_ADD, t(0), {r(32), n(24)}), op(NdOp::LOAD, t(8), {t(0)}),
       op(NdOp::INT_NOTEQUAL, t(16, 1), {t(8), n(0)}),
       op(NdOp::COND_BR, {}, {n(LocalExitGuards ? 0x1700 : 0x800), t(16, 1)})});
  P.block(3, 0x400,
          LocalExitGuards ? std::vector<int>{20, 21} : std::vector<int>{4, 5});
  P.instruction(
      {op(NdOp::INT_ADD, t(0), {r(32), n(8)}), op(NdOp::LOAD, t(8), {t(0)}),
       op(NdOp::INT_LESS, t(16, 1), {t(8), r(16)}),
       op(NdOp::COND_BR, {}, {n(LocalExitGuards ? 0x1500 : 0x500), t(16, 1)})});
  P.block(4, 0x500, {1});
  Add(8, 1);
  Add(16, Amount);
  P.branch(0x200);
  P.block(5, 0x600, {1});
  Store(8, n(0));
  Store(24, n(1));
  P.branch(0x200);
  P.block(7, 0x800, {1});
  Add(0, -OuterStep);
  Store(24, n(0));
  P.branch(0x200);
  if (LocalExitGuards)
    for (const auto &[Id, Target] : {std::pair{20, 4}, {21, 5}, {22, 7}}) {
      P.block(Id, (Id + 1) * 0x100, {Target, 8});
      P.zeroBranch(0x900);
    }
  P.block(8, 0x900,
          TrailingLoop ? std::vector<int>{9, 10} : std::vector<int>{});
  if (TrailingLoop) {
    Store(32, r(24));
    P.instruction({op(NdOp::INT_ADD, t(0), {r(32), n(32)}),
                   op(NdOp::LOAD, t(8), {t(0)}),
                   op(NdOp::INT_EQUAL, t(16, 1), {t(8), n(0)}),
                   op(NdOp::COND_BR, {}, {n(0xb00), t(16, 1)})});
    P.block(9, 0xa00, {9, 10});
    Add(32, -1);
    P.instruction({op(NdOp::INT_ADD, t(0), {r(32), n(32)}),
                   op(NdOp::LOAD, t(8), {t(0)}),
                   op(NdOp::INT_NOTEQUAL, t(16, 1), {t(8), n(0)}),
                   op(NdOp::COND_BR, {}, {n(0xa00), t(16, 1)})});
    P.block(10, 0xb00, {});
  }
  Load(16, r(0));
  P.instruction({op(NdOp::RETURN, {}, {r(0)})});
  return P;
}

TEST(LowIRLoopAlignment, SeparateResetAndProgressDiscoverPhaseCuts) {
  const auto P = phasedResetLoop();
  const auto R = P.check(P);
  ASSERT_TRUE(R.proved()) << R.Diagnostic << ": " << R.LastCandidateDiagnostic;
  ASSERT_TRUE(R.Refinement.Certificate);
  EXPECT_GT(R.Refinement.Certificate->LoopPlan->Cutpoints.size(), 1U);
  EXPECT_GT(R.Refinement.RankingChecks, 0U);
}

TEST(LowIRLoopAlignment, CrossFamiliesReuseCandidateInference) {
  const auto Shared = phasedResetLoop();
  const auto Local = phasedResetLoop(3, 1, false, true);
  const auto Default =
      inferLowIRLoopRefinementPlan(Local.Function, Local.Contract);
  ASSERT_TRUE(Default.inferred()) << Default.Diagnostic;
  ASSERT_EQ(Default.Plan->Cutpoints.size(), 3U);
  const auto Duplicate = detail::inferBranchArmLowIRLoopRefinementPlan(
      Local.Function, Local.Contract, {}, &*Default.Plan);
  EXPECT_FALSE(Duplicate.inferred());
  EXPECT_EQ(Duplicate.SolverQueries, 0U);
  EXPECT_EQ(Duplicate.Diagnostic, "branch-arm cuts duplicate default plan");
  const auto SharedDefault =
      inferLowIRLoopRefinementPlan(Shared.Function, Shared.Contract);
  ASSERT_FALSE(SharedDefault.inferred());

  // The default original plan and branch-arm candidate plan are the only
  // available pair. Both candidate families must be cached: replaying either
  // one during cross-family matching would exceed this attempt limit.
  LowIRLoopAlignmentLimits Limits;
  Limits.MaxCandidateAttempts = 2;
  const auto Forward = Local.check(Shared, Limits);
  ASSERT_TRUE(Forward.proved())
      << Forward.Diagnostic << ": " << Forward.LastCandidateDiagnostic;
  EXPECT_EQ(Forward.CandidateAttempts, 2U);
  EXPECT_TRUE(Shared.check(Local, Limits).proved());
}

TEST(LowIRLoopAlignment, PhaseCutsRejectWrongResultsAndMissingProgress) {
  const auto P = phasedResetLoop();
  for (const auto &Wrong : {phasedResetLoop(4), phasedResetLoop(3, 0)}) {
    const auto R = P.check(Wrong);
    EXPECT_FALSE(R.proved());
    EXPECT_FALSE(R.Refinement.Certificate);
  }
}

TEST(LowIRLoopAlignment, PhaseFamiliesKeepExactCumulativeBudgets) {
  const auto P = phasedResetLoop();
  const auto Good = P.check(P);
  ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
  LowIRLoopAlignmentLimits Limits;
  Limits.MaxSolverQueries = Good.SolverQueries;
  Limits.MaxCandidateAttempts = Good.CandidateAttempts;
  Limits.MaxPairingAttempts = Good.PairingAttempts;
  const auto Exact = P.check(P, Limits);
  ASSERT_TRUE(Exact.proved()) << Exact.Diagnostic;
  for (unsigned Kind = 0; Kind != 3; ++Kind) {
    auto Short = Limits;
    if (Kind == 0)
      --Short.MaxSolverQueries;
    else if (Kind == 1)
      --Short.MaxCandidateAttempts;
    else
      --Short.MaxPairingAttempts;
    const auto R = P.check(P, Short);
    EXPECT_EQ(R.Status, LowIRLoopAlignmentStatus::BudgetExceeded)
        << R.Diagnostic;
    EXPECT_FALSE(R.Refinement.Certificate);
  }
}

TEST(LowIRLoopAlignment, BranchArmsKeepPhasesAndCompleteOtherCycleCoverage) {
  for (bool Trailing : {false, true}) {
    SCOPED_TRACE(Trailing);
    const auto P = phasedResetLoop(3, 1, Trailing);
    LowIRLoopInferenceLimits Limits;
    Limits.Execution.MaxSolverQueries = 16384;
    const auto R = detail::inferBranchArmLowIRLoopRefinementPlan(
        P.Function, P.Contract, Limits);
    ASSERT_TRUE(R.inferred()) << R.Diagnostic;
    std::set<va_t> Expected{0x500, 0x600, 0x800};
    if (Trailing)
      Expected.insert(0xa00);
    std::set<va_t> Actual;
    for (const auto &C : R.Plan->Cutpoints)
      Actual.insert(C.OriginalAddress);
    EXPECT_EQ(Actual, Expected);
    const auto Proof = checkLowIRLoopRefinement(
        P.Function, P.Records, P.Function, P.Contract, *R.Plan);
    ASSERT_TRUE(Proof.proved()) << Proof.Diagnostic;
    auto Short = Limits;
    Short.MaxCutpointAttempts = Expected.size() - 1;
    const auto Rejected = detail::inferBranchArmLowIRLoopRefinementPlan(
        P.Function, P.Contract, Short);
    EXPECT_EQ(Rejected.Status, LowIRLoopInferenceStatus::BudgetExceeded);
    EXPECT_FALSE(Rejected.Plan);
  }
}

TEST(LowIRLoopAlignment, EmptyAndDuplicateBranchFamiliesDoNoSymbolicSearch) {
  Program Empty;
  Empty.block(0, 0x100, {});
  Empty.instruction({op(NdOp::RETURN, {}, {n(0)})});
  const auto E = detail::inferBranchArmLowIRLoopRefinementPlan(
      Empty.Function, Empty.Contract, {});
  EXPECT_EQ(E.Status, LowIRLoopInferenceStatus::Unsupported);
  EXPECT_FALSE(E.Plan);
  EXPECT_EQ(E.SolverQueries, 0U);
  const auto P = counterLoop();
  const auto Default = inferLowIRLoopRefinementPlan(P.Function, P.Contract);
  ASSERT_TRUE(Default.inferred());
  const auto Duplicate = detail::inferBranchArmLowIRLoopRefinementPlan(
      P.Function, P.Contract, {}, &*Default.Plan);
  EXPECT_EQ(Duplicate.Status, LowIRLoopInferenceStatus::Unsupported);
  EXPECT_FALSE(Duplicate.Plan);
  EXPECT_EQ(Duplicate.SolverQueries, 0U);
}

TEST(LowIRLoopAlignment, RetainedPlansShareOneMetadataPool) {
  const auto P = counterLoop();
  const auto Inferred = inferLowIRLoopRefinementPlan(P.Function, P.Contract);
  ASSERT_TRUE(Inferred.inferred());
  uint64_t Size = Inferred.Plan->Cutpoints.size();
  for (const auto &C : Inferred.Plan->Cutpoints)
    Size += C.Inputs.size() + C.Expressions.size() + C.OriginalState.size() +
            C.CandidateState.size() + C.Rank.size();
  LowIRLoopAlignmentLimits Limits;
  Limits.MaxMetadata = Size * 2 - 1;
  Limits.MaxCandidateAttempts = 1;
  ASSERT_LE(Size, Limits.MaxMetadata);
  const auto R = P.check(P, Limits);
  EXPECT_EQ(R.Status, LowIRLoopAlignmentStatus::BudgetExceeded);
  EXPECT_EQ(R.PairingAttempts, 0U);
  EXPECT_FALSE(R.Refinement.Certificate);
  EXPECT_EQ(R.LastCandidateDiagnostic,
            "loop alignment metadata budget exhausted");
}

TEST(LowIRLoopAlignment, PhaseFamiliesRetainOriginalEvidenceAndWitness) {
  auto A = phasedResetLoop();
  const auto B = A;
  A.Records.front().Effects.Coverage = LowUndefinedCoverage::Missing;
  const auto Missing = A.check(B);
  EXPECT_FALSE(Missing.proved());
  EXPECT_FALSE(Missing.Refinement.Certificate);
  A.Records.front().Effects.Coverage = LowUndefinedCoverage::Complete;
  A.Records.front().Effects.Effects.push_back({0, r(8), 0, 1, {}});
  const auto Lifted = A.check(B);
  ASSERT_TRUE(Lifted.proved())
      << Lifted.Diagnostic << ": " << Lifted.LastCandidateDiagnostic;
  EXPECT_GT(Lifted.Refinement.Producers, 0U);
  const auto Zero = A.check(B, {}, LowIRRefinementWitness::ZeroBits);
  EXPECT_FALSE(Zero.proved());
  EXPECT_FALSE(Zero.Refinement.Certificate);
}

TEST(LowIRLoopAlignment, AlternativeCandidatePhaseEstablishesRelation) {
  const auto A = counterLoop(), B = counterLoop(true);
  const auto Left = inferLowIRLoopRefinementPlan(A.Function, A.Contract);
  const auto Right = inferLowIRLoopRefinementPlan(B.Function, A.Contract);
  ASSERT_TRUE(Left.inferred()) << Left.Diagnostic;
  ASSERT_TRUE(Right.inferred()) << Right.Diagnostic;
  ASSERT_TRUE(checkLowIRLoopRefinement(A.Function, A.Records, A.Function,
                                       A.Contract, *Left.Plan)
                  .proved());
  ASSERT_TRUE(checkLowIRLoopRefinement(B.Function, B.Records, B.Function,
                                       A.Contract, *Right.Plan)
                  .proved());
  LowIRLoopAlignmentLimits FirstOnly;
  FirstOnly.MaxCandidateAttempts = 1;
  const auto Rejected = A.check(B, FirstOnly);
  EXPECT_FALSE(Rejected.proved());
  EXPECT_FALSE(Rejected.Refinement.Certificate);
  EXPECT_EQ(Rejected.Refinement.Status, LowIRRefinementStatus::Different);
  const auto R = A.check(B);
  ASSERT_TRUE(R.proved()) << R.Diagnostic << ": " << R.LastCandidateDiagnostic;
  EXPECT_GT(R.CandidateAttempts, 1U);
  EXPECT_GT(R.PairingAttempts, 1U);
  const auto &Certificate = *R.Refinement.Certificate;
  EXPECT_TRUE(checkLowIRLoopRefinement(A.Function, A.Records, B.Function,
                                       A.Contract, *Certificate.LoopPlan,
                                       Certificate.Witness, Certificate.Limits)
                  .proved());
}
TEST(LowIRLoopAlignment, WrongResultAndFrameWritesCannotGetCertificates) {
  const auto A = counterLoop();
  for (const auto &B : {counterLoop(true, 4), counterLoop(true, 3, true)}) {
    const auto R = A.check(B);
    EXPECT_FALSE(R.proved());
    EXPECT_FALSE(R.Refinement.Certificate);
    EXPECT_EQ(R.Status, LowIRLoopAlignmentStatus::Unsupported) << R.Diagnostic;
    EXPECT_FALSE(R.LastCandidateDiagnostic.empty());
  }
}

TEST(LowIRLoopAlignment, OriginalAuditedRecordsRemainMandatory) {
  const auto B = counterLoop(true);
  for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
    auto A = counterLoop();
    if (Mutation == 0)
      A.Records.front().Effects.Coverage = LowUndefinedCoverage::Missing;
    else if (Mutation == 1)
      A.Records.front().Effects.OperationDigest = "stale";
    else
      A.Records.pop_back();
    const auto R = A.check(B);
    EXPECT_FALSE(R.proved());
    EXPECT_FALSE(R.Refinement.Certificate);
    EXPECT_EQ(R.Status, Mutation == 1 ? LowIRLoopAlignmentStatus::Invalid
                                      : LowIRLoopAlignmentStatus::Unsupported)
        << R.Diagnostic;
    EXPECT_EQ(R.Refinement.Status, Mutation == 1
                                       ? LowIRRefinementStatus::Invalid
                                       : LowIRRefinementStatus::Unsupported);
  }
}

TEST(LowIRLoopAlignment, OriginalUndefinedWitnessIsNotReplacedBySelfInference) {
  auto A = counterLoop();
  const auto B = counterLoop(true);
  A.Records.front().Effects.Effects.push_back({0, r(8), 0, 1, {}});
  const auto Lifted = A.check(B);
  ASSERT_TRUE(Lifted.proved())
      << Lifted.Diagnostic << ": " << Lifted.LastCandidateDiagnostic;
  EXPECT_EQ(Lifted.Refinement.Certificate->Witness,
            LowIRRefinementWitness::LiftedBits);
  EXPECT_GT(Lifted.Refinement.Producers, 0U);
  const auto Zero = A.check(B, {}, LowIRRefinementWitness::ZeroBits);
  EXPECT_FALSE(Zero.proved());
  EXPECT_FALSE(Zero.Refinement.Certificate);
}

TEST(LowIRLoopAlignment, QueriesRemainChargedAcrossFailedAttempts) {
  const auto A = counterLoop(), B = counterLoop(true);
  LowIRLoopAlignmentLimits FirstOnly;
  FirstOnly.MaxCandidateAttempts = 1;
  const auto First = A.check(B, FirstOnly);
  ASSERT_EQ(First.PairingAttempts, 1U);
  ASSERT_GT(First.SolverQueries, First.Refinement.SolverQueries);
  LowIRLoopAlignmentLimits Limited;
  Limited.MaxSolverQueries = First.SolverQueries + 1;
  const auto Exhausted = A.check(B, Limited);
  EXPECT_FALSE(Exhausted.proved());
  EXPECT_FALSE(Exhausted.Refinement.Certificate);
  EXPECT_EQ(Exhausted.Status, LowIRLoopAlignmentStatus::BudgetExceeded);
  EXPECT_EQ(Exhausted.SolverQueries, Limited.MaxSolverQueries);
  // The duplicate branch family consumes an attempt but no solver query;
  // the following singleton exhausts the remaining shared query allowance.
  EXPECT_EQ(Exhausted.CandidateAttempts, 3U);
  EXPECT_NE(Exhausted.Diagnostic.find("total query budget"), std::string::npos);
  EXPECT_FALSE(Exhausted.LastCandidateDiagnostic.empty());
}

TEST(LowIRLoopAlignment, ExactTotalBudgetCanCompleteTheFinalProof) {
  const auto A = counterLoop(), B = counterLoop(true);
  const auto Baseline = A.check(B);
  ASSERT_TRUE(Baseline.proved());
  LowIRLoopAlignmentLimits Limits;
  Limits.MaxSolverQueries = Baseline.SolverQueries;
  const auto Exact = A.check(B, Limits);
  EXPECT_TRUE(Exact.proved()) << Exact.Diagnostic;
  EXPECT_EQ(Exact.SolverQueries, Limits.MaxSolverQueries);
  --Limits.MaxSolverQueries;
  const auto Short = A.check(B, Limits);
  EXPECT_EQ(Short.Status, LowIRLoopAlignmentStatus::BudgetExceeded);
  EXPECT_FALSE(Short.Refinement.Certificate);
  EXPECT_EQ(Short.SolverQueries, Limits.MaxSolverQueries);
}

TEST(LowIRLoopAlignment, PerAttemptQueryExhaustionCanTryAnotherCut) {
  const auto A = counterLoop(), B = counterLoop(true);
  const auto Default = inferLowIRLoopRefinementPlan(B.Function, A.Contract);
  const auto Alternative =
      inferLowIRLoopRefinementPlan(B.Function, A.Contract, {}, {0x200});
  ASSERT_TRUE(Default.inferred());
  ASSERT_TRUE(Alternative.inferred());
  ASSERT_LT(Alternative.SolverQueries, Default.SolverQueries);
  LowIRLoopAlignmentLimits Limits;
  Limits.CandidateInference.Execution.MaxSolverQueries =
      Alternative.SolverQueries;
  const auto R = A.check(B, Limits);
  ASSERT_TRUE(R.proved()) << R.Diagnostic << ": " << R.LastCandidateDiagnostic;
  EXPECT_GT(R.CandidateAttempts, 1U);
  EXPECT_GT(R.SolverQueries, Alternative.SolverQueries);
}

TEST(LowIRLoopAlignment, SearchLimitsCannotYieldPartialSuccess) {
  const auto A = counterLoop(), B = counterLoop(true);
  for (unsigned Budget = 0; Budget != 8; ++Budget) {
    LowIRLoopAlignmentLimits L;
    switch (Budget) {
    case 0:
      L.MaxSolverQueries = 0;
      break;
    case 1:
      L.MaxSearchWork = 1;
      break;
    case 2:
      L.MaxCandidateAttempts = 0;
      break;
    case 3:
      L.MaxPairingAttempts = 0;
      break;
    case 4:
      L.MaxMetadata = 0;
      break;
    case 5:
      L.MaxCuts = 0;
      break;
    case 6:
      L.OriginalInference.Execution.MaxSolverQueries = 0;
      break;
    case 7:
      L.Proof.Execution.MaxSolverQueries = 0;
      break;
    }
    const auto R = A.check(B, L);
    EXPECT_EQ(R.Status, LowIRLoopAlignmentStatus::BudgetExceeded)
        << Budget << ": " << R.Diagnostic;
    EXPECT_FALSE(R.proved());
    EXPECT_FALSE(R.Refinement.Certificate);
    EXPECT_LE(R.SolverQueries, L.MaxSolverQueries);
    EXPECT_LE(R.SearchWork, L.MaxSearchWork);
  }
}

TEST(LowIRLoopAlignment, MalformedGraphsAreRefusedBeforeSearch) {
  const auto A = counterLoop();
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    auto B = counterLoop(true);
    switch (Mutation) {
    case 0:
      B.Function.Blocks.back().Id = B.Function.Blocks.front().Id;
      break;
    case 1:
      B.Function.Blocks.back().StartAddr = B.Function.Entry;
      break;
    case 2:
      B.Function.Blocks.front().Succs.push_back(999);
      break;
    case 3:
      B.Function.Blocks.front().Succs.push_back(1);
      break;
    case 4:
      B.Function.Entry = 999;
      break;
    }
    const auto R = A.check(B);
    EXPECT_EQ(R.Status, LowIRLoopAlignmentStatus::Invalid) << R.Diagnostic;
    EXPECT_FALSE(R.Refinement.Certificate);
    EXPECT_EQ(R.SolverQueries, 0U);
  }
}

TEST(LowIRLoopAlignment, NondecreasingAndWrappingCountersRemainUnproved) {
  const auto A = counterLoop();
  for (uint64_t Step : {uint64_t{0}, ~uint64_t{0}}) {
    auto B = counterLoop(true);
    B.Function.Blocks[1].Ops[1].Inputs[1] = n(Step);
    LowIRLoopAlignmentLimits Limits;
    Limits.CandidateInference.Execution.MaxSolverQueries = 512;
    const auto R = A.check(B, Limits);
    EXPECT_FALSE(R.proved());
    EXPECT_FALSE(R.Refinement.Certificate);
    EXPECT_NE(R.Status, LowIRLoopAlignmentStatus::Invalid);
  }
}

Program alternativeLoops(bool ReverseAddresses) {
  Program P;
  P.block(0, 0x100, {1, 3});
  P.instruction(
      {op(NdOp::STORE, {}, {r(32), r(8)}),
       op(NdOp::INT_ADD, t(0), {r(32), n(8)}),
       op(NdOp::STORE, {}, {t(0), n(0)}),
       op(NdOp::INT_EQUAL, t(8, 1), {r(16), n(0)}),
       op(NdOp::COND_BR, {}, {n(ReverseAddresses ? 0x200 : 0x400), t(8, 1)})});
  for (unsigned I = 0; I != 2; ++I) {
    const unsigned Header = 1 + I * 2;
    const va_t Address = 0x200 + I * 0x200;
    P.block(Header, Address, {int(Header + 1), 5});
    P.zeroBranch(0x600);
    P.block(Header + 1, Address + 0x100, {int(Header)});
    P.add((I == 0) != ReverseAddresses ? 3 : 5);
    P.decrement();
    P.branch(Address);
  }
  P.block(5, 0x600, {});
  P.instruction({op(NdOp::INT_ADD, t(0), {r(32), n(8)}),
                 op(NdOp::LOAD, r(0), {t(0)}), op(NdOp::RETURN, {}, {r(0)})});
  return P;
}

TEST(LowIRLoopAlignment, MultipleCutsUseCheckedBoundedPermutations) {
  const auto A = alternativeLoops(false), B = alternativeLoops(true);
  const auto R = A.check(B);
  ASSERT_TRUE(R.proved()) << R.Diagnostic << ": " << R.LastCandidateDiagnostic;
  ASSERT_EQ(R.Refinement.Certificate->LoopPlan->Cutpoints.size(), 2U);
  EXPECT_EQ(R.CandidateAttempts, 1U);
  EXPECT_GT(R.PairingAttempts, 1U);
  LowIRLoopAlignmentLimits Limits;
  Limits.MaxPairingAttempts = 1;
  const auto Exhausted = A.check(B, Limits);
  EXPECT_EQ(Exhausted.Status, LowIRLoopAlignmentStatus::BudgetExceeded);
  EXPECT_FALSE(Exhausted.Refinement.Certificate);
  EXPECT_EQ(Exhausted.PairingAttempts, 1U);
  Limits.MaxPairingAttempts = 128;
  Limits.MaxCuts = 1;
  const auto TooMany = A.check(B, Limits);
  EXPECT_EQ(TooMany.Status, LowIRLoopAlignmentStatus::BudgetExceeded);
  EXPECT_FALSE(TooMany.Refinement.Certificate);
}

TEST(LowIRLoopAlignment, SearchWorkIncludesMatchingAndFailedAttempts) {
  const auto A = counterLoop(), B = counterLoop(true);
  const auto R = A.check(B);
  ASSERT_TRUE(R.proved());
  LowIRLoopAlignmentLimits Limits;
  Limits.MaxSearchWork = R.SearchWork;
  const auto Exact = A.check(B, Limits);
  EXPECT_TRUE(Exact.proved()) << Exact.Diagnostic;
  EXPECT_EQ(Exact.SearchWork, Limits.MaxSearchWork);
  --Limits.MaxSearchWork;
  const auto Exhausted = A.check(B, Limits);
  EXPECT_EQ(Exhausted.Status, LowIRLoopAlignmentStatus::BudgetExceeded);
  EXPECT_FALSE(Exhausted.Refinement.Certificate);
  EXPECT_LE(Exhausted.SearchWork, Limits.MaxSearchWork);
}
} // namespace
