//===- CompletedTargetFactsTests.cpp - Complete enumeration evidence ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../lib/analysis/core/CompletedTargetFacts.h"
#include "gtest/gtest.h"

#include <set>
using namespace neverd;
using namespace neverd::symbolic;
using namespace neverd::analysis::detail;
namespace {
using Answer = CompletedTargetFacts::Answer;
TEST(CompletedTargetFacts, OnlyACompleteSingletonCanEnterStorage) {
  SymContext C;
  auto X = C.mkVar("x", 4), Point = C.mkEq(X, C.mkConst(4, 3));
  FiniteDomainEncoding E(C, {});
  CompletedTargetFacts F(C, 64);
  uint64_t Q = 0;
  auto R = F.enumerate(E, Point, X, 1, 1, 10000, Q);
  EXPECT_EQ(R.Status, FiniteValueStatus::QueryBudgetExceeded);
  EXPECT_EQ(Q, 1U);
  EXPECT_EQ(F.size(), 0U);
  Q = 0;
  R = F.enumerate(E, Point, X, 1, 2, 10000, Q);
  ASSERT_EQ(R.Status, FiniteValueStatus::Complete);
  EXPECT_EQ(Q, 2U);
  EXPECT_EQ(F.size(), 1U);
  uint64_t Work = 100;
  EXPECT_EQ(F.proves(C, Point, Point, Work), Answer::Proved);
  Q = 0;
  R = F.enumerate(E, C.mkFalse(), X, 1, 10, 10000, Q);
  EXPECT_EQ(R.Status, FiniteValueStatus::Complete);
  EXPECT_TRUE(R.Tuples.empty());
  EXPECT_EQ(F.size(), 1U);
  Q = 0;
  R = F.enumerate(E, C.mkUlt(X, C.mkConst(4, 3)), X, 3, 10, 10000, Q);
  EXPECT_EQ(R.Status, FiniteValueStatus::Complete);
  EXPECT_EQ(R.Tuples.size(), 3U);
  EXPECT_EQ(F.size(), 1U);
  Q = 0;
  R = F.enumerate(E, C.mkTrue(), X, 1, 10, 10000, Q);
  EXPECT_EQ(R.Status, FiniteValueStatus::TooManyValues);
  EXPECT_EQ(F.size(), 1U);
}
TEST(CompletedTargetFacts, EveryStoredPremiseAndTheEntireValueAreRequired) {
  SymContext C;
  auto X = C.mkVar("x", 64), A = C.mkVar("a", 1), B = C.mkVar("b", 1);
  const uint64_t K = (uint64_t{1} << 63) | 5;
  auto E = C.mkEq(X, C.mkConst(64, K)), D = C.mkAnd(E, A);
  CompletedTargetFacts F(C, 64);
  FiniteDomainEncoding Encoding(C, {});
  uint64_t Q = 0;
  ASSERT_EQ(F.enumerate(Encoding, D, X, 1, 10, 10000, Q).Status,
            FiniteValueStatus::Complete);
  for (auto Domain : {D, C.mkAnd(D, B)}) {
    uint64_t Work = 1000;
    EXPECT_EQ(F.proves(C, Domain, E, Work), Answer::Proved);
  }
  for (auto Domain : {C.mkTrue(), E, A, C.mkOr(D, B), C.mkAnd(E, C.mkNot(A))}) {
    uint64_t Work = 1000;
    EXPECT_EQ(F.proves(C, Domain, E, Work), Answer::Unavailable);
  }
  uint64_t Work = 1000;
  EXPECT_EQ(F.proves(C, D, C.mkEq(X, C.mkConst(64, 5)), Work),
            Answer::Unavailable);
}
TEST(CompletedTargetFacts, ContextStorageAndInspectionBoundsRemainExact) {
  SymContext C, Other;
  auto X = C.mkVar("x", 4), D = C.mkEq(X, C.mkConst(4, 3));
  auto OX = Other.mkVar("x", 4), OD = Other.mkEq(OX, Other.mkConst(4, 3));
  ASSERT_EQ(X.index(), OX.index());
  ASSERT_EQ(D.index(), OD.index());
  CompletedTargetFacts F(C, 9), Empty(C, 0);
  FiniteDomainEncoding E(C, {}), OE(Other, {});
  uint64_t Q = 0;
  EXPECT_EQ(F.enumerate(OE, OD, OX, 1, 10, 10000, Q).Status,
            FiniteValueStatus::Invalid);
  EXPECT_EQ(Q, 0U);
  EXPECT_EQ(F.size(), 0U);
  for (unsigned I = 0; I != 5; ++I) {
    Q = 0;
    ASSERT_EQ(F.enumerate(E, D, X, 1, 10, 10000, Q).Status,
              FiniteValueStatus::Complete);
  }
  EXPECT_EQ(F.size(), 4U);
  EXPECT_LE(F.allocatedWords(), 9U);
  Q = 0;
  EXPECT_EQ(Empty.enumerate(E, D, X, 1, 10, 10000, Q).Status,
            FiniteValueStatus::Complete);
  EXPECT_EQ(Empty.size(), 0U);
  uint64_t Work = 1000;
  EXPECT_EQ(F.proves(Other, OD, OD, Work), Answer::Unavailable);
  Work = 1000;
  ASSERT_EQ(F.proves(C, D, D, Work), Answer::Proved);
  auto Used = 1000 - Work;
  ASSERT_GT(Used, 0U);
  Work = Used;
  EXPECT_EQ(F.proves(C, D, D, Work), Answer::Proved);
  EXPECT_EQ(Work, 0U);
  Work = Used - 1;
  EXPECT_EQ(F.proves(C, D, D, Work), Answer::BudgetExceeded);
}
TEST(CompletedTargetFacts, IndependentBooleanPremiseMatrix) {
  for (unsigned Mask = 0; Mask < 4; ++Mask) {
    SymContext C;
    auto X = C.mkVar("x", 3), A = C.mkVar("a", 1), B = C.mkVar("b", 1);
    auto E = C.mkEq(X, C.mkConst(3, 5));
    std::vector<SymRef> Parts{E};
    if (Mask & 1)
      Parts.push_back(A);
    if (Mask & 2)
      Parts.push_back(B);
    auto Stored = C.mkAnd(Parts);
    CompletedTargetFacts F(C, 64);
    FiniteDomainEncoding Encoding(C, {});
    uint64_t Q = 0;
    ASSERT_EQ(F.enumerate(Encoding, Stored, X, 1, 10, 10000, Q).Status,
              FiniteValueStatus::Complete);
    for (unsigned Current = 0; Current < 8; ++Current) {
      std::vector<SymRef> DomainParts;
      if (Current & 1)
        DomainParts.push_back(E);
      if (Current & 2)
        DomainParts.push_back(A);
      if (Current & 4)
        DomainParts.push_back(B);
      auto D = DomainParts.empty() ? C.mkTrue() : C.mkAnd(DomainParts);
      uint64_t Work = 1000;
      auto Result = F.proves(C, D, E, Work);
      bool Counterexample = false;
      for (unsigned V = 0; V < 8; ++V)
        for (bool Av : {false, true})
          for (bool Bv : {false, true}) {
            bool In = (!(Current & 1) || V == 5) && (!(Current & 2) || Av) &&
                      (!(Current & 4) || Bv);
            Counterexample |= In && V != 5;
          }
      if (Result == Answer::Proved)
        EXPECT_FALSE(Counterexample);
      EXPECT_EQ(Result == Answer::Proved, (Current & 1) &&
                                              (!(Mask & 1) || (Current & 2)) &&
                                              (!(Mask & 2) || (Current & 4)));
    }
  }
}
TEST(CompletedTargetFacts,
     ReducedEnumerationRetainsMultiplicityAndModularCarry) {
  // Enumerate one nonlinear modular cancellation once, then reuse only its
  // complete singleton fact inside a larger additive target.
  for (unsigned Width : {4U, 8U}) {
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width),
         Z = C.mkVar("z", Width), U = C.mkVar("u", Width);
    auto Domain = C.mkEq(X, Y);
    const uint64_t KnownValue = (1U << Width) - 2;
    auto Known = C.mkAdd(C.mkSub(C.mkMul(X, Z), C.mkMul(Y, Z)),
                         C.mkConst(Width, KnownValue));
    auto Low = C.mkAnd(U, C.mkConst(Width, 3));
    auto Target = C.mkAdd({Known, Low, C.mkConst(Width, (1U << Width) - 1)});
    CompletedTargetFacts F(C, 10000);
    FiniteDomainEncoding Full(C, {});
    uint64_t Q = 0;
    auto R = F.enumerate(Full, Domain, Known, 1, 1000, 100000, Q);
    ASSERT_EQ(R.Status, FiniteValueStatus::Complete);
    ASSERT_EQ(R.Tuples, (std::vector<std::vector<uint64_t>>{{KnownValue}}));
    solver::SolverOptions Options;
    Options.Blast.MaxGates = 16 * Width;
    FiniteDomainEncoding Limited(C, Options);
    Q = 0;
    auto Error = solver::BlastError::None;
    auto Direct = enumerateFiniteValues(Limited, Domain, {Target}, 4, 1000,
                                        100000, Q, Error);
    ASSERT_EQ(Direct.Status, FiniteValueStatus::Unknown) << Width;
    ASSERT_EQ(Error, solver::BlastError::TooManyGates) << Width;
    CompletedTargetFacts NoFacts(C, 10000);
    Q = 0;
    EXPECT_EQ(
        NoFacts.enumerate(Limited, Domain, Target, 4, 1000, 100000, Q).Status,
        FiniteValueStatus::Unknown);
    Q = 0;
    R = F.enumerate(Limited, Domain, Target, 4, 1000, 100000, Q);
    ASSERT_EQ(R.Status, FiniteValueStatus::Complete);
    const uint64_t Mask = (1U << Width) - 1;
    std::set<uint64_t> Expected;
    for (unsigned Xv = 0; Xv < (1U << Width); ++Xv)
      for (unsigned Uv = 0; Uv < 4; ++Uv)
        Expected.insert(((Xv * 3 - Xv * 3) + KnownValue + Uv + Mask) & Mask);
    std::set<uint64_t> Actual;
    for (const auto &T : R.Tuples) {
      ASSERT_EQ(T.size(), 1U);
      Actual.insert(T[0]);
    }
    EXPECT_EQ(Actual, Expected);
    const auto NeededQueries = Q;
    ASSERT_GT(NeededQueries, 0U);
    Q = 0;
    EXPECT_EQ(F.enumerate(Limited, Domain, Target, 4, NeededQueries, 100000, Q)
                  .Status,
              FiniteValueStatus::Complete);
    EXPECT_EQ(Q, NeededQueries);
    Q = 0;
    auto Short =
        F.enumerate(Limited, Domain, Target, 4, NeededQueries - 1, 100000, Q);
    EXPECT_EQ(Short.Status, FiniteValueStatus::QueryBudgetExceeded);
    EXPECT_TRUE(Short.Tuples.empty());
    Q = 0;
    EXPECT_NE(
        F.enumerate(Limited, C.mkTrue(), Target, 4, 1000, 100000, Q).Status,
        FiniteValueStatus::Complete);
    // With an extra independent occurrence the previous cancellation does not
    // eliminate that source. The complete result must include every value.
    auto Different = C.mkAdd(Target, C.mkMul(X, Z));
    Q = 0;
    EXPECT_NE(
        F.enumerate(Limited, Domain, Different, 4, 1000, 100000, Q).Status,
        FiniteValueStatus::Complete);
    Q = 0;
    auto Wide =
        F.enumerate(Full, Domain, Different, 1U << Width, 1000, 100000, Q);
    ASSERT_EQ(Wide.Status, FiniteValueStatus::Complete);
    EXPECT_EQ(Wide.Tuples.size(), 1U << Width);
  }
}
} // namespace
