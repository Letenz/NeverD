//===- SymBatchSubstitutionTests.cpp - Shared substitution walks ----------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymExpr.h"

using namespace neverd::symbolic;

namespace {

TEST(SymBatchSubstitution, SimultaneousValuesAndOrderedDuplicateRoots) {
  SymContext C;
  auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8), Z = C.mkVar("z", 8);
  auto Sum = C.mkAdd(X, Y);
  SymRef Roots[] = {Sum, X, Sum, Y};
  auto Results = C.substituteAll(Roots, {{X.index(), Y}, {Y.index(), Z}});
  ASSERT_EQ(Results.size(), 4u);
  EXPECT_EQ(Results[0], C.mkAdd(Y, Z));
  EXPECT_EQ(Results[1], Y);
  EXPECT_EQ(Results[2], Results[0]);
  EXPECT_EQ(Results[3], Z);
}

TEST(SymBatchSubstitution, EmptyInputsAndUnchangedRoots) {
  SymContext C;
  auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  EXPECT_TRUE(C.substituteAll({}, {{X.index(), Y}}).empty());
  auto Root = C.mkUDiv(X, Y);
  SymRef Roots[] = {Root, X, Root};
  EXPECT_EQ(C.substituteAll(Roots, {}), std::vector<SymRef>({Root, X, Root}));
  auto Other = C.mkVar("other", 8);
  EXPECT_EQ(C.substituteAll(Roots, {{Other.index(), Y}}),
            std::vector<SymRef>({Root, X, Root}));
}

TEST(SymBatchSubstitution,
     RebuildsSharedOpaqueChildrenWithAnIndependentOracle) {
  SymContext C;
  auto X = C.mkVar("x", 4), Y = C.mkVar("y", 4), Z = C.mkVar("z", 4);
  auto Shared = C.mkUDiv(X, Y);
  auto Sum = C.mkAdd(Shared, Y);
  auto Mix = C.mkXor(Shared, Z);
  auto Results = C.substituteAll({Sum, Mix}, {{X.index(), Z}});
  for (unsigned A = 0; A != 16; ++A)
    for (unsigned B = 0; B != 16; ++B) {
      uint64_t Inputs[] = {0, A, B};
      unsigned Quotient = A ? B / A : 15;
      EXPECT_EQ(C.evalU64(Results[0], Inputs), (Quotient + A) & 15);
      EXPECT_EQ(C.evalU64(Results[1], Inputs), Quotient ^ B);
    }
}

TEST(SymBatchSubstitution, RootsMayBorrowOperandStorageWhileBuildersGrowIt) {
  SymContext C;
  auto X = C.mkVar("x", 32), Y = C.mkVar("y", 32);
  std::vector<SymRef> Inputs;
  for (unsigned I = 0; I != 512; ++I) {
    auto Denominator = C.mkVar("d" + std::to_string(I), 32);
    Inputs.push_back(C.mkUDiv(X, Denominator));
  }
  auto Root = C.mkAdd(Inputs);
  auto Borrowed = C.operands(Root);
  std::vector<SymRef> Snapshot(Borrowed.begin(), Borrowed.end());
  auto Results = C.substituteAll(Borrowed, {{X.index(), Y}});
  ASSERT_EQ(Results.size(), Snapshot.size());
  for (unsigned I = 0; I != Results.size(); ++I)
    EXPECT_EQ(Results[I], C.mkUDiv(Y, C.operand(Snapshot[I], 1)));
}

TEST(SymBatchSubstitution, DeepSharedGraphsDoNotUseTheHostStack) {
  SymContext C;
  auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  auto Deep = X;
  for (unsigned I = 0; I != 20000; ++I)
    Deep = C.mkUDiv(Deep, Y);
  auto Results = C.substituteAll({Deep, Deep}, {{X.index(), C.mkConst(8, 7)}});
  ASSERT_EQ(Results.size(), 2u);
  EXPECT_EQ(Results[0], Results[1]);
  for (unsigned I = 0; I != 20000; ++I) {
    ASSERT_EQ(C.op(Results[0]), SymOp::UDiv);
    Results[0] = C.operand(Results[0], 0);
  }
  EXPECT_EQ(Results[0], C.mkConst(8, 7));
}

} // namespace
