#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighIR.h"

namespace neverd {
void elimConsecutiveDeadStores(std::vector<HighStmt> &Statements);
void elimUnreadPrivateFrameStores(HighFunc &Function, Arch Architecture);
} // namespace neverd
using namespace neverd;
namespace {
ExprPtr variable(int Version, uint16_t Size = 8, int Rename = -1) {
  MedVar V;
  V.Kind = MedVar::Reg;
  V.Id = 8;
  V.SSAVer = Version;
  V.RegOff = 64;
  V.Size = Size;
  V.RenameTag = Rename;
  V.TheArch = Arch::AArch64;
  return HighExpr::makeVar(V, NdType::makeInt(Size, false));
}
HighStmt assign(ExprPtr Dst, ExprPtr Value) {
  HighStmt S;
  S.Kind = StmtKind::Assign;
  S.Dst = std::move(Dst);
  S.Val = std::move(Value);
  return S;
}
HighStmt returning(ExprPtr Value) {
  HighStmt S;
  S.Kind = StmtKind::Return;
  S.RetVal = std::move(Value);
  return S;
}

TEST(HighSSADefinitions, SamePhysicalRegisterDoesNotOverwriteEarlierSSAValue) {
  auto A = variable(5), B = variable(8);
  auto Sum = HighExpr::makeBinop(NdOp::INT_ADD, A, B);
  std::vector<HighStmt> Body{assign(A, HighExpr::makeConst(93, 8)),
                             assign(B, HighExpr::makeConst(93, 8)),
                             returning(Sum)};
  elimConsecutiveDeadStores(Body);
  ASSERT_EQ(Body.size(), 3U);
  EXPECT_EQ(Body[0].Dst->Var.SSAVer, 5);
  EXPECT_EQ(Body[1].Dst->Var.SSAVer, 8);
}

TEST(HighSSADefinitions, WideningKeepsBothDistinctValuesAndTheConversion) {
  for (NdOp Extension : {NdOp::INT_ZEXT, NdOp::INT_SEXT}) {
    auto A = variable(1, 4), B = variable(2, 8);
    auto Bits = HighExpr::makeConst(0x80000001, 4);
    auto Wide = HighExpr::makeUnary(Extension, Bits);
    Wide->Type = NdType::makeInt(8, Extension == NdOp::INT_SEXT);
    std::vector<HighStmt> Body{assign(A, Bits), assign(B, Wide), returning(B)};
    elimConsecutiveDeadStores(Body);
    ASSERT_EQ(Body.size(), 3U);
    EXPECT_EQ(Body[1].Val->Op, Extension);
    EXPECT_EQ(Body[2].RetVal->Var.SSAVer, 2);
    EXPECT_EQ(Body[2].RetVal->Type->Size, 8);
  }
}

TEST(HighSSADefinitions, RenamedLocalsAndWidthsDoNotBorrowAnOverwrite) {
  for (bool DifferentWidth : {false, true}) {
    auto A = variable(3, 4, 7);
    auto B = variable(3, DifferentWidth ? 8 : 4, DifferentWidth ? 7 : 8);
    std::vector<HighStmt> Body{assign(A, HighExpr::makeConst(7, 4)),
                               assign(B, HighExpr::makeConst(9, B->Type->Size)),
                               returning(A)};
    elimConsecutiveDeadStores(Body);
    EXPECT_EQ(Body.size(), 3U);
  }
}

TEST(HighSSADefinitions, RealOverwriteKeepsCallEffectAndDependentRead) {
  auto A = variable(3);
  std::vector<HighStmt> Body{assign(A, HighExpr::makeConst(7, 8)),
                             assign(A, HighExpr::makeConst(9, 8)),
                             returning(A)};
  elimConsecutiveDeadStores(Body);
  ASSERT_EQ(Body.size(), 2U);
  EXPECT_EQ(Body[0].Val->ConstVal, 9U);
  Body = {assign(A, HighExpr::makeCall("effect", 0x2000, {})),
          assign(A, HighExpr::makeConst(9, 8)), returning(A)};
  elimConsecutiveDeadStores(Body);
  ASSERT_EQ(Body.size(), 3U);
  EXPECT_EQ(Body[0].Kind, StmtKind::Call);
  auto Call = HighExpr::makeCall("nested_effect", 0x3000, {});
  Call->Type = NdType::makeInt(8);
  Body = {assign(A, HighExpr::makeBinop(NdOp::INT_ADD, Call,
                                        HighExpr::makeConst(1, 8))),
          assign(A, HighExpr::makeConst(9, 8)), returning(A)};
  elimConsecutiveDeadStores(Body);
  ASSERT_EQ(Body.size(), 3U);
  EXPECT_EQ(Body[0].Kind, StmtKind::ExprStmt);
  EXPECT_EQ(Body[0].Val->Operands[0]->Kind, ExprKind::Call);
  Body = {assign(A, HighExpr::makeConst(7, 8)),
          assign(A, HighExpr::makeBinop(NdOp::INT_ADD, A,
                                        HighExpr::makeConst(9, 8))),
          returning(A)};
  elimConsecutiveDeadStores(Body);
  ASSERT_EQ(Body.size(), 3U);
}

HighFunc privateFrameStore(Arch Architecture) {
  const auto &TRI = getTargetRegInfo(Architecture);
  HighFunc Function;
  Function.FrameSize = 64;
  auto Base = variable(0, TRI.PointerSize);
  Base->Var.RegOff = TRI.StackPointer;
  Base->Var.TheArch = Architecture;
  auto Address = HighExpr::makeBinop(NdOp::INT_SUB, Base,
                                     HighExpr::makeConst(8, TRI.PointerSize));
  Address = HighExpr::makeBinop(NdOp::INT_SUB, Address,
                                HighExpr::makeConst(16, TRI.PointerSize));
  HighStmt Store;
  Store.Kind = StmtKind::Store;
  Store.Addr = 0x1010;
  Store.StoreAddr = Address;
  Store.StoreVal = variable(0);
  Store.StoreVal->Var.RegOff = TRI.IntReturnReg;
  Store.StoreVal->Var.TheArch = Architecture;
  Function.Body = {Store, returning(HighExpr::makeConst(42, 8))};
  return Function;
}

TEST(HighPrivateFrameStores, DiscardsUnreadPaddingAndPreservesBranchEntry) {
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    for (uint16_t ImmediateBytes : {1, 2, 4}) {
      auto Function = privateFrameStore(Architecture);
      auto &Address = Function.Body[0].StoreAddr;
      Address->Operands[1] = HighExpr::makeConst(16, ImmediateBytes);
      Address->Operands[0]->Operands[1] =
          HighExpr::makeConst(8, ImmediateBytes);
      elimUnreadPrivateFrameStores(Function, Architecture);
      ASSERT_EQ(Function.Body.size(), 2U);
      EXPECT_EQ(Function.Body[0].Kind, StmtKind::Block);
      EXPECT_EQ(Function.Body[0].Addr, 0x1010U);
      EXPECT_EQ(Function.Body[1].RetVal->ConstVal, 42U);
    }
  }
}

