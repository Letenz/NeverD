//===- SymMBAPrefixResidueTests.cpp - Low-prefix product proof oracles ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Check full-word product proofs through independently perturbed high bits.
/// Separate arithmetic oracles, nonzero controls and resource boundaries
/// protect the distinction between a zero prefix and a zero complete word.
///
//===----------------------------------------------------------------------===//

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include "neverd/solver/SymSynthVerifier.h"

#include <algorithm>
#include <array>
#include <bit>
#include <random>
#include <utility>
#include <vector>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

llvm::APInt word(unsigned Width, std::mt19937_64 &Random) {
  llvm::APInt Value(Width, Random());
  for (unsigned I = 64; I < Width; I += 64)
    Value |= llvm::APInt(Width, Random()).shl(I);
  return Value;
}

struct Fixture {
  SymRef Input;
  llvm::APInt Scale;
  std::vector<std::pair<llvm::APInt, llvm::APInt>> Factors;
  unsigned Bits;
  unsigned Kind;

  llvm::APInt oracle(const std::array<llvm::APInt, 5> &V) const {
    const unsigned Width = Scale.getBitWidth();
    auto Mask = llvm::APInt::getLowBitsSet(Width, Bits);
    auto Base = V[0] + 3 * V[1] + 5 * V[2];
    auto High = V[3].shl(Bits);
    auto Other = V[4].shl(Bits);
    auto Result = Scale;
    for (const auto &[A, B] : Factors) {
      auto F = A * (Kind == 3 ? Base & Mask : Base) + B;
      if (Kind == 0 || Kind == 3)
        F ^= High;
      else if (Kind == 1)
        F |= High;
      else if (Kind == 2)
        F &= High | Mask;
      else if (Kind == 4)
        F += High | Other;
      else if (Kind == 5)
        F ^= High | Mask;
      else if (Kind == 6 || Kind == 7)
        F &= Mask;
      else if (Kind == 8)
        F = F.trunc(Bits).sextOrTrunc(Width);
      else if (Kind == 9)
        F += (V[3] * V[4]).shl(Bits);
      Result *= F;
    }
    return Result;
  }
};

Fixture fixture(SymContext &Ctx, unsigned Width, unsigned Degree, unsigned Kind,
                unsigned Control = 0, unsigned PrefixBits = 0) {
  std::mt19937_64 Random(0x505245464958 + Width + 17 * Degree + 31 * Kind);
  auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
  auto Z = Ctx.mkVar("z", Width), U = Ctx.mkVar("u", Width);
  auto V = Ctx.mkVar("v", Width);
  const unsigned Bits =
      PrefixBits ? PrefixBits
                 : std::min(Width, unsigned(std::bit_width(Degree - 1)));
  unsigned Zeros = 0;
  for (unsigned B = 1; B <= Bits; ++B)
    Zeros += Degree >> B;
  auto Base = Ctx.mkAdd({X, Ctx.mkMul(Ctx.mkConst(Width, 3), Y),
                         Ctx.mkMul(Ctx.mkConst(Width, 5), Z)});
  const auto Mask = Ctx.mkConst(llvm::APInt::getLowBitsSet(Width, Bits));
  auto High = Ctx.mkShl(U, Ctx.mkConst(Width, Bits));
  auto Other = Ctx.mkShl(V, Ctx.mkConst(Width, Bits));
  Fixture Out{{},
              llvm::APInt(Width, 1).shl(Width > Zeros ? Width - Zeros : 0),
              {},
              Bits,
              Kind};
  if (Control == 2)
    ++Out.Scale;
  llvm::SmallVector<SymRef, 64> Factors{Ctx.mkConst(Out.Scale)};
  for (unsigned I = 0; I < Degree; ++I) {
    auto A = word(Width, Random) | 1;
    auto B = A * (Control == 1 && I + 1 == Degree ? I - 1 : I);
    if (Kind == 5)
      --B; // Complement then gives -A*(Base+I), not an unrelated offset.
    auto F = Ctx.mkAdd(
        Ctx.mkMul(Ctx.mkConst(A), Kind == 3 ? Ctx.mkAnd(Base, Mask) : Base),
        Ctx.mkConst(B));
    if (Kind == 0 || Kind == 3)
      F = Ctx.mkXor(F, High);
    else if (Kind == 1)
      F = Ctx.mkOr(F, High);
    else if (Kind == 2)
      F = Ctx.mkAnd(F, Ctx.mkOr(High, Mask));
    else if (Kind == 4)
      F = Ctx.mkAdd(F, Ctx.mkOr(High, Other));
    else if (Kind == 5)
      F = Ctx.mkXor(F, Ctx.mkOr(High, Mask));
    else if (Kind == 6)
      F = Ctx.mkAnd(F, Mask);
    else if (Kind == 7)
      F = Ctx.mkZExt(Ctx.mkExtract(F, 0, Bits), Width);
    else if (Kind == 8)
      F = Ctx.mkSExt(Ctx.mkExtract(F, 0, Bits), Width);
    else if (Kind == 9)
      F = Ctx.mkAdd(F, Ctx.mkShl(Ctx.mkMul(U, V), Ctx.mkConst(Width, Bits)));
    Factors.push_back(F);
    Out.Factors.emplace_back(std::move(A), std::move(B));
  }
  Out.Input = Ctx.mkMul(Factors);
  return Out;
}

TEST(SymMBAPrefixResidue, ProvesPerturbedHighDegreeProductsAcrossWidths) {
  std::mt19937_64 Random(0x5052454649584f52);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned Degree : {4u, 8u, 16u, 32u, 64u, 128u, 256u})
      for (unsigned Kind = 0; Kind < 9; ++Kind) {
        SymContext Ctx;
        auto F = fixture(Ctx, Width, Degree, Kind);
        MBAOptions Opts;
        Opts.MaxWork = 65536;
        Opts.VerifySamples = 0;
        auto Result = simplifyMBADeep(Ctx, F.Input, Opts);
        ASSERT_TRUE(Ctx.isConstZero(Result.Expr))
            << Width << ':' << Degree << ':' << Kind << ':' << Result.Work;
        EXPECT_LT(Result.Work, Opts.MaxWork);
        EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
        for (unsigned Trial = 0; Trial < 16; ++Trial) {
          std::array<llvm::APInt, 5> V;
          for (auto &Value : V)
            Value = word(Width, Random);
          auto Want = F.oracle(V);
          ASSERT_TRUE(Want.isZero()) << Width << ':' << Degree << ':' << Kind;
          EXPECT_EQ(Ctx.eval(F.Input, V), Want);
        }
      }
}

