//===- HighCDefinitionIndexTests.cpp - Exact address definitions
//-----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Definition identity and dominance in HighC memory analysis.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/pass/HighC/HighCPasses.h"
#include "neverd/ir/TargetRegInfo.h"

#include <array>

using namespace neverd;

namespace {

MedVar local(unsigned Id = 1) {
  MedVar Var;
  Var.Kind = MedVar::Temp;
  Var.Id = Id;
  Var.SSAVer = 1;
  Var.Size = 8;
  Var.TheArch = Arch::X64;
  return Var;
}

ExprPtr frameAddress(unsigned Offset) {
  MedVar SP;
  SP.Kind = MedVar::Reg;
  SP.TheArch = Arch::X64;
  SP.Size = 8;
  SP.RegOff = getTargetRegInfo(Arch::X64).StackPointer;
  return HighExpr::makeBinop(NdOp::INT_SUB, HighExpr::makeVar(SP),
                             HighExpr::makeConst(Offset, 8));
}

HighStmt assign(const MedVar &Var, ExprPtr Value) {
  HighStmt Stmt;
  Stmt.Kind = StmtKind::Assign;
  Stmt.Dst = HighExpr::makeVar(Var);
  Stmt.Val = std::move(Value);
  return Stmt;
}

HighStmt read(ExprPtr Address) {
  HighStmt Stmt;
  Stmt.Kind = StmtKind::Return;
  Stmt.RetVal = HighExpr::makeLoad(std::move(Address), NdType::makeInt(8));
  return Stmt;
}

HighStmt store(ExprPtr Address) {
  HighStmt Stmt;
  Stmt.Kind = StmtKind::Store;
  Stmt.StoreAddr = std::move(Address);
  Stmt.StoreVal = HighExpr::makeConst(42, 8);
  return Stmt;
}

HighCAnalysisState analyze(const HighFunc &Func) {
  HighCAnalysisState State;
  analyzeDeadStores(
      State, Func, [](const MedVar &Var) { return Var.display(); },
      [](const HighExpr &) { return std::string("unreduced-address"); });
  return State;
}

TEST(HighCDefinitionIndex, DistinguishesEveryExistingIdentityField) {
  const MedVar A = local();
  std::array<MedVar, 6> Others;
  Others.fill(A);
  Others[0].Kind = MedVar::Reg;
  ++Others[1].Id;
  ++Others[2].SSAVer;
  ++Others[3].RegOff;
  ++Others[4].RenameTag;
  Others[5].TheArch = Arch::AArch64;
  for (size_t I = 0; I < Others.size(); ++I) {
    SCOPED_TRACE(I);
    const MedVar &B = Others[I];
    for (bool Reversed : {false, true}) {
      SCOPED_TRACE(Reversed);
      auto AddressA = HighExpr::makeVar(A);
      auto AddressB = HighExpr::makeVar(B);
      auto ExpectedA = frameAddress(8);
      auto ExpectedB = frameAddress(16);
      HighFunc Func;
      Func.Body.push_back(
          assign(Reversed ? B : A, Reversed ? ExpectedB : ExpectedA));
      Func.Body.push_back(
          assign(Reversed ? A : B, Reversed ? ExpectedA : ExpectedB));
      Func.Body.push_back(store(AddressA));
      Func.Body.push_back(store(AddressB));
      Func.Body.push_back(store(ExpectedA));
      Func.Body.push_back(read(ExpectedB));
      auto State = analyze(Func);
      ASSERT_TRUE(State.AddressKeys.count(AddressA.get()));
      ASSERT_TRUE(State.AddressKeys.count(AddressB.get()));
      EXPECT_EQ(State.AddressKeys.at(AddressA.get()),
                State.AddressKeys.at(ExpectedA.get()));
      EXPECT_EQ(State.AddressKeys.at(AddressB.get()),
                State.AddressKeys.at(ExpectedB.get()));
      EXPECT_NE(State.AddressKeys.at(AddressA.get()),
                State.AddressKeys.at(AddressB.get()));
    }
  }
}

TEST(HighCDefinitionIndex, RepeatedDefinitionsNeverBecomeUniqueAgain) {
  for (unsigned Count : {2u, 3u}) {
    for (bool SameExpression : {false, true}) {
      SCOPED_TRACE(Count);
      SCOPED_TRACE(SameExpression);
      const MedVar Alias = local();
      auto Expected = frameAddress(8);
      auto Address = HighExpr::makeVar(Alias);
      HighFunc Func;
      Func.FrameSize = 32;
      for (unsigned I = 0; I < Count; ++I)
        Func.Body.push_back(assign(
            Alias, SameExpression ? Expected : frameAddress(8 * (I + 1))));
      Func.Body.push_back(store(Address));
      Func.Body.push_back(read(Expected));
      auto State = analyze(Func);
      EXPECT_FALSE(State.CanElideFrameStores);
      EXPECT_FALSE(State.DeadStmts.count(&Func.Body[Count]));
      EXPECT_NE(State.AddressKeys.at(Address.get()),
                State.AddressKeys.at(Expected.get()));
    }
  }
}

TEST(HighCDefinitionIndex, WidthDoesNotCreateAnIndependentDefinition) {
  MedVar Wide = local();
  MedVar Narrow = Wide;
  Narrow.Size = 4;
  auto Address = HighExpr::makeVar(Wide);
  auto Expected = frameAddress(8);
  HighFunc Func;
  Func.FrameSize = 32;
  Func.Body.push_back(assign(Wide, Expected));
  Func.Body.push_back(assign(Narrow, frameAddress(16)));
  Func.Body.push_back(store(Address));
  Func.Body.push_back(read(Expected));
  auto State = analyze(Func);
  EXPECT_FALSE(State.CanElideFrameStores);
  EXPECT_FALSE(State.DeadStmts.count(&Func.Body[2]));
  EXPECT_NE(State.AddressKeys.at(Address.get()),
            State.AddressKeys.at(Expected.get()));
}

TEST(HighCDefinitionIndex, UniqueDefinitionStillRequiresDominance) {
  const MedVar Alias = local();
  for (unsigned Placement = 0; Placement < 3; ++Placement) {
    SCOPED_TRACE(Placement);
    HighFunc Func;
    Func.FrameSize = 32;
    auto Address = HighExpr::makeVar(Alias);
    auto Definition = assign(Alias, frameAddress(8));
    if (Placement == 0) {
      Func.Body.push_back(std::move(Definition));
    } else if (Placement == 1) {
      HighStmt Branch;
      Branch.Kind = StmtKind::If;
      Branch.Cond = HighExpr::makeVar(local(2));
      Branch.Body.push_back(std::move(Definition));
      Func.Body.push_back(std::move(Branch));
    }
    Func.Body.push_back(store(Address));
    Func.Body.push_back(read(Address));
    if (Placement == 2)
      Func.Body.push_back(std::move(Definition));
    auto State = analyze(Func);
    EXPECT_EQ(State.CanElideFrameStores, Placement == 0);
    for (const auto &Stmt : Func.Body)
      if (Stmt.Kind == StmtKind::Store)
        EXPECT_FALSE(State.DeadStmts.count(&Stmt));
  }
}

TEST(HighCDefinitionIndex, CyclesAndAbsentDefinitionsStayUnresolved) {
  for (bool Cycle : {false, true}) {
    SCOPED_TRACE(Cycle);
    MedVar A = local(1);
    MedVar B = local(2);
    auto Address = HighExpr::makeVar(A);
    HighFunc Func;
    Func.FrameSize = 32;
    if (Cycle) {
      Func.Body.push_back(assign(A, HighExpr::makeVar(B)));
      Func.Body.push_back(assign(B, HighExpr::makeVar(A)));
    }
    Func.Body.push_back(store(Address));
    Func.Body.push_back(read(Address));
    auto State = analyze(Func);
    EXPECT_FALSE(State.CanElideFrameStores);
    EXPECT_TRUE(State.DeadStmts.empty());
    if (Cycle)
      EXPECT_EQ(State.AddressKeys.at(Address.get()), "unreduced-address");
  }
}

} // namespace
