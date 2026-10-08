//===- CompleteModelTests.cpp - Complete witness guards ===========//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/analysis/core/CompleteModel.h"
#include "gtest/gtest.h"
using namespace neverd;
using namespace neverd::symbolic;
using namespace neverd::solver;
using namespace neverd::analysis::complete_model;

TEST(CompleteModel, MissingVariableNeverBecomesZero) {
  SymContext C;
  auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  auto P = C.mkEq(X, C.mkConst(8, 3));
  auto Q = C.mkAnd(P, C.mkEq(Y, C.mkZero(8)));
  BitVectorModel M;
  M.set(C.varId(X), llvm::APInt(8, 3));
  EXPECT_EQ(verify(C, Q, M, 1000, 256).Answer, Verdict::MissingVariable);
  M.set(C.varId(Y), llvm::APInt(8, 0));
  EXPECT_EQ(verify(C, Q, M, 1000, 256).Answer, Verdict::Satisfied);
}
TEST(CompleteModel, WrongWidthIsNotCoerced) {
  SymContext C;
  auto X = C.mkVar("x", 8);
  auto Q = C.mkEq(X, C.mkConst(8, 3));
  BitVectorModel M;
  M.set(C.varId(X), llvm::APInt(16, 3));
  EXPECT_EQ(verify(C, Q, M, 1000, 256).Answer, Verdict::Invalid);
}
TEST(CompleteModel, FullQuestionIncludesEveryCondition) {
  SymContext C;
  auto X = C.mkVar("x", 8);
  auto Y = C.mkVar("y", 8);
  auto P = C.mkEq(X, C.mkConst(8, 3));
  auto Q = C.mkAnd(P, C.mkEq(Y, C.mkConst(8, 5)));
  BitVectorModel M;
  M.set(C.varId(X), llvm::APInt(8, 3));
  M.set(C.varId(Y), llvm::APInt(8, 4));
  EXPECT_EQ(verify(C, P, M, 1000, 256).Answer, Verdict::Satisfied);
  EXPECT_EQ(verify(C, Q, M, 1000, 256).Answer, Verdict::Refuted);
  M.set(C.varId(Y), llvm::APInt(8, 5));
  EXPECT_EQ(verify(C, Q, M, 1000, 256).Answer, Verdict::Satisfied);
  M.set(C.varId(X), llvm::APInt(8, 4));
  EXPECT_EQ(verify(C, Q, M, 1000, 256).Answer, Verdict::Refuted);
}
TEST(CompleteModel, BudgetAndWidthLimitsFailClosed) {
  SymContext C;
  auto X = C.mkVar("x", 65);
  auto Q = C.mkEq(X, C.mkConst(65, 7));
  BitVectorModel M;
  M.set(C.varId(X), llvm::APInt(65, 7));
  auto R = verify(C, Q, M, 1000, 128);
  ASSERT_EQ(R.Answer, Verdict::Satisfied);
  EXPECT_EQ(verify(C, Q, M, R.Words, 128).Answer, Verdict::Satisfied);
  EXPECT_EQ(verify(C, Q, M, R.Words - 1, 128).Answer, Verdict::BudgetExceeded);
  EXPECT_EQ(verify(C, Q, M, 1000, 64).Answer, Verdict::Invalid);
  EXPECT_EQ(verify(C, Q, M, 0, 128).Answer, Verdict::BudgetExceeded);
}
TEST(CompleteModel, ConstantsAndInvalidRoots) {
  SymContext C;
  BitVectorModel M;
  EXPECT_EQ(verify(C, C.mkConst(1, 1), M, 100, 256).Answer, Verdict::Satisfied);
  EXPECT_EQ(verify(C, C.mkZero(1), M, 100, 256).Answer, Verdict::Refuted);
  EXPECT_EQ(verify(C, {}, M, 100, 256).Answer, Verdict::Invalid);
  EXPECT_EQ(verify(C, SymRef(100000), M, 100, 256).Answer, Verdict::Invalid);
  EXPECT_EQ(verify(C, C.mkConst(8, 1), M, 100, 256).Answer, Verdict::Invalid);
}
TEST(CompleteModel, IndependentFourBitArithmeticMatrix) {
  SymContext C;
  auto X = C.mkVar("x", 4), Y = C.mkVar("y", 4);
  auto Sum = C.mkAdd(C.mkMul(X, C.mkConst(4, 3)), C.mkXor(Y, C.mkConst(4, 5)));
  auto Other = C.mkAnd(C.mkOr(X, Y), C.mkConst(4, 7));
  for (unsigned Xv = 0; Xv < 16; ++Xv)
    for (unsigned Yv = 0; Yv < 16; ++Yv) {
      BitVectorModel M;
      M.set(C.varId(X), llvm::APInt(4, Xv));
      M.set(C.varId(Y), llvm::APInt(4, Yv));
      for (unsigned K = 0; K < 4; ++K) {
        const auto Want = ((3 * Xv + (Yv ^ 5)) + K) & 15;
        auto Q = C.mkAnd(C.mkEq(Sum, C.mkConst(4, Want)),
                         C.mkUlt(Other, C.mkConst(4, 6)));
        const bool Expected = K == 0 && ((Xv | Yv) & 7) < 6;
        EXPECT_EQ(verify(C, Q, M, 1000, 256).Answer,
                  Expected ? Verdict::Satisfied : Verdict::Refuted);
      }
    }
}
TEST(CompleteModel, SeparateSatPartsAreNotCombined) {
  SymContext C;
  auto X = C.mkVar("x", 4);
  auto A = C.mkEq(X, C.mkConst(4, 1)), B = C.mkEq(X, C.mkConst(4, 2));
  BitVectorSolver SA(C), SB(C);
  SA.assertTrue(A);
  SB.assertTrue(B);
  ASSERT_EQ(SA.check(), SatResult::Sat);
  ASSERT_EQ(SB.check(), SatResult::Sat);
  auto Q = C.mkAnd(A, B);
  EXPECT_EQ(verify(C, Q, SA.model(), 1000, 256).Answer, Verdict::Refuted);
  EXPECT_EQ(verify(C, Q, SB.model(), 1000, 256).Answer, Verdict::Refuted);
}

TEST(CompleteModel, MalformedOperandAndOperatorShapeFailBeforeEvaluation) {
  SymContext C;
  auto Wide = C.mkVar("wide", 16), X = C.mkVar("x", 8);
  auto Q = C.mkEq(X, C.mkConst(8, 3));
  BitVectorModel M;
  M.set(C.varId(X), llvm::APInt(8, 3));
  auto &Operand = const_cast<SymRef &>(C.operands(Q)[0]);
  auto Original = Operand;
  for (auto Invalid : {SymRef{}, SymRef(0xfffffffe), Q, Wide}) {
    Operand = Invalid;
    EXPECT_EQ(verify(C, Q, M, 1000, 256).Answer, Verdict::Invalid);
    Operand = Original;
  }
}