TEST(SymMBAPrefixResidue, RetainsNonzeroOffsetsAndCoefficientNeighbors) {
  std::mt19937_64 Random(0x4e45494748424f52);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned Kind = 0; Kind < 9; ++Kind)
      for (unsigned Control : {1u, 2u}) {
        // A complete set of explicitly truncated factors contains a literal
        // zero for every input regardless of the outer coefficient.
        if (Control == 2 && Kind >= 6)
          continue;
        SymContext Ctx;
        auto F = fixture(Ctx, Width, 4, Kind, Control);
        detail::WorkBudget Budget(65536);
        size_t Storage = 1 << 24;
        auto Result =
            detail::foldProductResidues(Ctx, F.Input, Budget, Storage);
        bool Nonzero = false;
        for (unsigned Trial = 0; Trial < 256; ++Trial) {
          std::array<llvm::APInt, 5> V;
          for (auto &Value : V)
            Value = word(Width, Random);
          const auto Want = F.oracle(V);
          Nonzero |= !Want.isZero();
          EXPECT_EQ(Ctx.eval(F.Input, V), Want);
          EXPECT_EQ(Ctx.eval(Result, V), Want);
        }
        EXPECT_TRUE(Nonzero) << Width << ':' << Kind << ':' << Control;
      }
}

TEST(SymMBAPrefixResidue, ProvesCoarsePrefixesWithoutAssumingTheHigherBits) {
  std::mt19937_64 Random(0x434f415253455052);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned Degree : {4u, 16u, 64u, 256u})
      for (unsigned Bits : {1u, 2u, 3u}) {
        if (Bits >= std::bit_width(Degree - 1))
          continue;
        for (unsigned Kind = 0; Kind < 10; ++Kind) {
          SymContext Ctx;
          auto F = fixture(Ctx, Width, Degree, Kind, 0, Bits);
          detail::WorkBudget Budget(65536);
          size_t Storage = 1 << 24;
          auto Output =
              detail::foldProductResidues(Ctx, F.Input, Budget, Storage);
          ASSERT_TRUE(Ctx.isConstZero(Output))
              << Width << ':' << Degree << ':' << Bits << ':' << Kind;
          EXPECT_LT(Budget.used(), 65536u);
          for (unsigned Trial = 0; Trial < 8; ++Trial) {
            std::array<llvm::APInt, 5> V;
            for (auto &Value : V)
              Value = word(Width, Random);
            const auto Want = F.oracle(V);
            ASSERT_TRUE(Want.isZero());
            EXPECT_EQ(Ctx.eval(F.Input, V), Want);
          }
        }
      }
}

TEST(SymMBAPrefixResidue, RereadsEvenZeroPrefixesAndKeepsNonzeroNeighbors) {
  std::mt19937_64 Random(0x4556454e50524546);
  const std::array<unsigned, 4> Units{2, 3, 10, 7};
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned Control = 0; Control < 3; ++Control) {
      SymContext Ctx;
      auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
      auto U = Ctx.mkVar("u", Width);
      auto Base = Ctx.mkAdd(X, Ctx.mkMul(Ctx.mkConst(Width, 3), Y));
      auto Scale = llvm::APInt(Width, 1).shl(Width - 4);
      if (Control == 2)
        ++Scale;
      llvm::SmallVector<SymRef, 8> Factors{Ctx.mkConst(Scale)};
      for (unsigned I = 0; I < 4; ++I) {
        auto Offset = Control == 1 && I == 3 ? 2 : I;
        auto F = Ctx.mkMul(Ctx.mkConst(Width, Units[I]),
                           Ctx.mkAdd(Base, Ctx.mkConst(Width, Offset)));
        Factors.push_back(
            Ctx.mkXor(F, Ctx.mkShl(U, Ctx.mkConst(Width, 1 + (I % 2 == 0)))));
      }
      auto Input = Ctx.mkMul(Factors);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      auto Output =
          detail::foldProjectedProductResidues(Ctx, Input, Budget, Storage);
      if (!Control)
        ASSERT_TRUE(Ctx.isConstZero(Output)) << Width;
      bool Nonzero = false;
      for (unsigned Trial = 0; Trial < 256; ++Trial) {
        std::array<llvm::APInt, 3> V;
        for (auto &Value : V)
          Value = word(Width, Random);
        auto B = V[0] + 3 * V[1];
        auto Want = Scale;
        for (unsigned I = 0; I < 4; ++I) {
          const auto Offset = Control == 1 && I == 3 ? 2 : I;
          Want *= (Units[I] * (B + Offset)) ^ V[2].shl(1 + (I % 2 == 0));
        }
        Nonzero |= !Want.isZero();
        EXPECT_EQ(Ctx.eval(Input, V), Want);
        EXPECT_EQ(Ctx.eval(Output, V), Want);
      }
      EXPECT_EQ(Nonzero, Control != 0) << Width << ':' << Control;
      if (Control || Width != 64)
        continue;
      const size_t Work = Budget.used(), Bytes = (1 << 24) - Storage;
      for (unsigned Short = 0; Short < 3; ++Short) {
        detail::WorkBudget Exact(Work - (Short == 1));
        size_t Remaining = Bytes - (Short == 2);
        auto Repeated =
            detail::foldProjectedProductResidues(Ctx, Input, Exact, Remaining);
        EXPECT_EQ(Repeated, Short ? Input : Output);
      }
    }
}