TEST(HighPrivateFrameStores, KeepsReadsEscapesEffectsAndUnprovenRanges) {
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    for (unsigned Variant = 0; Variant != 14; ++Variant) {
      SCOPED_TRACE(Variant);
      auto Function = privateFrameStore(Architecture);
      auto &Store = Function.Body[0];
      const auto Address = Store.StoreAddr;
      const auto Base = Address->Operands[0]->Operands[0];
      const auto Size = getTargetRegInfo(Architecture).PointerSize;
      if (Variant == 0)
        Function.Body[1].RetVal =
            HighExpr::makeLoad(Address, NdType::makeInt(8));
      if (Variant == 1)
        Function.Body[1].RetVal = Address;
      if (Variant == 2) {
        auto &Escape = Function.Body[1];
        Escape.Kind = StmtKind::Store;
        Escape.RetVal = nullptr;
        Escape.StoreAddr = HighExpr::makeConst(0x9000, Size);
        Escape.StoreVal = Address;
      }
      if (Variant == 3)
        Function.Body[1] = assign(variable(9), Address);
      if (Variant >= 4 && Variant <= 6)
        Store.StoreAddr =
            HighExpr::makeBinop(NdOp::INT_SUB, Base,
                                HighExpr::makeConst(Variant == 4   ? 4
                                                    : Variant == 5 ? 0
                                                                   : 72,
                                                    Size));
      if (Variant == 7)
        Store.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
      if (Variant == 8) {
        Store.StoreVal = HighExpr::makeCall("effect", 0x3000, {});
        Store.StoreVal->Type = NdType::makeInt(8);
      }
      if (Variant == 9)
        Function.Body[1].RetVal = HighExpr::makeCall("unknown", 0x3000, {});
      if (Variant == 10)
        Base->Var.SSAVer = 1;
      if (Variant == 11)
        Base->Var.RenameTag = 2;
      if (Variant == 12)
        Address->Operands[1] = HighExpr::makeConst(UINT64_C(1) << 40, 4);
      if (Variant == 13)
        Address->Operands[1] = HighExpr::makeConst(16, Size * 2);
      elimUnreadPrivateFrameStores(Function, Architecture);
      ASSERT_EQ(Function.Body[0].Kind, StmtKind::Store);
    }
  }
}
} // namespace
