//===- CnfEncoderTests.cpp - Gate definitions and folding -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Checks the encoder against the truth tables of the gates it claims to
/// build, by forcing every combination of its inputs and reading the result
/// back out of a model.  Also pins the two behaviours the bit-blaster's size
/// depends on — that a constant operand folds a gate away, and that a gate
/// built twice is one gate — and the rule that makes a one-sided definition
/// safe, namely that asking later for the other side adds it.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/solver/BitVectorSolver.h"
#include "neverd/solver/CnfEncoder.h"
#include "neverd/solver/SatSolver.h"
#include "neverd/solver/SatTypes.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <functional>
#include <memory>

using namespace neverd::solver;

namespace {

/// Force \p A and \p B to fixed values and report what \p Gate is worth.
bool gateValue(SatSolver &S, SatLit A, bool ValueA, SatLit B, bool ValueB,
               SatLit Gate) {
  const SatLit Assumptions[] = {A.withPolarity(ValueA), B.withPolarity(ValueB)};
  EXPECT_EQ(S.solve(Assumptions), SatResult::Sat);
  return S.modelValue(Gate) == SatValue::True;
}

void checkTwoInputGate(
    const char *Name,
    const std::function<SatLit(CnfEncoder &, SatLit, SatLit)> &Build,
    const std::function<bool(bool, bool)> &Expected) {
  SatSolver S;
  CnfEncoder E(S);
  SatLit A = E.freshLit();
  SatLit B = E.freshLit();
  SatLit G = Build(E, A, B);

  for (unsigned Combination = 0; Combination < 4; ++Combination) {
    bool ValueA = (Combination & 1) != 0;
    bool ValueB = (Combination & 2) != 0;
    EXPECT_EQ(gateValue(S, A, ValueA, B, ValueB, G), Expected(ValueA, ValueB))
        << Name << " at " << ValueA << "," << ValueB;
  }
}

TEST(CnfEncoder, TwoInputGatesMatchTheirTruthTables) {
  checkTwoInputGate(
      "and", [](CnfEncoder &E, SatLit A, SatLit B) { return E.mkAnd(A, B); },
      [](bool A, bool B) { return A && B; });
  checkTwoInputGate(
      "or", [](CnfEncoder &E, SatLit A, SatLit B) { return E.mkOr(A, B); },
      [](bool A, bool B) { return A || B; });
  checkTwoInputGate(
      "xor", [](CnfEncoder &E, SatLit A, SatLit B) { return E.mkXor(A, B); },
      [](bool A, bool B) { return A != B; });
  checkTwoInputGate(
      "equiv",
      [](CnfEncoder &E, SatLit A, SatLit B) { return E.mkEquiv(A, B); },
      [](bool A, bool B) { return A == B; });
  checkTwoInputGate(
      "nand", [](CnfEncoder &E, SatLit A, SatLit B) { return ~E.mkAnd(A, B); },
      [](bool A, bool B) { return !(A && B); });
}

TEST(CnfEncoder, SelectionAndMajorityMatchTheirTruthTables) {
  SatSolver S;
  CnfEncoder E(S);
  SatLit A = E.freshLit();
  SatLit B = E.freshLit();
  SatLit C = E.freshLit();

  SatLit Selected = E.mkIte(A, B, C);
  SatLit Majority = E.mkMajority(A, B, C);
  SatLit Sum;
  SatLit Carry;
  E.mkFullAdder(A, B, C, Sum, Carry);

  for (unsigned Combination = 0; Combination < 8; ++Combination) {
    bool ValueA = (Combination & 1) != 0;
    bool ValueB = (Combination & 2) != 0;
    bool ValueC = (Combination & 4) != 0;

    const SatLit Assumptions[] = {
        A.withPolarity(ValueA), B.withPolarity(ValueB), C.withPolarity(ValueC)};
    ASSERT_EQ(S.solve(Assumptions), SatResult::Sat);

    unsigned Total = unsigned(ValueA) + unsigned(ValueB) + unsigned(ValueC);
    EXPECT_EQ(S.modelValue(Selected) == SatValue::True,
              ValueA ? ValueB : ValueC);
    EXPECT_EQ(S.modelValue(Majority) == SatValue::True, Total >= 2);
    EXPECT_EQ(S.modelValue(Sum) == SatValue::True, (Total & 1) != 0);
    EXPECT_EQ(S.modelValue(Carry) == SatValue::True, Total >= 2);
  }
}

TEST(CnfEncoder, ConstantOperandsFoldInsteadOfBuildingAGate) {
  SatSolver S;
  CnfEncoder E(S);
  SatLit A = E.freshLit();

  EXPECT_EQ(E.mkAnd(E.trueLit(), A), A);
  EXPECT_EQ(E.mkAnd(E.falseLit(), A), E.falseLit());
  EXPECT_EQ(E.mkOr(E.trueLit(), A), E.trueLit());
  EXPECT_EQ(E.mkOr(E.falseLit(), A), A);
  EXPECT_EQ(E.mkXor(E.falseLit(), A), A);
  EXPECT_EQ(E.mkXor(E.trueLit(), A), ~A);
  EXPECT_EQ(E.mkIte(E.trueLit(), A, ~A), A);
  EXPECT_EQ(E.mkIte(E.falseLit(), A, ~A), ~A);
  EXPECT_EQ(E.mkMajority(E.falseLit(), A, ~A), E.falseLit());

  // The algebraic identities matter as much as the constant ones, because
  // bit-blasting produces both in quantity.
  EXPECT_EQ(E.mkAnd(A, A), A);
  EXPECT_EQ(E.mkAnd(A, ~A), E.falseLit());
  EXPECT_EQ(E.mkOr(A, ~A), E.trueLit());
  EXPECT_EQ(E.mkXor(A, A), E.falseLit());
  EXPECT_EQ(E.mkXor(A, ~A), E.trueLit());

  EXPECT_EQ(E.numGates(), 0u);
}

TEST(CnfEncoder, EqualGatesAreOneGate) {
  SatSolver S;
  CnfEncoder E(S);
  SatLit A = E.freshLit();
  SatLit B = E.freshLit();

  SatLit First = E.mkAnd(A, B);
  EXPECT_EQ(E.numGates(), 1u);

  // Operand order is not part of a conjunction's identity.
  EXPECT_EQ(E.mkAnd(B, A), First);
  EXPECT_EQ(E.numGates(), 1u);

  // A disjunction is the same gate seen through complemented operands, which
  // is what lets a formula written either way share one variable.
  EXPECT_EQ(E.mkOr(~A, ~B), ~First);
  EXPECT_EQ(E.numGates(), 1u);

  // Complements on an exclusive or move to its result, so these are one gate.
  SatLit Parity = E.mkXor(A, B);
  EXPECT_EQ(E.mkXor(~A, B), ~Parity);
  EXPECT_EQ(E.mkXor(~A, ~B), Parity);
  EXPECT_EQ(E.numGates(), 2u);
}

TEST(CnfEncoder, WideExclusiveOrStaysLinear) {
  SatSolver S;
  CnfEncoder E(S);

  llvm::SmallVector<SatLit, 8> Inputs;
  for (unsigned I = 0; I < 8; ++I)
    Inputs.push_back(E.freshLit());

  SatLit Parity = E.mkXor(Inputs);

  // A single node would need one clause per assignment of its inputs, so a
  // wide parity has to become a chain instead.
  EXPECT_LT(S.numClauses(), 64u);

  for (unsigned Combination = 0; Combination < 256; ++Combination) {
    llvm::SmallVector<SatLit, 8> Assumptions;
    unsigned Ones = 0;
    for (unsigned I = 0; I < 8; ++I) {
      bool Bit = ((Combination >> I) & 1) != 0;
      Ones += unsigned(Bit);
      Assumptions.push_back(Inputs[I].withPolarity(Bit));
    }
    ASSERT_EQ(S.solve(Assumptions), SatResult::Sat);
    EXPECT_EQ(S.modelValue(Parity) == SatValue::True, (Ones & 1) != 0);
  }
}

TEST(CnfEncoder, CollidingGateIdentitiesStayDistinctAfterGrowth) {
  for (auto Polarity : {GatePolarity::Positive, GatePolarity::Negative}) {
    SatSolver S;
    CnfEncoder E(S);
    llvm::SmallVector<SatLit, 72> Inputs;
    for (unsigned I = 0; I != 69; ++I)
      Inputs.push_back(E.freshLit());

    // These different input pairs have the same complete identity hash.
    // They must remain separate gates, including after a table expansion.
    const auto First = E.mkAnd(Inputs[1], Inputs[68], Polarity);
    const auto Second = E.mkAnd(Inputs[2], Inputs[3], Polarity);
    ASSERT_NE(First, Second);
    for (unsigned I = 0; I + 2 < Inputs.size(); ++I) {
      E.mkXor(Inputs[I], Inputs[I + 1]);
      E.mkMajority(Inputs[I], Inputs[I + 1], Inputs[I + 2]);
    }
    const auto Gates = E.numGates();
    EXPECT_EQ(E.mkAnd(Inputs[68], Inputs[1]), First);
    EXPECT_EQ(E.mkAnd(Inputs[3], Inputs[2]), Second);
    EXPECT_EQ(E.numGates(), Gates);

    for (unsigned Assignment = 0; Assignment != 16; ++Assignment) {
      SCOPED_TRACE(Assignment);
      llvm::SmallVector<SatLit, 5> Assumptions;
      unsigned Bit = 0;
      for (unsigned I : {1U, 68U, 2U, 3U})
        Assumptions.push_back(
            Inputs[I].withPolarity((Assignment >> Bit++) & 1));
      const bool FirstValue = (Assignment & 3) == 3;
      const bool SecondValue = (Assignment & 12) == 12;
      ASSERT_EQ(S.solve(Assumptions), SatResult::Sat);
      EXPECT_EQ(S.modelValue(First) == SatValue::True, FirstValue);
      EXPECT_EQ(S.modelValue(Second) == SatValue::True, SecondValue);
      Assumptions.push_back(First.withPolarity(!FirstValue));
      EXPECT_EQ(S.solve(Assumptions), SatResult::Unsat);
      Assumptions.back() = Second.withPolarity(!SecondValue);
      EXPECT_EQ(S.solve(Assumptions), SatResult::Unsat);
    }
  }
}

TEST(CnfEncoder, PlacementFingerprintCollisionsOutliveSourceAndGrowth) {
  // These conjunctions have different 64-bit identity hashes and the same
  // 32-bit placement fingerprint. Exact operands must decide their identity.
  const unsigned FirstIndices[] = {4, 56, 65, 90, 91, 93};
  const unsigned SecondIndices[] = {10, 24, 27, 40, 53, 67};
  for (auto Polarity : {GatePolarity::Positive, GatePolarity::Negative}) {
    neverd::symbolic::SymContext Context;
    auto Source = std::make_unique<BitVectorSolver>(Context);
    auto &E = Source->blaster().encoder();
    llvm::SmallVector<SatLit, 96> Inputs;
    for (unsigned I = 0; I != 96; ++I)
      Inputs.push_back(E.freshLit());
    llvm::SmallVector<SatLit, 6> FirstTerms, SecondTerms;
    for (unsigned I : FirstIndices)
      FirstTerms.push_back(Inputs[I]);
    for (unsigned I : SecondIndices)
      SecondTerms.push_back(Inputs[I]);
    const auto First = E.mkAnd(FirstTerms, Polarity);
    const auto Second = E.mkAnd(SecondTerms, Polarity);
    ASSERT_NE(First, Second);

    for (unsigned I = 0; I != Inputs.size(); ++I)
      for (unsigned Offset : {1U, 7U, 23U})
        E.mkXor(Inputs[I], Inputs[(I + Offset) % Inputs.size()]);
    const auto Gates = E.numGates();
    const auto SourceClauses = Source->sat().numClauses();
    auto Copy = Source->cloneEncoding();
    ASSERT_TRUE(Copy);
    auto &Copied = Copy->blaster().encoder();
    std::reverse(FirstTerms.begin(), FirstTerms.end());
    std::reverse(SecondTerms.begin(), SecondTerms.end());
    EXPECT_EQ(Copied.mkAnd(FirstTerms), First);
    EXPECT_EQ(Copied.mkAnd(SecondTerms), Second);
    EXPECT_EQ(Copied.numGates(), Gates);
    EXPECT_EQ(Source->sat().numClauses(), SourceClauses);
    Source.reset();
    EXPECT_EQ(Copied.mkAnd(FirstTerms), First);
    EXPECT_EQ(Copied.mkAnd(SecondTerms), Second);

    for (unsigned Assignment = 0; Assignment != 4096; ++Assignment) {
      SCOPED_TRACE(Assignment);
      llvm::SmallVector<SatLit, 97> Assumptions;
      for (auto Input : Inputs)
        Assumptions.push_back(~Input);
      bool FirstValue = true, SecondValue = true;
      for (unsigned I = 0; I != 6; ++I) {
        const bool A = (Assignment >> I) & 1;
        const bool B = (Assignment >> (I + 6)) & 1;
        Assumptions[FirstIndices[I]] = Inputs[FirstIndices[I]].withPolarity(A);
        Assumptions[SecondIndices[I]] =
            Inputs[SecondIndices[I]].withPolarity(B);
        FirstValue &= A;
        SecondValue &= B;
      }
      ASSERT_EQ(Copy->sat().solve(Assumptions), SatResult::Sat);
      EXPECT_EQ(Copy->sat().modelValue(First) == SatValue::True, FirstValue);
      EXPECT_EQ(Copy->sat().modelValue(Second) == SatValue::True, SecondValue);
      Assumptions.push_back(First.withPolarity(!FirstValue));
      EXPECT_EQ(Copy->sat().solve(Assumptions), SatResult::Unsat);
      Assumptions.back() = Second.withPolarity(!SecondValue);
      EXPECT_EQ(Copy->sat().solve(Assumptions), SatResult::Unsat);
    }
  }
}

TEST(CnfEncoder, SharingAndPolarityCompletionSurviveRepeatedGrowth) {
  SatSolver S;
  CnfEncoder E(S);
  llvm::SmallVector<SatLit, 10> Inputs;
  for (unsigned I = 0; I != 10; ++I)
    Inputs.push_back(E.freshLit().withPolarity((I & 1) == 0));

  struct GateCase {
    unsigned A, B, C;
    SatLit And, Xor, Selected, Majority;
  };
  llvm::SmallVector<GateCase, 128> Cases;
  for (unsigned I = 0; I != Inputs.size(); ++I)
    for (unsigned J = I + 1; J != Inputs.size(); ++J)
      for (unsigned K = J + 1; K != Inputs.size(); ++K) {
        const SatLit Terms[] = {Inputs[I], Inputs[J], Inputs[K]};
        const auto Polarity =
            (I + J + K) & 1 ? GatePolarity::Positive : GatePolarity::Negative;
        Cases.push_back({I, J, K, E.mkAnd(Terms, Polarity), E.mkXor(Terms),
                         E.mkIte(Terms[2], Terms[0], Terms[1]),
                         E.mkMajority(Terms[0], Terms[1], Terms[2])});
      }
  ASSERT_EQ(E.numGates(), 480u);

  // Revisit every old gate in reverse creation order. Complete each AND
  // definition and ask for equivalent spellings without making new gates.
  for (auto It = Cases.rbegin(); It != Cases.rend(); ++It) {
    const auto &G = *It;
    const SatLit Reversed[] = {Inputs[G.C], Inputs[G.B], Inputs[G.A]};
    EXPECT_EQ(E.mkAnd(Reversed), G.And);
    EXPECT_EQ(E.mkXor(Reversed), G.Xor);
    EXPECT_EQ(E.mkIte(~Inputs[G.C], Inputs[G.B], Inputs[G.A]), G.Selected);
    EXPECT_EQ(E.mkMajority(Reversed[0], Reversed[1], Reversed[2]), G.Majority);
  }
  EXPECT_EQ(E.numGates(), 480u);

  for (unsigned Assignment = 0; Assignment != 1024; ++Assignment) {
    SCOPED_TRACE(Assignment);
    llvm::SmallVector<SatLit, 10> Assumptions;
    for (unsigned I = 0; I != Inputs.size(); ++I)
      Assumptions.push_back(Inputs[I].withPolarity((Assignment >> I) & 1));
    ASSERT_EQ(S.solve(Assumptions), SatResult::Sat);
    for (const auto &G : Cases) {
      const bool A = (Assignment >> G.A) & 1;
      const bool B = (Assignment >> G.B) & 1;
      const bool C = (Assignment >> G.C) & 1;
      EXPECT_EQ(S.modelValue(G.And) == SatValue::True, A && B && C);
      EXPECT_EQ(S.modelValue(G.Xor) == SatValue::True, A ^ B ^ C);
      EXPECT_EQ(S.modelValue(G.Selected) == SatValue::True, C ? A : B);
      EXPECT_EQ(S.modelValue(G.Majority) == SatValue::True,
                unsigned(A) + unsigned(B) + unsigned(C) >= 2);
    }
  }
}

TEST(CnfEncoder, OperandPermutationsKeepSharingAndTruthTables) {
  for (unsigned Count : {2U, 3U, 5U}) {
    SCOPED_TRACE(Count);
    for (unsigned Signs = 0; Signs < (1U << Count); ++Signs) {
      SCOPED_TRACE(Signs);
      SatSolver S;
      CnfEncoder E(S);
      llvm::SmallVector<SatLit, 8> Variables, Inputs;
      llvm::SmallVector<unsigned, 8> Order;
      for (unsigned I = 0; I != Count; ++I) {
        Variables.push_back(E.freshLit());
        Inputs.push_back(Variables.back().withPolarity(!(Signs & (1U << I))));
        Order.push_back(I);
      }
      const auto And = E.mkAnd(Inputs), Xor = E.mkXor(Inputs);
      const auto Majority =
          Count == 3 ? E.mkMajority(Inputs[0], Inputs[1], Inputs[2]) : SatLit{};
      const auto Gates = E.numGates();
      do {
        llvm::SmallVector<SatLit, 8> Permuted;
        for (unsigned I : Order)
          Permuted.push_back(Inputs[I]);
        EXPECT_EQ(E.mkAnd(Permuted), And);
        EXPECT_EQ(E.mkXor(Permuted), Xor);
        if (Count == 3)
          EXPECT_EQ(E.mkMajority(Permuted[0], Permuted[1], Permuted[2]),
                    Majority);
      } while (std::next_permutation(Order.begin(), Order.end()));
      EXPECT_EQ(E.numGates(), Gates);

      for (unsigned Assignment = 0; Assignment < (1U << Count); ++Assignment) {
        SCOPED_TRACE(Assignment);
        llvm::SmallVector<SatLit, 8> Assumptions;
        unsigned Ones = 0;
        for (unsigned I = 0; I != Count; ++I) {
          const bool Bit = (Assignment & (1U << I)) != 0;
          Assumptions.push_back(Variables[I].withPolarity(Bit));
          Ones += Bit != ((Signs & (1U << I)) != 0);
        }
        ASSERT_EQ(S.solve(Assumptions), SatResult::Sat);
        EXPECT_EQ(S.modelValue(And) == SatValue::True, Ones == Count);
        EXPECT_EQ(S.modelValue(Xor) == SatValue::True, (Ones & 1) != 0);
        if (Count == 3)
          EXPECT_EQ(S.modelValue(Majority) == SatValue::True, Ones >= 2);
      }
    }
  }
}

TEST(CnfEncoder, AOneSidedDefinitionIsCompletedWhenTheOtherSideIsAskedFor) {
  SatSolver S;
  CnfEncoder E(S);
  SatLit A = E.freshLit();
  SatLit B = E.freshLit();

  SatLit G = E.mkAnd(A, B, GatePolarity::Positive);

  // With only the half that makes forcing the gate true meaningful, the gate
  // does imply its operands...
  const SatLit Forced[] = {G};
  ASSERT_EQ(S.solve(Forced), SatResult::Sat);
  EXPECT_EQ(S.modelValue(A), SatValue::True);
  EXPECT_EQ(S.modelValue(B), SatValue::True);

  // ...but the operands do not yet imply the gate.
  const SatLit Loose[] = {A, B, ~G};
  EXPECT_EQ(S.solve(Loose), SatResult::Sat);

  // Asking for the same gate with both halves completes the definition rather
  // than building a second gate.
  EXPECT_EQ(E.mkAnd(A, B, GatePolarity::Both), G);
  EXPECT_EQ(E.numGates(), 1u);
  EXPECT_EQ(S.solve(Loose), SatResult::Unsat);
}

TEST(CnfEncoder, TheConstantLiteralBehavesLikeAnyOther) {
  SatSolver S;
  CnfEncoder E(S);

  EXPECT_TRUE(E.isConstant(E.trueLit()));
  EXPECT_TRUE(E.isConstant(E.falseLit()));
  EXPECT_EQ(E.constant(true), E.trueLit());
  EXPECT_EQ(E.constant(false), E.falseLit());
  EXPECT_FALSE(E.isConstant(E.freshLit()));

  ASSERT_EQ(S.solve(), SatResult::Sat);
  EXPECT_EQ(S.modelValue(E.trueLit()), SatValue::True);
  EXPECT_EQ(S.modelValue(E.falseLit()), SatValue::False);

  // Asserting the false literal is how an encoder reports that what it was
  // asked to build cannot hold.
  EXPECT_FALSE(E.assertTrue(E.falseLit()));
  EXPECT_EQ(S.solve(), SatResult::Unsat);
}

} // namespace