TEST(SymMBAPrefixResidue, KeepsNativeNonzeroAffineRecoveryWithinBudget) {
  std::mt19937_64 Random(0x414646494e454e5a);
  for (unsigned Width : {8u, 16u, 32u, 64u}) {
    SymContext Ctx;
    auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    auto Z = Ctx.mkVar("z", Width), W = Ctx.mkVar("w", Width);
    auto Scale = llvm::APInt(Width, 1).shl(Width - 8);
    std::vector<llvm::APInt> Units;
    llvm::SmallVector<SymRef, 16> Factors{Ctx.mkConst(Scale)};
    for (unsigned I = 0; I < 8; ++I) {
      auto A = (word(Width, Random) | 1).shl(I == 0);
      auto B = I ? A * I : llvm::APInt(Width, 1);
      Units.push_back(A);
      Factors.push_back(
          Ctx.mkAdd(Ctx.mkMul(Ctx.mkConst(A), X), Ctx.mkConst(B)));
    }
    auto Input = Ctx.mkAdd({Ctx.mkMul(Factors), Ctx.mkXor(X, Y),
                            Ctx.mkMul(Ctx.mkConst(Width, 2), Ctx.mkAnd(X, Y)),
                            Ctx.mkOr(Z, W), Ctx.mkAnd(Z, W)});
    MBAOptions Opts;
    Opts.MaxWork = 65536;
    Opts.VerifySamples = 0;
    auto Output = simplifyMBADeep(Ctx, Input, Opts);
    EXPECT_LT(Output.Work, Opts.MaxWork) << Width;
    bool Nonzero = false;
    for (unsigned Trial = 0; Trial < 128; ++Trial) {
      std::array<llvm::APInt, 4> V;
      for (auto &Value : V)
        Value = word(Width, Random);
      auto Product = Scale;
      for (unsigned I = 0; I < 8; ++I)
        Product *= Units[I] * V[0] + (I ? Units[I] * I : llvm::APInt(Width, 1));
      Nonzero |= !Product.isZero();
      auto Want = Product + V[0] + V[1] + V[2] + V[3];
      EXPECT_EQ(Ctx.eval(Input, V), Want);
      EXPECT_EQ(Ctx.eval(Output.Expr, V), Want);
    }
    EXPECT_TRUE(Nonzero) << Width;
  }
}

TEST(SymMBAPrefixResidue, ExhaustsSmallWordSlopesMasksAndPerturbations) {
  for (unsigned Slope : {1u, 3u, 5u, 7u})
    for (unsigned Mask : {3u, 7u, 11u, 15u})
      for (unsigned Layout = 0; Layout < 4; ++Layout) {
        SymContext Ctx;
        auto X = Ctx.mkVar("x", 4), U = Ctx.mkVar("u", 4);
        llvm::SmallVector<SymRef, 8> Factors{Ctx.mkConst(4, 2)};
        std::array<unsigned, 4> Offsets{};
        for (unsigned I = 0; I < 4; ++I) {
          Offsets[I] = Layout == 1 && I == 3 ? 2 : I;
          auto F = Ctx.mkAdd(Ctx.mkMul(Ctx.mkConst(4, Slope), X),
                             Ctx.mkConst(4, Slope * Offsets[I]));
          if (Layout == 2)
            F = Ctx.mkAnd(F, Ctx.mkOr(Ctx.mkConst(4, Mask), U));
          else
            F = Ctx.mkXor(F, Ctx.mkShl(U, Ctx.mkConst(4, Layout == 3 ? 1 : 2)));
          Factors.push_back(F);
        }
        auto Input = Ctx.mkMul(Factors);
        detail::WorkBudget Budget(65536);
        size_t Storage = 1 << 24;
        auto Output = detail::foldProductResidues(Ctx, Input, Budget, Storage);
        for (uint64_t A = 0; A < 16; ++A)
          for (uint64_t B = 0; B < 16; ++B) {
            uint64_t Want = 2;
            for (auto Offset : Offsets) {
              auto F = (Slope * (A + Offset)) & 15;
              F = Layout == 2 ? F & (Mask | B)
                              : F ^ ((B << (Layout == 3 ? 1 : 2)) & 15);
              Want = Want * F & 15;
            }
            EXPECT_EQ(Ctx.evalU64(Input, {A, B}), Want);
            EXPECT_EQ(Ctx.evalU64(Output, {A, B}), Want);
          }
      }
}

TEST(SymMBAPrefixResidue, DoesNotPromoteZeroPrefixesToZeroWords) {
  for (unsigned Width : {8u, 64u, 257u}) {
    SymContext Ctx;
    auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    auto High = ~llvm::APInt::getLowBitsSet(Width, 2);
    auto Odd = Ctx.mkOr(Y, Ctx.mkConst(Width, 1));
    auto Input = Ctx.mkMul(
        {Ctx.mkAnd(Ctx.mkAdd(X, Ctx.mkConst(Width, 1)), Ctx.mkConst(High)), Odd,
         Odd, Odd});
    detail::WorkBudget Budget(65536);
    size_t Storage = 1 << 20;
    auto Output = detail::foldProductResidues(Ctx, Input, Budget, Storage);
    EXPECT_EQ(Output, Input);
    const std::array<llvm::APInt, 2> Values{llvm::APInt(Width, 3),
                                            llvm::APInt(Width, 0)};
    EXPECT_EQ(Ctx.eval(Output, Values), llvm::APInt(Width, 4));
  }
}

