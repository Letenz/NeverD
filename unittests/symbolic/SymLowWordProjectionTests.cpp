//===- SymLowWordProjectionTests.cpp - Completed low-word projections -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks projection identity, context lifetime and bounded-cache collisions.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymExpr.h"

#include <array>
#include <utility>

using namespace neverd::symbolic;

namespace {

void exactContexts(const SymContext &A, const SymContext &B) {
  ASSERT_EQ(A.numNodes(), B.numNodes());
  ASSERT_EQ(A.numVars(), B.numVars());
  for (uint32_t I = 0; I != A.numNodes(); ++I) {
    const SymRef R(I);
    const auto &X = A.node(R), &Y = B.node(R);
    EXPECT_EQ(X.Op, Y.Op);
    EXPECT_EQ(X.Width, Y.Width);
    EXPECT_EQ(X.FirstOperand, Y.FirstOperand);
    EXPECT_EQ(X.NumOperands, Y.NumOperands);
    EXPECT_EQ(X.Aux, Y.Aux);
    EXPECT_EQ(A.operands(R), B.operands(R));
    if (A.isConst(R))
      EXPECT_EQ(A.constValue(R), B.constValue(R));
  }
  for (uint32_t I = 0; I != A.numVars(); ++I) {
    const auto &X = A.varInfo(I), &Y = B.varInfo(I);
    EXPECT_EQ(X.Name, Y.Name);
    EXPECT_EQ(X.Width, Y.Width);
    EXPECT_EQ(X.Fresh, Y.Fresh);
    EXPECT_EQ(X.InputOrigin, Y.InputOrigin);
  }
}

struct WideningExample {
  SymContext C;
  SymRef X = C.mkVar("x", 8);
  SymRef Wide = C.mkZExt(X, 64);
  SymRef Y = C.mkVar("y", 64);
  SymRef Root = C.mkAdd(Wide, Y);

