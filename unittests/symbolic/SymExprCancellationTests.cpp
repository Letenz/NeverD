//===- SymExprCancellationTests.cpp - Exact shared-word identities -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Checks exact modular cancellation and bounded bitwise absorption.

#include "gtest/gtest.h"

#include "neverd/solver/BitVectorSolver.h"
#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/STLExtras.h"

#include <array>
#include <random>
#include <string>
#include <vector>

using namespace neverd::symbolic;
using namespace neverd::solver;

namespace {
llvm::APInt word(std::mt19937_64 &Random, unsigned Width) {
  llvm::APInt Value(Width, 0);
  for (unsigned Bit = 0; Bit < Width; Bit += 64)
    Value |= llvm::APInt(Width, Random()).shl(Bit);
  return Value;
}
} // namespace

TEST(SymExprCancellation, SharedSumsCancelOpaqueValuesAtEveryWidth) {
  for (unsigned Width : {1u, 3u, 8u, 32u, 64u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto Z = C.mkVar("z", Width);
    auto Common = C.mkMul(X, Y);
    auto A = C.mkAdd({Common, Y, C.mkConst(Width, 9)});
    auto B = C.mkAdd({Common, Z, C.mkConst(Width, 21)});
    auto Difference = C.mkSub(A, B);
    llvm::SmallVector<uint32_t, 4> Variables;
    C.collectVars(Difference, Variables);
    EXPECT_FALSE(llvm::is_contained(Variables, C.varId(X)));
    EXPECT_EQ(C.mkSub(A, A), C.mkZero(Width));
    auto Unequal = C.mkSub(C.mkAdd(C.mkMul(C.mkConst(Width, 3), X), Y),
                           C.mkAdd(C.mkMul(C.mkConst(Width, 2), X), Z));
    std::mt19937_64 Random(19);
    for (unsigned I = 0; I != 256; ++I) {
      std::array<llvm::APInt, 3> V{word(Random, Width), word(Random, Width),
                                   word(Random, Width)};
      const auto Product = V[0] * V[1];
      const auto Want = (Product + V[1] + llvm::APInt(Width, 9)) -
                        (Product + V[2] + llvm::APInt(Width, 21));
      EXPECT_EQ(C.eval(Difference, V), Want);
      EXPECT_EQ(C.eval(Unequal, V), V[0] + V[1] - V[2]);
    }
  }
}

TEST(SymExprCancellation, CoveredXorTermsCannotAddBitsToTheirCover) {
  for (unsigned Width : {1u, 3u, 8u, 32u, 64u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto Z = C.mkVar("z", Width);
    auto Cover = C.mkAdd(X, C.mkConst(Width, 5));
    auto One = C.mkOr(Cover, C.mkXor(Cover, C.mkAnd(Cover, Y)));
    auto Many = C.mkOr(C.mkXor(C.mkAnd(Cover, Y), C.mkAnd(Z, Cover)), Cover);
    EXPECT_EQ(One, Cover);
    EXPECT_EQ(Many, Cover);
    auto Uncovered = C.mkOr(Cover, C.mkXor(C.mkAnd(Cover, Y), Z));
    std::mt19937_64 Random(23);
    for (unsigned I = 0; I != 256; ++I) {
      std::array<llvm::APInt, 3> V{word(Random, Width), word(Random, Width),
                                   word(Random, Width)};
      const auto Base = V[0] + llvm::APInt(Width, 5);
      EXPECT_EQ(C.eval(One, V), Base | (Base ^ (Base & V[1])));
      EXPECT_EQ(C.eval(Many, V), Base | ((Base & V[1]) ^ (Base & V[2])));
      EXPECT_EQ(C.eval(Uncovered, V), Base | ((Base & V[1]) ^ V[2]));
    }
  }
}

TEST(SymExprCancellation, ExhaustiveNarrowMixedOperandsRemainExact) {
  SymContext C;
  auto X = C.mkVar("x", 3), Y = C.mkVar("y", 3), Z = C.mkVar("z", 3);
  auto A = C.mkAdd(X, Y), B = C.mkAdd(X, Z);
  const std::array<SymRef, 4> Exprs{C.mkSub(A, B), C.mkSub(X, B),
                                    C.mkOr(X, C.mkXor(X, C.mkAnd(X, Y))),
                                    C.mkOr({X, C.mkXor(C.mkAnd(X, Y), Z), Y})};
  for (uint64_t AVal = 0; AVal != 8; ++AVal)
    for (uint64_t BVal = 0; BVal != 8; ++BVal)
      for (uint64_t CVal = 0; CVal != 8; ++CVal) {
        const std::array<uint64_t, 3> Values{AVal, BVal, CVal};
        const std::array<uint64_t, 4> Want{
            (AVal + BVal - (AVal + CVal)) & 7, (AVal - (AVal + CVal)) & 7,
            AVal | (AVal ^ (AVal & BVal)),
            AVal | ((AVal & BVal) ^ CVal) | BVal};
        for (unsigned I = 0; I != Exprs.size(); ++I)
          EXPECT_EQ(C.evalU64(Exprs[I], Values), Want[I]);
      }
}

TEST(SymExprCancellation, AdjacentIndexedBytesSeparateWithinSmallSolverBudget) {
  SymContext C;
  auto Root = C.mkVar("root", 64), Index = C.mkVar("index", 16);
  auto Current = C.mkAdd(Root, C.mkConst(64, uint64_t{0} - 80));
  auto Address = C.mkAdd(
      C.mkOr(Current, C.mkXor(Current, C.mkAnd(Current, C.mkZExt(Index, 64)))),
      C.mkZExt(Index, 64));
  auto Cell = C.mkAdd(Root, C.mkConst(64, uint64_t{0} - 88));
  auto Overlap = C.mkOr(C.mkUlt(C.mkSub(Address, Cell), C.mkConst(64, 8)),
                        C.mkUlt(C.mkSub(Cell, Address), C.mkConst(64, 1)));
  SolverOptions O;
  O.Blast.MaxGates = 512;
  O.Sat.MaxConflicts = 1000;
  O.Sat.MaxPropagations = 20000;
  O.Sat.MaxWatchVisits = 200000;
  BitVectorSolver S(C, O);
  ASSERT_TRUE(S.assertTrue(Overlap));
  EXPECT_EQ(S.check(), SatResult::Unsat);
  for (uint64_t R : {uint64_t{0}, uint64_t{7}, uint64_t{88},
                     uint64_t{0x80000000}, UINT64_MAX - 7, UINT64_MAX})
    for (uint64_t I : {uint64_t{0}, uint64_t{1}, uint64_t{7}, uint64_t{8},
                       uint64_t{32768}, uint64_t{65535}}) {
      bool Want = false;
      for (unsigned B = 0; B != 8; ++B)
        Want |= R - 80 + I == R - 88 + B;
      EXPECT_EQ(C.evalU64(Overlap, std::array<uint64_t, 2>{R, I}), Want);
    }
  auto Aliasing = C.mkUlt(C.mkSub(C.mkAdd(Cell, C.mkZExt(Index, 64)), Cell),
                          C.mkConst(64, 8));
  BitVectorSolver Counterexample(C, O);
  ASSERT_TRUE(Counterexample.assertTrue(Aliasing));
  ASSERT_EQ(Counterexample.check(), SatResult::Sat);
  EXPECT_FALSE(
      C.eval(Aliasing, Counterexample.model().asVarValues(C)).isZero());
}

TEST(SymExprCancellation, ExhaustedSubsetWorkRetainsUncoveredBits) {
  SymContext C;
  llvm::SmallVector<SymRef, 32> Covers;
  for (unsigned I = 0; I != 300; ++I)
    Covers.push_back(C.mkVar("cover" + std::to_string(I), 64));
  auto Uncovered = C.mkVar("uncovered", 64);
  auto Term = C.mkXor(C.mkAnd(Covers[0], Covers[1]), Uncovered);
  Covers.push_back(Term);
  auto Result = C.mkOr(Covers);
  std::vector<uint64_t> Values(301, 0);
  Values.back() = 0x8000000000000001ULL;
  EXPECT_EQ(C.evalU64(Result, Values), Values.back());
}