TEST(SymMBAPrefixResidue, CompleteAffineBasesKeepPolynomialRecoveryBudget) {
  std::mt19937_64 Random(0x434f4d504c455445);
  const std::array<std::array<unsigned, 4>, 3> Units{
      {{1, 1, 1, 1}, {141, 75, 249, 107}, {119, 201, 135, 135}}};
  for (unsigned Width : {8u, 16u, 32u, 64u})
    for (const auto &Row : Units) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Row[0]);
      SymContext Ctx;
      auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
      auto Z = Ctx.mkVar("z", Width), W = Ctx.mkVar("w", Width);
      const auto Scale = llvm::APInt(Width, 33).shl(Width - 8);
      auto Base = Ctx.mkXor(Ctx.mkAdd(X, Y), Z);
      llvm::SmallVector<SymRef, 8> Factors{Ctx.mkConst(Scale)};
      for (unsigned I = 0; I < Row.size(); ++I)
        Factors.push_back(Ctx.mkAdd(Ctx.mkMul(Ctx.mkConst(Width, Row[I]), Base),
                                    Ctx.mkConst(Width, Row[I] * I)));
      auto Input = Ctx.mkAdd({Ctx.mkMul(Factors), Ctx.mkXor(X, Y),
                              Ctx.mkMul(Ctx.mkConst(Width, 2), Ctx.mkAnd(X, Y)),
                              Ctx.mkOr(Z, W), Ctx.mkAnd(Z, W)});
      MBAOptions Options;
      Options.MaxWork = 65536;
      Options.VerifySamples = 0;
      auto Result = simplifyMBADeep(Ctx, Input, Options);
      ASSERT_LT(Result.Work, Options.MaxWork);
      EXPECT_LT(detail::readingScore(Ctx, Result.Expr),
                detail::readingScore(Ctx, Input));
      if (Width == 8)
        EXPECT_LE(Result.SizeAfter, Row[0] == 1 ? 39u : 38u);
      for (unsigned Trial = 0; Trial < 256; ++Trial) {
        const std::array<llvm::APInt, 4> Values{
            word(Width, Random), word(Width, Random), word(Width, Random),
            word(Width, Random)};
        auto Expected = Scale;
        const auto B = (Values[0] + Values[1]) ^ Values[2];
        for (unsigned I = 0; I < Row.size(); ++I)
          Expected *= Row[I] * (B + llvm::APInt(Width, I));
        for (const auto &Value : Values)
          Expected += Value;
        EXPECT_EQ(Ctx.eval(Input, Values), Expected);
        EXPECT_EQ(Ctx.eval(Result.Expr, Values), Expected);
      }
    }
}

TEST(SymMBAPrefixResidue, VariableMonomialsKeepTheirAffineFastPath) {
  for (unsigned Width : {8u, 16u, 32u, 64u}) {
    SCOPED_TRACE(Width);
    SymContext Ctx;
    auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
    llvm::SmallVector<SymRef, 256> Factors;
    for (unsigned I = 0; I < 256; ++I) {
      auto Unit = llvm::APInt(Width, 2 * I + 1);
      Factors.push_back(
          Ctx.mkAdd(Ctx.mkMul({Ctx.mkConst(Unit), X, Y}),
                    Ctx.mkConst(Unit * llvm::APInt(Width, 11 + 5 * I))));
    }
    auto Input = Ctx.mkMul(Factors);
    MBAOptions Options;
    Options.MaxWork = 18000;
    Options.VerifySamples = 0;
    auto Result = simplifyMBADeep(Ctx, Input, Options);
    EXPECT_TRUE(Ctx.isConstZero(Result.Expr));
    EXPECT_LT(Result.Work, Options.MaxWork);
    for (unsigned I = 0; I < 256; ++I) {
      const std::array<llvm::APInt, 2> Values{llvm::APInt(Width, I),
                                              llvm::APInt(Width, 2 * I + 1)};
      auto Product = llvm::APInt(Width, 1);
      for (unsigned J = 0; J < 256; ++J)
        Product *= llvm::APInt(Width, 2 * J + 1) *
                   (Values[0] * Values[1] + llvm::APInt(Width, 11 + 5 * J));
      EXPECT_TRUE(Product.isZero());
      EXPECT_EQ(Ctx.eval(Input, Values), Product);
      EXPECT_EQ(Ctx.eval(Result.Expr, Values), Product);
    }
  }
}

TEST(SymMBAPrefixResidue, RequiresCompleteWorkAndStorage) {
  SymContext Ctx;
  auto F = fixture(Ctx, 257, 8, 2);
  detail::WorkBudget Full(65536);
  size_t Storage = 1 << 24;
  ASSERT_TRUE(Ctx.isConstZero(
      detail::foldProjectedProductResidues(Ctx, F.Input, Full, Storage)));
  const size_t Bytes = (1 << 24) - Storage;
  ASSERT_GT(Full.used(), 1u);
  ASSERT_GT(Bytes, 1u);
  for (size_t Work : {size_t(0), size_t(1), Full.used() - 1}) {
    detail::WorkBudget Budget(Work);
    size_t Remaining = 1 << 24;
    EXPECT_EQ(
        detail::foldProjectedProductResidues(Ctx, F.Input, Budget, Remaining),
        F.Input);
    EXPECT_LE(Budget.used(), Work);
  }
  for (size_t Limit : {size_t(0), size_t(1), Bytes - 1}) {
    detail::WorkBudget Budget(65536);
    size_t Remaining = Limit;
    EXPECT_EQ(
        detail::foldProjectedProductResidues(Ctx, F.Input, Budget, Remaining),
        F.Input);
  }
  detail::WorkBudget Exact(Full.used());
  Storage = Bytes;
  EXPECT_TRUE(Ctx.isConstZero(
      detail::foldProjectedProductResidues(Ctx, F.Input, Exact, Storage)));
}