  SymRef expectedParent() {
    // The original walk visits the higher-index y first. Independently
    // author the expected narrow expression without projecting the parent.
    const auto LowY = C.mkExtract(Y, 0, 32);
    const auto LowX = C.mkZExt(X, 32);
    return C.mkAdd(LowX, LowY);
  }
};

TEST(SymLowWordProjection, WideningKeepsItsDirectAndInWalkFormsDistinct) {
  for (bool ParentFirst : {false, true}) {
    WideningExample Actual, Expected;
    if (!ParentFirst) {
      EXPECT_EQ(Actual.C.mkExtract(Actual.Wide, 0, 32),
                Expected.C.mkExtract(Expected.Wide, 0, 32));
      exactContexts(Actual.C, Expected.C);
    }
    for (unsigned I = 0; I != 4; ++I) {
      EXPECT_EQ(Actual.C.mkExtract(Actual.Root, 0, 32),
                Expected.expectedParent());
      exactContexts(Actual.C, Expected.C);
      const auto Direct = Actual.C.mkExtract(Actual.Wide, 0, 32);
      EXPECT_EQ(Direct, Expected.C.mkExtract(Expected.Wide, 0, 32));
      EXPECT_EQ(Actual.C.op(Direct), SymOp::Extract);
      EXPECT_EQ(Actual.C.operand(Direct, 0), Actual.Wide);
      exactContexts(Actual.C, Expected.C);
      Actual.C.mkFreshVar(17, "later");
      Expected.C.mkFreshVar(17, "later");
    }
  }
}

TEST(SymLowWordProjection, ACompletedImageCanHaveIndexZero) {
  SymContext C;
  const auto X = C.mkVar("x", 8);
  ASSERT_EQ(X.index(), 0u);
  const auto Root = C.mkAdd(C.mkZExt(X, 16), C.mkConst(16, 256));
  ASSERT_EQ(C.mkExtract(Root, 0, 8), X);
  const auto Nodes = C.numNodes();
  for (unsigned I = 0; I != 16; ++I)
    EXPECT_EQ(C.mkExtract(Root, 0, 8), X);
  EXPECT_EQ(C.numNodes(), Nodes);
  for (unsigned V = 0; V != 256; ++V) {
    const uint64_t Values[] = {V};
    EXPECT_EQ(C.evalU64(C.mkExtract(Root, 0, 8), Values), V);
  }
}

TEST(SymLowWordProjection, EqualNumericKeysNeverCrossContextBoundaries) {
  SymContext A, B;
  const auto AX = A.mkVar("x", 8), BX = B.mkVar("x", 8);
  const auto AW = A.mkZExt(AX, 16), BW = B.mkZExt(BX, 16);
  const auto AC = A.mkConst(16, 256), BC = B.mkConst(16, 255);
  const auto ARoot = A.mkAdd(AW, AC), BRoot = B.mkAdd(BW, BC);
  ASSERT_EQ(ARoot, BRoot);
  for (unsigned V = 0; V != 256; ++V) {
    const uint64_t Values[] = {V};
    const auto AI = A.mkExtract(ARoot, 0, 8);
    ASSERT_LT(AI.index(), A.numNodes());
    EXPECT_EQ(AI, AX);
    EXPECT_EQ(A.evalU64(AI, Values), V);
    const auto BI = B.mkExtract(BRoot, 0, 8);
    ASSERT_LT(BI.index(), B.numNodes());
    EXPECT_EQ(B.evalU64(BI, Values), (V + 255) & 255);
  }
}

TEST(SymLowWordProjection, WidthKeysKeepHighBitsAndAllWideInputValues) {
  SymContext C;
  const auto X = C.mkVar("x", 1024), Y = C.mkVar("y", 1024);
  const auto Root = C.mkXor(C.mkAdd(X, Y), C.mkMul(X, Y));
  const unsigned Widths[] = {1, 3, 8, 16, 32, 64, 65, 128, 256, 257};
  std::vector<SymRef> Images;
  for (unsigned Width : Widths) {
    Images.push_back(C.mkExtract(Root, 0, Width));
    EXPECT_EQ(C.width(Images.back()), Width);
  }
  const auto Nodes = C.numNodes();
  for (unsigned I = 0; I != 32; ++I) {
    llvm::APInt A(1024, uint64_t(I) * UINT64_C(0x9e3779b97f4a7c15));
    llvm::APInt B(1024, uint64_t(I + 3) * UINT64_C(0x94d049bb133111eb));
    A.setBit(700 + I);
    B.setBit(900 - I);
    const llvm::APInt Values[] = {A, B};
    const llvm::APInt Expected = (A + B) ^ (A * B);
    for (unsigned J = std::size(Widths); J-- != 0;) {
      const auto Image = C.mkExtract(Root, 0, Widths[J]);
      EXPECT_EQ(Image, Images[J]);
      EXPECT_EQ(C.eval(Image, Values), Expected.trunc(Widths[J]));
    }
  }
  EXPECT_EQ(C.numNodes(), Nodes);
}

TEST(SymLowWordProjection, EvictionAndGrowthKeepExactCompletedReferences) {
  SymContext C;
  const auto X = C.mkVar("x", 16), Mask = C.mkConst(16, 0x35);
  std::vector<SymRef> Roots, Images;
  // More distinct arithmetic roots than the maximum retained slot count.
  // The result checks hold regardless of which full keys collide or survive.
  for (unsigned I = 0; I != 5000; ++I) {
    Roots.push_back(C.mkXor(C.mkAdd(X, C.mkConst(16, I)), Mask));
    Images.push_back(C.mkExtract(Roots.back(), 0, 8));
  }
  const auto Nodes = C.numNodes();
  for (bool Reverse : {true, false}) {
    for (unsigned J = 0; J != Roots.size(); ++J) {
      const unsigned I = Reverse ? Roots.size() - 1 - J : J;
      const auto Image = C.mkExtract(Roots[I], 0, 8);
      EXPECT_EQ(Image, Images[I]);
      for (unsigned V : {0u, 127u, 65535u}) {
        const uint64_t Values[] = {V};
        EXPECT_EQ(C.evalU64(Image, Values), ((V + I) ^ 0x35) & 255);
      }
    }
  }
  EXPECT_EQ(C.numNodes(), Nodes);
}

void checkLateWord(SymContext &C, uint32_t Salt) {
  const auto X = C.mkFreshVar(16, "late");
  const auto Root =
      C.mkXor(C.mkAdd(X, C.mkConst(16, Salt)), C.mkConst(16, 0x5a));
  const auto Image = C.mkExtract(Root, 0, 8);
  std::vector<llvm::APInt> Values;
  for (uint32_t I = 0; I != C.numVars(); ++I)
    Values.emplace_back(C.varInfo(I).Width, 0);
  for (unsigned V : {0u, 1u, 127u, 65535u}) {
    Values[C.varId(X)] = llvm::APInt(16, V);
    EXPECT_EQ(C.eval(Image, Values).getZExtValue(), ((V + Salt) ^ 0x5a) & 255);
    EXPECT_EQ(C.mkExtract(Root, 0, 8), Image);
  }
}

TEST(SymLowWordProjection, CopiesAssignmentsAndMovesKeepIndependentContexts) {
  WideningExample Original;
  const auto Image = Original.C.mkExtract(Original.Root, 0, 32);
  SymContext Copy = Original.C;
  exactContexts(Original.C, Copy);
  EXPECT_EQ(Copy.mkExtract(Original.Root, 0, 32), Image);
  for (unsigned I = 0; I != 80; ++I) {
    // Both contexts append different graphs at overlapping numeric indices.
    checkLateWord(Original.C, I * 11);
    checkLateWord(Copy, I * 13 + 3);
  }
  SymContext Assigned;
  checkLateWord(Assigned, 67);
  Assigned = Copy;
  exactContexts(Assigned, Copy);
  SymContext Moved = std::move(Copy);
  exactContexts(Assigned, Moved);
  EXPECT_EQ(Moved.mkExtract(Original.Root, 0, 32), Image);
  checkLateWord(Copy, 191);
  checkLateWord(Moved, 23);
  SymContext MoveAssigned;
  checkLateWord(MoveAssigned, 239);
  MoveAssigned = std::move(Moved);
  EXPECT_EQ(MoveAssigned.mkExtract(Original.Root, 0, 32), Image);
  checkLateWord(Moved, 73);
  checkLateWord(MoveAssigned, 137);
  EXPECT_EQ(Original.C.mkExtract(Original.Root, 0, 32), Image);
}

TEST(SymLowWordProjection, IncrementalSharedPrefixesRetainAllByteInputs) {
  SymContext C;
  const auto X = C.mkVar("x", 16), Y = C.mkVar("y", 16);
  const auto Mask = C.mkConst(16, 0x35);
  SymRef Root = X;
  for (unsigned I = 0; I != 1024; ++I) {
    const auto Previous = Root;
    const auto OldImage = C.mkExtract(Previous, 0, 8);
    Root = C.mkXor(C.mkAdd(Root, Y), Mask);
    const auto Image = C.mkExtract(Root, 0, 8);
    EXPECT_EQ(C.mkExtract(Previous, 0, 8), OldImage);
    EXPECT_EQ(C.mkExtract(Root, 0, 8), Image);
  }
  const auto Image = C.mkExtract(Root, 0, 8);
  for (unsigned V = 0; V != 256; ++V) {
    unsigned Expected = V;
    for (unsigned I = 0; I != 1024; ++I)
      Expected = ((Expected + 29) ^ 0x35) & 255;
    const uint64_t Values[] = {V | 0xa500u, 29 | 0x7300u};
    EXPECT_EQ(C.evalU64(Image, Values), Expected);
  }
}

} // namespace
