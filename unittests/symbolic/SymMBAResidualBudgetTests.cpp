//===- SymMBAResidualBudgetTests.cpp - Bounded high-degree residuals
//--------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent modular oracles for shared powers and sparse nonlinear sums.

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include "neverd/symbolic/SymParse.h"

#include "llvm/ADT/SmallString.h"

#include <array>
#include <random>
#include <string>

using namespace neverd::symbolic;

namespace {

std::string residualText(unsigned Width, unsigned Degree) {
  const char *Words[] = {"((x+y)^z)", "((y*z)|w)", "((x+z)&(y|v))", "(x&~y)",
                         "(z|v)",     "(y^w)",     "~(x&z)",        "(x|v)"};
  const char *Axes[] = {"z", "x", "y", "w", "v"};
  // Retain the interning order of a coefficient-perturbed sparse product
  // whose best completed result used to be lost when later work ran out.
  const unsigned Corners[] = {30, 25, 1,  29, 20, 6,  16, 24, 5,  21,
                              15, 4,  10, 9,  28, 13, 17, 11, 12, 22,
                              8,  18, 14, 19, 2,  27, 7,  23, 26};
  std::string Text;
  for (unsigned Corner : Corners) {
    llvm::APInt Coefficient(Width, 182, false, true);
    if (Corner == 1)
      Coefficient += llvm::APInt::getSignMask(Width);
    llvm::SmallString<96> Number;
    Coefficient.toStringUnsigned(Number);
    if (!Text.empty())
      Text += "+";
    Text += "(" + std::string(Number) + "*";
    for (unsigned I = 0; I != Degree; ++I) {
      if (I)
        Text += "*";
      if (I < 5) {
        Text += "(" + std::string(Words[I]) + "&";
        if (Corner & (1u << (4 - I)))
          Text += "~";
        Text += std::string(Axes[I]) + ")";
      } else {
        Text += Words[I % 8];
      }
    }
    Text += ")";
  }
  return Text + "+(x^y)+2*(x&y)+19*x*y*z";
}

llvm::APInt residualOracle(const std::array<llvm::APInt, 5> &V,
                           unsigned Degree) {
  const auto &[X, Y, Z, W, U] = V;
  unsigned Width = X.getBitWidth();
  std::array<llvm::APInt, 8> Words{(X + Y) ^ Z, (Y * Z) | W, (X + Z) & (Y | U),
                                   X & ~Y,      Z | U,       Y ^ W,
                                   ~(X & Z),    X | U};
  std::array<llvm::APInt, 5> Axes{Z, X, Y, W, U};
  auto Product = [&](int Corner) {
    llvm::APInt R(Width, 1);
    for (unsigned I = 0; I != Degree; ++I) {
      auto Word = Words[I % 8];
      if (Corner >= 0 && I < 5)
        Word &= Corner & (1 << (4 - I)) ? ~Axes[I] : Axes[I];
      R *= Word;
    }
    return R;
  };
  // Closed product minus the three missing corners, with one coefficient
  // disturbance. This does not reevaluate the generated sum term by term.
  return llvm::APInt(Width, 182, false, true) *
             (Product(-1) - Product(0) - Product(3) - Product(31)) +
         llvm::APInt::getSignMask(Width) * Product(1) + X + Y +
         llvm::APInt(Width, 19, false, true) * X * Y * Z;
}

void checkResidual(SymContext &C, SymRef Input, SymRef Output, unsigned Width,
                   unsigned Degree) {
  std::mt19937_64 Random(0x524553494455414c);
  for (unsigned Sample = 0; Sample != 128; ++Sample) {
    std::array<llvm::APInt, 5> V;
    for (auto &Word : V) {
      Word = llvm::APInt(Width, 0);
      for (unsigned Bit = 0; Bit < Width; Bit += 64)
        Word |= llvm::APInt(Width, Random(), false, true).shl(Bit);
      if (Sample < 4)
        Word = Sample == 0   ? llvm::APInt(Width, 0)
               : Sample == 1 ? llvm::APInt::getAllOnes(Width)
               : Sample == 2 ? llvm::APInt::getSignMask(Width)
                             : llvm::APInt(Width, 1);
    }
    llvm::SmallVector<llvm::APInt, 5> Values;
    for (unsigned I = 0; I != C.numVars(); ++I) {
      auto Name = C.varInfo(I).Name;
      Values.push_back(V[Name == "x"   ? 0
                         : Name == "y" ? 1
                         : Name == "z" ? 2
                         : Name == "w" ? 3
                                       : 4]);
    }
    auto Want = residualOracle(V, Degree);
    ASSERT_EQ(C.eval(Input, Values), Want);
    ASSERT_EQ(C.eval(Output, Values), Want);
  }
}

TEST(SymMBAResidualBudget, RetainsLinearTailInCompletedCandidate) {
  for (unsigned Width : {3u, 8u, 32u, 64u, 257u}) {
    SymContext C;
    auto Input = parseSymExpr(C, residualText(Width, 32), Width);
    ASSERT_TRUE(Input.ok());
    MBAOptions Options;
    Options.VerifySamples = 0;
    auto Result = simplifyMBADeep(C, Input.Root, Options);
    EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
    EXPECT_LE(Result.Work, Options.MaxWork);
    if (Width == 64)
      EXPECT_LE(Result.SizeAfter, 286u);
    checkResidual(C, Input.Root, Result.Expr, Width, 32);
  }
}

TEST(SymMBAResidualBudget, RefusalAndPartialSearchKeepSparseProductsExact) {
  for (size_t Limit : {0u, 1u, 128u, 4096u, 32768u, 65535u, 65536u}) {
    SymContext C;
    auto Input = parseSymExpr(C, residualText(64, 32), 64);
    ASSERT_TRUE(Input.ok());
    MBAOptions Options;
    Options.MaxWork = Limit;
    Options.VerifySamples = 0;
    auto Result = simplifyMBADeep(C, Input.Root, Options);
    EXPECT_LE(Result.Work, Limit);
    EXPECT_LE(Result.SizeAfter, Result.SizeBefore);
    checkResidual(C, Input.Root, Result.Expr, 64, 32);
  }
}

TEST(SymMBAResidualBudget, BulkCommonPowersFitTheSameAllowance) {
  for (unsigned Width : {1u, 3u, 8u, 64u, 257u}) {
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto Z = C.mkVar("z", Width), W = C.mkVar("w", Width);
    auto A = C.mkVar("a", Width), B = C.mkVar("b", Width);
    llvm::SmallVector<SymRef, 64> Factors;
    for (unsigned I = 0; I != 16; ++I)
      Factors.append({X, Y, Z, W});
    auto Common = C.mkMul(Factors);
    auto Input = C.mkAdd(C.mkMul(Common, A), C.mkMul(Common, B));
    auto Expected = C.mkMul(Common, C.mkAdd(A, B));
    MBAOptions Options;
    Options.VerifySamples = 0;
    detail::WorkBudget Budget(6000);
    detail::SolveReport Report;
    auto Output = detail::solveArithmetic(C, Input, Options, Budget, Report);
    ASSERT_EQ(Output, Expected);
    EXPECT_EQ(Report.Evidence, MBAEvidence::Derivation);
    const size_t Required = Budget.used();
    for (size_t Limit : {size_t(0), Required - 1, Required}) {
      detail::WorkBudget Tight(Limit);
      detail::SolveReport Check;
      auto R = detail::solveArithmetic(C, Input, Options, Tight, Check);
      EXPECT_EQ(R, Limit == Required ? Expected : Input);
      EXPECT_LE(Tight.used(), Limit);
    }
    Options.MaxTableBytes = 128;
    detail::WorkBudget StorageBudget(6000);
    detail::SolveReport StorageReport;
    EXPECT_EQ(detail::solveArithmetic(C, Input, Options, StorageBudget,
                                      StorageReport),
              Input);
  }
}

TEST(SymMBAResidualBudget, DelayedCommonPowersPreserveCoefficientCancellation) {
  for (unsigned Width : {1u, 3u, 8u, 64u, 257u}) {
    SymContext C;
    auto Input = parseSymExpr(
        C, "13*x*x*y*y*z*z*w*w*(a+b)*(a-b)-13*x*x*y*y*z*z*w*w*a*a", Width);
    auto Expected = parseSymExpr(C, "-13*x*x*y*y*z*z*w*w*b*b", Width);
    ASSERT_TRUE(Input.ok());
    ASSERT_TRUE(Expected.ok());
    MBAOptions Options;
    Options.VerifySamples = 0;
    detail::WorkBudget Budget(Options.MaxWork);
    detail::SolveReport Report;
    EXPECT_EQ(detail::solveArithmetic(C, Input.Root, Options, Budget, Report),
              Expected.Root);
    EXPECT_EQ(Report.Evidence, MBAEvidence::Derivation);
  }
}

} // namespace