TEST(SymMBAPrefixResidue, SynthesizesNonzeroProjectedWordsAndMixedWidths) {
  std::mt19937_64 Random(0x50524f4a454354);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned SourceWidth : {4u, Width, 257u})
      for (auto Op : {SymOp::And, SymOp::Or, SymOp::Xor}) {
        SymContext Ctx;
        auto X = Ctx.mkVar("x", SourceWidth), Y = Ctx.mkVar("y", SourceWidth);
        auto U = Ctx.mkVar("u", Width), V = Ctx.mkVar("v", Width);
        auto Base = Ctx.mkAdd(
            Ctx.mkZExtOrTrunc(X, Width),
            Ctx.mkMul(Ctx.mkConst(Width, 3), Ctx.mkZExtOrTrunc(Y, Width)));
        auto Scale = llvm::APInt(Width, 1).shl(Width - 3);
        llvm::SmallVector<SymRef, 8> Factors{Ctx.mkConst(Scale)};
        const unsigned Units[] = {3, 5, 7, 11};
        for (unsigned I = 0; I < 4; ++I) {
          auto A = Ctx.mkShl(Ctx.mkMul(Ctx.mkConst(Width, 2 * I + 1), U),
                             Ctx.mkConst(Width, 2));
          auto B = Ctx.mkShl(V, Ctx.mkConst(Width, 2));
          auto High = Op == SymOp::And  ? Ctx.mkAnd(A, B)
                      : Op == SymOp::Or ? Ctx.mkOr(A, B)
                                        : Ctx.mkXor(A, B);
          Factors.push_back(Ctx.mkAdd(
              {Ctx.mkMul(Ctx.mkConst(Width, Units[I]), Base),
               Ctx.mkConst(Width, Units[I] * std::min(I, 2u)), High}));
        }
        // A source optimizer can absorb the outer scale into the first
        // additive factor. Keep that exact association in this fixture.
        auto First = Factors[1];
        llvm::SmallVector<SymRef, 8> Scaled;
        llvm::SmallVector<SymRef, 8> FirstTerms(Ctx.operands(First));
        for (auto Term : FirstTerms)
          Scaled.push_back(Ctx.mkMul(Ctx.mkConst(Scale), Term));
        Factors[1] = Ctx.mkAdd(Scaled);
        Factors.erase(Factors.begin());
        auto Input = Ctx.mkMul(Factors);
        MBAOptions Opts;
        Opts.MaxWork = 65536;
        Opts.VerifySamples = 0;
        auto Result = simplifyMBADeep(Ctx, Input, Opts);
        ASSERT_LT(Result.Work, Opts.MaxWork)
            << Width << ':' << SourceWidth << ':' << unsigned(Op);
        EXPECT_LT(detail::readingScore(Ctx, Result.Expr),
                  detail::readingScore(Ctx, Input));
        EXPECT_EQ(Ctx.width(Result.Expr), Width);
        bool Nonzero = false;
        for (unsigned Trial = 0; Trial < 128; ++Trial) {
          const std::array<llvm::APInt, 4> Values{
              word(SourceWidth, Random), word(SourceWidth, Random),
              word(Width, Random), word(Width, Random)};
          auto Want = Scale;
          auto Plain =
              Values[0].zextOrTrunc(Width) + 3 * Values[1].zextOrTrunc(Width);
          for (unsigned I = 0; I < 4; ++I) {
            auto A = ((2 * I + 1) * Values[2]).shl(2);
            auto B = Values[3].shl(2);
            auto High = Op == SymOp::And  ? A & B
                        : Op == SymOp::Or ? A | B
                                          : A ^ B;
            Want *= Units[I] * Plain + Units[I] * std::min(I, 2u) + High;
          }
          Nonzero |= !Want.isZero();
          EXPECT_EQ(Ctx.eval(Input, Values), Want);
          EXPECT_EQ(Ctx.eval(Result.Expr, Values), Want);
        }
        EXPECT_TRUE(Nonzero);
        if (Width == 32 && SourceWidth == Width && Op == SymOp::Or) {
          detail::WorkBudget Full(65536);
          size_t Storage = 1 << 24;
          auto Complete =
              detail::foldAffineProductResidues(Ctx, Input, Full, Storage);
          ASSERT_TRUE(Complete && *Complete != Input);
          const size_t Bytes = (1 << 24) - Storage;
          ASSERT_GT(Full.used(), 1u);
          ASSERT_GT(Bytes, 1u);
          for (bool ShortWork : {false, true}) {
            detail::WorkBudget Short(Full.used() - ShortWork);
            Storage = Bytes - !ShortWork;
            auto Refused =
                detail::foldAffineProductResidues(Ctx, Input, Short, Storage);
            ASSERT_TRUE(Refused);
            EXPECT_EQ(*Refused, Input);
          }
          detail::WorkBudget Exact(Full.used());
          Storage = Bytes;
          auto Repeated =
              detail::foldAffineProductResidues(Ctx, Input, Exact, Storage);
          ASSERT_TRUE(Repeated);
          EXPECT_EQ(*Repeated, *Complete);
        }
        if (Width <= 64 && SourceWidth == Width &&
            solver::proofBackendAvailable(solver::ProofBackend::Z3)) {
          solver::SymSynthVerifier Verifier({}, solver::ProofBackend::Z3, 5000);
          EXPECT_EQ(Verifier(Ctx, Input, Result.Expr),
                    SynthVerification::Equivalent);
        }
      }
}

TEST(SymMBAPrefixResidue, HasIndependentFullWidthSMTProofs) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  for (unsigned Width : {8u, 16u, 32u, 64u})
    for (unsigned Kind : {0u, 2u, 5u, 8u})
      for (unsigned Prefix : {0u, 1u}) {
        SymContext Ctx;
        auto F = fixture(Ctx, Width, 4, Kind, 0, Prefix);
        detail::WorkBudget Budget(65536);
        size_t Storage = 1 << 24;
        auto Output =
            detail::foldProductResidues(Ctx, F.Input, Budget, Storage);
        ASSERT_TRUE(Ctx.isConstZero(Output)) << Width << ':' << Kind;
        solver::SymSynthVerifier Verifier({}, solver::ProofBackend::Z3, 5000);
        EXPECT_EQ(Verifier(Ctx, F.Input, Output), SynthVerification::Equivalent)
            << Width << ':' << Kind;
      }
}

struct DistributedFixture {
  SymRef Input;
  unsigned Degree;
  unsigned Kind;
  unsigned Control;
  llvm::APInt Scale;

