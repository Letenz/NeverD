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
      Result *= F;
    }
    return Result;
  }
};

Fixture fixture(SymContext &Ctx, unsigned Width, unsigned Degree, unsigned Kind,
                unsigned Control = 0) {
  std::mt19937_64 Random(0x505245464958 + Width + 17 * Degree + 31 * Kind);
  auto X = Ctx.mkVar("x", Width), Y = Ctx.mkVar("y", Width);
  auto Z = Ctx.mkVar("z", Width), U = Ctx.mkVar("u", Width);
  auto V = Ctx.mkVar("v", Width);
  const unsigned Bits = std::min(Width, unsigned(std::bit_width(Degree - 1)));
  const unsigned Zeros = Degree - std::popcount(Degree);
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
    for (unsigned Kind : {0u, 2u, 5u, 8u}) {
      SymContext Ctx;
      auto F = fixture(Ctx, Width, 4, Kind);
      detail::WorkBudget Budget(65536);
      size_t Storage = 1 << 24;
      auto Output = detail::foldProductResidues(Ctx, F.Input, Budget, Storage);
      ASSERT_TRUE(Ctx.isConstZero(Output)) << Width << ':' << Kind;
      solver::SymSynthVerifier Verifier({}, solver::ProofBackend::Z3, 5000);
      EXPECT_EQ(Verifier(Ctx, F.Input, Output), SynthVerification::Equivalent)
          << Width << ':' << Kind;
    }
}

} // namespace
