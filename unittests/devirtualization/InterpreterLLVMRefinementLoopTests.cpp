//===- InterpreterLLVMRefinementLoopTests.cpp - Untrusted loop plans -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "InterpreterLLVMRefinementTest.h"

namespace neverd::analysis::llvm_refinement_test {
TEST(InterpreterLLVMRefinement, ArbitraryCountRequiresBothInductivePremises) {
  // jrcxz done; lea rcx,[rcx-1]; jmp entry; done: ret.
  Program P({0xe3, 6, 0x48, 0x8d, 0x49, 0xff, 0xeb, 0xf8, 0xc3});
  const auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  const auto IR = module(R"(
  entry:
    %rcx = getelementptr i8, ptr %state, i64 8
    %initial = load i64, ptr %rcx, align 8
    br label %loop
  loop:
    %count = phi i64 [%initial, %entry], [%next, %body]
    %done = icmp eq i64 %count, 0
    br i1 %done, label %exit, label %body
  body:
    %next = sub nuw i64 %count, 1
    br label %loop
  exit:
    store i64 0, ptr %rcx, align 8
    ret i64 0
  )");
  InterpreterLLVMRefinementLimits Limits;
  Limits.NativeProof.Execution.MaxBlockVisits = 32;
  const auto Finite = P.check(R.Residual, IR, Limits);
  rejected(Finite, Stage::Native);
  EXPECT_EQ(Finite.Native.Proof.Status, Status::BudgetExceeded);

  InterpreterLLVMRefinementPlans Plans;
  LowIRLoopCutpoint NativeCut;
  NativeCut.OriginalAddress = Entry;
  NativeCut.UseEntryPrefix = true;
  unsigned Matches = 0;
  for (const auto &Origin : R.Origins)
    if (Origin.NativeInstruction.Address == Entry) {
      NativeCut.CandidateAddress = Origin.ResidualAddress;
      ++Matches;
    }
  ASSERT_EQ(Matches, 1U);
  const LowIRLoopLocation RCX{LowIRLoopSpace::Register, x86reg::RCX, 8};
  const auto Count = NdVar::tmp(0, 8);
  NativeCut.Inputs = {{LowIRLoopSide::Original, RCX, Count}};
  NativeCut.OriginalState = NativeCut.CandidateState = {{RCX, Count}};
  NativeCut.Rank = {Count};
  Plans.Native = LowIRLoopRefinementPlan{{NativeCut}};

  auto Models =
      prepareInterpreterLLVMRefinement(R.Residual, IR, "model", P.Frame);
  ASSERT_TRUE(bool(Models)) << llvm::toString(Models.takeError());
  // Align the guarded headers. A body cut after the nonzero branch would
  // describe a different transition boundary from the LLVM header.
  const auto Left =
      inferLowIRLoopRefinementPlan(Models->Residual.Function, Models->Contract,
                                   {}, {NativeCut.CandidateAddress});
  const auto Right =
      inferLowIRLoopRefinementPlan(Models->LLVM.Function, Models->Contract);
  ASSERT_TRUE(Left.inferred()) << Left.Diagnostic;
  ASSERT_TRUE(Right.inferred()) << Right.Diagnostic;
  ASSERT_EQ(Left.Plan->Cutpoints.size(), 1U);
  ASSERT_EQ(Right.Plan->Cutpoints.size(), 1U);
  const auto &A = Left.Plan->Cutpoints.front();
  const auto &B = Right.Plan->Cutpoints.front();
  ASSERT_EQ(A.Rank.size(), 1U);
  ASSERT_EQ(B.Rank.size(), 1U);
  LowIRLoopCutpointPair Pair{A.OriginalAddress, B.OriginalAddress, {}};
  for (const auto &I : A.Inputs)
    for (const auto &J : B.Inputs)
      if (I.Side == LowIRLoopSide::Original && I.Temporary == A.Rank.front() &&
          J.Side == LowIRLoopSide::Original && J.Temporary == B.Rank.front())
        Pair.SharedInputs.push_back({I.Location, J.Location});
  ASSERT_EQ(Pair.SharedInputs.size(), 1U);
  auto Paired = pairLowIRLoopRefinementPlans(*Left.Plan, *Right.Plan, {Pair});
  ASSERT_TRUE(bool(Paired)) << llvm::toString(Paired.takeError());
  Plans.LLVM = std::move(*Paired);
  const auto Good = P.check(R.Residual, IR, {}, Plans);
  ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
  EXPECT_EQ(Good.Certificate->Native.Relation.Scope,
            LowIRRefinementScope::InductiveNativeToLowIRLoops);
  EXPECT_EQ(Good.Certificate->LLVM.Scope,
            LowIRRefinementScope::InductiveLowIRLoops);
  EXPECT_GT(Good.Native.Proof.RankingChecks, 0U);
  EXPECT_GT(Good.LLVM.RankingChecks, 0U);

  for (bool Native : {true, false}) {
    auto Bad = Plans;
    (Native ? Bad.Native : Bad.LLVM)->Cutpoints.front().Rank = {
        NdVar::scalar(0, 8)};
    rejected(P.check(R.Residual, IR, {}, Bad),
             Native ? Stage::Native : Stage::LLVM);
  }
  auto Changed = IR;
  Changed.replace(Changed.find("%count, 1"), 9, "%count, 2");
  rejected(P.check(R.Residual, Changed, {}, Plans), Stage::LLVM);
}
} // namespace neverd::analysis::llvm_refinement_test