  llvm::APInt oracle(const std::array<llvm::APInt, 5> &V) const {
    const unsigned Width = Scale.getBitWidth();
    const auto One = llvm::APInt(Width, 1);
    const auto Low = llvm::APInt(Width, 3);
    auto Product = Scale;
    for (unsigned I = 0; I < Degree; ++I) {
      auto U = V[2] + 2 * I, W = V[3] + 2 * I;
      llvm::APInt High(Width, 0);
      switch (Kind) {
      case 0:
        High = (U & ~One) * (W & ~One);
        break;
      case 1:
        High = (U | One) * (W | One) - One;
        break;
      case 2:
        High = (U | Low) * (W | Low) + Low;
        break;
      case 3:
        High = (U & ~One) * (W & ~One) * (V[4] & ~One);
        break;
      case 4:
        High = ~((U | One) * (W | One));
        break;
      case 5:
        High = ((U & ~One) ^ (W & ~One)) * (V[4] & ~One);
        break;
      case 6:
        High = ((U & ~One) | (W & ~One)) * (V[4] & ~One);
        break;
      default:
        High = ((U & ~Low) + (W & ~Low)) * (V[4] | One);
        break;
      }
      if (Control == 1 && I + 1 == Degree)
        High += One;
      auto Base = V[0] + 3 * V[1] + I;
      Product *= Kind & 1 ? Base + High : Base ^ High;
    }
    return Product;
  }
};

DistributedFixture distributedFixture(SymContext &Ctx, unsigned Width,
                                      unsigned Degree, unsigned Kind,
                                      unsigned Control = 0) {
  auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
  auto U = Ctx.mkVar("u", Width), V = Ctx.mkVar("v", Width);
  auto W = Ctx.mkVar("w", Width);
  auto C = [&](uint64_t Value) { return Ctx.mkConst(Width, Value); };
  auto One = C(1), Low = C(3);
  auto Even = Ctx.mkConst(~llvm::APInt(Width, 1));
  auto Four = Ctx.mkConst(~llvm::APInt(Width, 3));
  const unsigned Bits = Kind == 1 || Kind == 4 ? 1 : Kind == 3 ? 3 : 2;
  unsigned Zeros = 0;
  for (unsigned B = 1; B <= Bits; ++B)
    Zeros += Degree >> B;
  auto Scale = llvm::APInt(Width, 1).shl(Width > Zeros ? Width - Zeros : 0);
  if (Control == 2)
    ++Scale;
  llvm::SmallVector<SymRef, 64> Factors{Ctx.mkConst(Scale)};
  for (unsigned I = 0; I < Degree; ++I) {
    auto A = Ctx.mkAdd(U, C(2 * I)), B = Ctx.mkAdd(V, C(2 * I));
    auto AE = Ctx.mkAnd(A, Even), BE = Ctx.mkAnd(B, Even);
    SymRef High;
    switch (Kind) {
    case 0:
      High = Ctx.mkMul(AE, BE);
      break;
    case 1:
      High = Ctx.mkSub(Ctx.mkMul(Ctx.mkOr(A, One), Ctx.mkOr(B, One)), One);
      break;
    case 2:
      High = Ctx.mkAdd(Ctx.mkMul(Ctx.mkOr(A, Low), Ctx.mkOr(B, Low)), Low);
      break;
    case 3:
      High = Ctx.mkMul({AE, BE, Ctx.mkAnd(W, Even)});
      break;
    case 4:
      High = Ctx.mkNot(Ctx.mkMul(Ctx.mkOr(A, One), Ctx.mkOr(B, One)));
      break;
    case 5:
      High = Ctx.mkMul(Ctx.mkXor(AE, BE), Ctx.mkAnd(W, Even));
      break;
    case 6:
      High = Ctx.mkMul(Ctx.mkOr(AE, BE), Ctx.mkAnd(W, Even));
      break;
    default:
      High = Ctx.mkMul(Ctx.mkAdd(Ctx.mkAnd(A, Four), Ctx.mkAnd(B, Four)),
                       Ctx.mkOr(W, One));
      break;
    }
    if (Control == 1 && I + 1 == Degree)
      High = Ctx.mkAdd(High, One);
    auto Base = Ctx.mkAdd({X, Ctx.mkMul(C(3), Y), C(I)});
    Factors.push_back(Kind & 1 ? Ctx.mkAdd(Base, High) : Ctx.mkXor(Base, High));
  }
  return {Ctx.mkMul(Factors), Degree, Kind, Control, Scale};
}

TEST(SymMBAPrefixResidue,
     CombinesPartialKnownBitsAcrossArithmeticAndBitwiseNodes) {
  std::mt19937_64 Random(0x5041525449414c);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned Kind = 0; Kind < 8; ++Kind)
      for (unsigned Degree : {4u, 16u, 64u}) {
        SCOPED_TRACE(::testing::Message()
                     << Width << ':' << Kind << ':' << Degree);
        SymContext Ctx;
        auto F = distributedFixture(Ctx, Width, Degree, Kind);
        detail::WorkBudget Budget(65536);
        size_t Storage = 1 << 24;
        auto Result =
            detail::foldProductResidues(Ctx, F.Input, Budget, Storage);
        ASSERT_TRUE(Ctx.isConstZero(Result));
        for (unsigned Trial = 0; Trial < 32; ++Trial) {
          const std::array<llvm::APInt, 5> Values{
              word(Width, Random), word(Width, Random), word(Width, Random),
              word(Width, Random), word(Width, Random)};
          auto Expected = F.oracle(Values);
          EXPECT_TRUE(Expected.isZero());
          EXPECT_EQ(Ctx.eval(F.Input, Values), Expected);
        }
      }
}

TEST(SymMBAPrefixResidue,
     PartialKnownBitsPreserveNonzeroNeighborsAndResourceBounds) {
  for (unsigned Width : {8u, 32u, 65u, 257u})
    for (unsigned Kind = 0; Kind < 8; ++Kind) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Kind);
      for (unsigned Control : {1u, 2u}) {
        SymContext Ctx;
        auto F = distributedFixture(Ctx, Width, 4, Kind, Control);
        detail::WorkBudget Budget(65536);
        size_t Storage = 1 << 24;
        auto Result =
            detail::foldProductResidues(Ctx, F.Input, Budget, Storage);
        EXPECT_FALSE(Ctx.isConstZero(Result));
        bool Witness = false;
        for (unsigned X = 0; X < 16; ++X) {
          const std::array<llvm::APInt, 5> Values{
              llvm::APInt(Width, X), llvm::APInt(Width, 1),
              llvm::APInt(Width, 2), llvm::APInt(Width, 4),
              llvm::APInt(Width, 6)};
          auto Expected = F.oracle(Values);
          Witness |= !Expected.isZero();
          EXPECT_EQ(Ctx.eval(F.Input, Values), Expected);
          EXPECT_EQ(Ctx.eval(Result, Values), Expected);
        }
        EXPECT_TRUE(Witness);
      }
      SymContext Ctx;
      auto F = distributedFixture(Ctx, Width, 4, Kind);
      detail::WorkBudget Full(65536);
      size_t Storage = 1 << 24;
      auto Result = detail::foldProductResidues(Ctx, F.Input, Full, Storage);
      ASSERT_TRUE(Ctx.isConstZero(Result));
      const size_t Bytes = (1 << 24) - Storage;
      for (bool ShortWork : {false, true}) {
        detail::WorkBudget Short(Full.used() - ShortWork);
        Storage = Bytes - !ShortWork;
        EXPECT_EQ(detail::foldProductResidues(Ctx, F.Input, Short, Storage),
                  F.Input);
      }
      detail::WorkBudget Exact(Full.used());
      Storage = Bytes;
      EXPECT_EQ(detail::foldProductResidues(Ctx, F.Input, Exact, Storage),
                Result);
    }
}

TEST(SymMBAPrefixResidue,
     PartialKnownBitProductsHaveIndependentFullWidthProofs) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  for (unsigned Width : {8u, 16u, 32u, 64u})
    for (unsigned Kind : {0u, 1u, 2u, 5u, 6u, 7u}) {
      SymContext Ctx;
      auto F = distributedFixture(Ctx, Width, 4, Kind);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      auto Result = detail::foldProductResidues(Ctx, F.Input, Budget, Storage);
      ASSERT_TRUE(Ctx.isConstZero(Result));
      solver::SymSynthVerifier Verifier({}, solver::ProofBackend::Z3, 5000);
      EXPECT_EQ(Verifier(Ctx, F.Input, Result), SynthVerification::Equivalent)
          << Width << ':' << Kind;
    }
}

struct NonzeroResidueFixture {
  SymRef Input;
  llvm::APInt Scale;
  unsigned Kind;

  llvm::APInt oracle(const std::array<llvm::APInt, 5> &V) const {
    const unsigned Width = Scale.getBitWidth();
    const auto One = llvm::APInt(Width, 1);
    const auto Low = llvm::APInt(Width, 3);
    const uint64_t Units[] = {17, 51, 101, 121};
    auto Product = Scale;
    for (unsigned I = 0; I < 4; ++I) {
      auto U = V[2] + 2 * I, W = V[3] + 4 * I;
      auto High = llvm::APInt(Width, 0);
      if (Kind == 0)
        High = (U | Low) * (W | Low) + Low;
      else if (Kind == 1)
        High = ((U | Low) + (W | Low) + 2) * (V[4] | One);
      else if (Kind == 2)
        High = ((U & ~One) * (V[3] | One) + (V[4] & ~One)) * (W & ~Low);
      else
        High = ((U & ~One) * (W & ~One) * (V[4] | One)) |
               ((U | Low) * (W | Low) - One);
      const unsigned Offset = Kind < 2 ? std::min(I, 2u) : I;
      Product *= Units[I] * (V[0] + 3 * V[1] + Offset) + High;
    }
    return Product;
  }
};

NonzeroResidueFixture nonzeroResidueFixture(SymContext &Ctx, unsigned Width,
                                            unsigned Kind) {
  auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
  auto U = Ctx.mkVar("u", Width), V = Ctx.mkVar("v", Width);
  auto W = Ctx.mkVar("w", Width);
  auto C = [&](uint64_t Value) { return Ctx.mkConst(Width, Value); };
  const auto One = C(1), Low = C(3);
  const auto Even = Ctx.mkConst(~llvm::APInt(Width, 1));
  const auto Four = Ctx.mkConst(~llvm::APInt(Width, 3));
  const uint64_t Units[] = {17, 51, 101, 121};
  const unsigned Needed = Kind < 2 ? 3 : 4;
  auto Scale = llvm::APInt(Width, 1).shl(Width - Needed);
  llvm::SmallVector<SymRef, 8> Factors{Ctx.mkConst(Scale)};
  for (unsigned I = 0; I < 4; ++I) {
    auto A = Ctx.mkAdd(U, C(2 * I)), B = Ctx.mkAdd(V, C(4 * I));
    SymRef High;
    if (Kind == 0)
      High = Ctx.mkAdd(Ctx.mkMul(Ctx.mkOr(A, Low), Ctx.mkOr(B, Low)), Low);
    else if (Kind == 1)
      High = Ctx.mkMul(Ctx.mkAdd({Ctx.mkOr(A, Low), Ctx.mkOr(B, Low), C(2)}),
                       Ctx.mkOr(W, One));
    else if (Kind == 2)
      High =
          Ctx.mkMul(Ctx.mkAdd(Ctx.mkMul(Ctx.mkAnd(A, Even), Ctx.mkOr(V, One)),
                              Ctx.mkAnd(W, Even)),
                    Ctx.mkAnd(B, Four));
    else
      High = Ctx.mkOr(
          Ctx.mkMul({Ctx.mkAnd(A, Even), Ctx.mkAnd(B, Even), Ctx.mkOr(W, One)}),
          Ctx.mkSub(Ctx.mkMul(Ctx.mkOr(A, Low), Ctx.mkOr(B, Low)), One));
    const unsigned Offset = Kind < 2 ? std::min(I, 2u) : I;
    Factors.push_back(Ctx.mkAdd(
        Ctx.mkMul(C(Units[I]), Ctx.mkAdd({X, Ctx.mkMul(C(3), Y), C(Offset)})),
        High));
  }
  return {Ctx.mkMul(Factors), Scale, Kind};
}

TEST(SymMBAPrefixResidue, RetainsTheUnobservedFactorInNonzeroProducts) {
  std::mt19937_64 Random(0x4e4f4e5a45524f);
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned Kind = 0; Kind < 4; ++Kind)
      for (unsigned OddScale : {1u, 3u}) {
        SCOPED_TRACE(::testing::Message() << Width << ':' << Kind);
        SymContext Ctx;
        auto F = nonzeroResidueFixture(Ctx, Width, Kind);
        F.Input = Ctx.mkMul(Ctx.mkConst(Width, OddScale), F.Input);
        F.Scale *= OddScale;
        detail::WorkBudget Budget(65536);
        size_t Storage = 1 << 24;
        auto Result =
            detail::foldProductResidues(Ctx, F.Input, Budget, Storage);
        ASSERT_LT(Ctx.readabilityCost(Result), Ctx.readabilityCost(F.Input));
        ASSERT_FALSE(Ctx.isConstZero(Result));
        SymEvalPlan Original(Ctx, F.Input), Reduced(Ctx, Result);
        bool Witness = false;
        for (unsigned Trial = 0; Trial < 48; ++Trial) {
          std::array<llvm::APInt, 5> Values{
              word(Width, Random), word(Width, Random), word(Width, Random),
              word(Width, Random), word(Width, Random)};
          if (Trial < 16)
            Values = {llvm::APInt(Width, Trial), llvm::APInt(Width, 1),
                      llvm::APInt(Width, 2), llvm::APInt(Width, 4),
                      llvm::APInt(Width, 6)};
          auto Expected = F.oracle(Values);
          Witness |= !Expected.isZero();
          EXPECT_EQ(Original.eval(Values), Expected);
          EXPECT_EQ(Reduced.eval(Values), Expected);
        }
        EXPECT_TRUE(Witness);
      }
}

TEST(SymMBAPrefixResidue, NonzeroResidueTablesRespectExactResourceBoundaries) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 65u, 128u, 257u})
    for (unsigned Kind = 0; Kind < 4; ++Kind) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Kind);
      SymContext Ctx;
      auto F = nonzeroResidueFixture(Ctx, Width, Kind);
      detail::WorkBudget Full(65536);
      size_t Storage = 1 << 24;
      auto Result = detail::foldProductResidues(Ctx, F.Input, Full, Storage);
      ASSERT_LT(Ctx.readabilityCost(Result), Ctx.readabilityCost(F.Input));
      const size_t Bytes = (1 << 24) - Storage;
      ASSERT_GT(Full.used(), 0u);
      ASSERT_GT(Bytes, 0u);
      for (bool ShortWork : {false, true}) {
        detail::WorkBudget Short(Full.used() - ShortWork);
        Storage = Bytes - !ShortWork;
        auto Partial =
            detail::foldProductResidues(Ctx, F.Input, Short, Storage);
        EXPECT_LE(Short.used(), Full.used() - ShortWork);
        EXPECT_LE(Storage, Bytes - !ShortWork);
        EXPECT_LE(Ctx.readabilityCost(Partial), Ctx.readabilityCost(F.Input));
        SymEvalPlan Reduced(Ctx, Partial);
        for (unsigned X = 0; X < 32; ++X) {
          const std::array<llvm::APInt, 5> Values{
              llvm::APInt(Width, X), llvm::APInt(Width, 3),
              llvm::APInt(Width, X * 17), llvm::APInt(Width, X * 51),
              llvm::APInt(Width, X * 101)};
          EXPECT_EQ(Reduced.eval(Values), F.oracle(Values));
        }
      }
      detail::WorkBudget Exact(Full.used());
      Storage = Bytes;
      EXPECT_EQ(detail::foldProductResidues(Ctx, F.Input, Exact, Storage),
                Result);
    }
}

TEST(SymMBAPrefixResidue, NonzeroResidueTablesExhaustSmallInputDomains) {
  for (unsigned Kind = 0; Kind < 4; ++Kind) {
    SCOPED_TRACE(Kind);
    SymContext Ctx;
    auto F = nonzeroResidueFixture(Ctx, 8, Kind);
    detail::WorkBudget Budget(65536);
    size_t Storage = 1 << 24;
    auto Result = detail::foldProductResidues(Ctx, F.Input, Budget, Storage);
    ASSERT_LT(Ctx.readabilityCost(Result), Ctx.readabilityCost(F.Input));
    SymEvalPlan Original(Ctx, F.Input), Reduced(Ctx, Result);
    bool Witness = false;
    for (unsigned X = 0; X < 16; ++X)
      for (unsigned U = 0; U < 16; ++U)
        for (unsigned V = 0; V < 16; ++V) {
          const std::array<llvm::APInt, 5> Values{
              llvm::APInt(8, X), llvm::APInt(8, 3), llvm::APInt(8, U),
              llvm::APInt(8, V), llvm::APInt(8, 5)};
          auto Expected = F.oracle(Values);
          Witness |= !Expected.isZero();
          EXPECT_EQ(Original.eval(Values), Expected)
              << X << ':' << U << ':' << V;
          EXPECT_EQ(Reduced.eval(Values), Expected)
              << X << ':' << U << ':' << V;
        }
    EXPECT_TRUE(Witness);
  }
}

TEST(SymMBAPrefixResidue, NonzeroResidueTablesHaveIndependentFullWidthProofs) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 unavailable";
  for (unsigned Width : {8u, 16u, 32u, 64u})
    for (unsigned Kind = 0; Kind < 4; ++Kind) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Kind);
      SymContext Ctx;
      auto F = nonzeroResidueFixture(Ctx, Width, Kind);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      auto Result = detail::foldProductResidues(Ctx, F.Input, Budget, Storage);
      ASSERT_LT(Ctx.readabilityCost(Result), Ctx.readabilityCost(F.Input));
      ASSERT_FALSE(Ctx.isConstZero(Result));
      solver::SymSynthVerifier Verifier({}, solver::ProofBackend::Z3, 5000);
      EXPECT_EQ(Verifier(Ctx, F.Input, Result), SynthVerification::Equivalent);
    }
}

} // namespace
