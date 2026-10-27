//===- RegisterFlowTests.cpp - Original LowIR register queries -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Branch, byte-alias and undefined-output boundaries of register navigation.
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/ir/RegisterFlow.h"
#include "neverd/ir/TargetRegInfo.h"
using namespace neverd;
namespace {
LowOp op(NdOp Code, va_t PC, NdVar Output,
         std::initializer_list<NdVar> Inputs) {
  LowOp O;
  O.Opcode = Code;
  O.Addr = PC;
  O.Output = Output;
  for (auto V : Inputs)
    O.addInput(V);
  return O;
}
NdVar reg(unsigned Bytes = 8) {
  return NdVar::reg(getTargetRegInfo(Arch::X64).IntReturnReg, Bytes);
}
LowFunc fixture() {
  LowFunc F;
  F.Entry = 0x1000;
  LowBlock B;
  B.Id = 0;
  B.StartAddr = F.Entry;
  B.Ops = {
      op(NdOp::COPY, 0x1000, reg(), {NdVar::scalar(0x123456789abcdef0ULL, 8)}),
      op(NdOp::COPY, 0x1005, reg(1), {NdVar::scalar(7, 1)}),
      op(NdOp::COPY, 0x1008, NdVar::tmp(0, 8), {reg()}),
      op(NdOp::RETURN, 0x100C, {}, {reg()})};
  F.Blocks.push_back(B);
  return F;
}
RegisterFlowResult query(const LowFunc &F, va_t PC, const std::string &Kind,
                         std::map<va_t, RegisterValueAudit> Audits = {}) {
  if (Audits.empty())
    for (const auto &B : F.Blocks)
      for (const auto &O : B.Ops)
        Audits[O.Addr].Complete = true;
  return queryRegisterFlow(F, Arch::X64, PC, reg(), Kind, Audits);
}
TEST(RegisterFlow, PartialDefinitionsAndUses) {
  const auto F = fixture();
  auto D = query(F, 0x1008, "definitions");
  ASSERT_TRUE(D.Error.empty()) << D.Error;
  ASSERT_EQ(D.Sites.size(), 2u);
  EXPECT_FALSE(D.Unknown);
  auto U = query(F, 0x1000, "uses");
  ASSERT_EQ(U.Sites.size(), 2u);
  EXPECT_EQ(query(F, 0x1008, "value").Value, 0x123456789abcde07ULL);
}
TEST(RegisterFlow, ZeroExtendsArchitecturalSubregisterWrites) {
  auto F = fixture();
  F.Blocks[0].Ops[1] = op(NdOp::COPY, 0x1005, reg(4), {NdVar::scalar(42, 4)});
  EXPECT_EQ(query(F, 0x1008, "value").Value, 42u);
  auto D = query(F, 0x1008, "definitions");
  ASSERT_EQ(D.Sites.size(), 1u);
  EXPECT_EQ(D.Sites[0].Address, 0x1005u);
}
TEST(RegisterFlow, DifferentPathsAndIndependentRootsStayUnknown) {
  auto F = fixture();
  F.Blocks[0].Ops.resize(1);
  F.Blocks[0].Succs = {1, 2};
  for (int I = 1; I <= 2; ++I) {
    LowBlock B;
    B.Id = I;
    B.StartAddr = 0x1010 * I;
    B.Ops = {op(NdOp::COPY, B.StartAddr, reg(), {NdVar::scalar(I, 8)})};
    B.Succs = {3};
    F.Blocks.push_back(B);
  }
  LowBlock B;
  B.Id = 3;
  B.StartAddr = 0x4000;
  B.Ops = {op(NdOp::RETURN, 0x4000, {}, {reg()})};
  F.Blocks.push_back(B);
  EXPECT_FALSE(query(F, 0x4000, "value").Value);
  EXPECT_EQ(query(F, 0x4000, "definitions").Sites.size(), 2u);
  F.Blocks[2].Ops[0].Inputs[0] = NdVar::scalar(1, 8);
  EXPECT_EQ(query(F, 0x4000, "value").Value, 1u);
  F.ModuleAnalysisRoots.insert(0x4000);
  EXPECT_FALSE(query(F, 0x4000, "value").Value);
  EXPECT_TRUE(query(F, 0x4000, "definitions").Unknown);
}
TEST(RegisterFlow, CallsAndUndefinedOutputsDoNotClaimValues) {
  auto F = fixture();
  F.Blocks[0].Ops[1] = op(NdOp::CALL, 0x1005, {}, {NdVar::scalar(0x5000, 8)});
  EXPECT_TRUE(query(F, 0x1008, "definitions").Unknown);
  EXPECT_TRUE(query(F, 0x1000, "uses").Unknown);
  EXPECT_FALSE(query(F, 0x1008, "value").Value);
  F = fixture();
  std::map<va_t, RegisterValueAudit> Audits;
  for (auto &O : F.Blocks[0].Ops)
    Audits[O.Addr].Complete = true;
  Audits[0x1005].Undefined.push_back({1, reg(1)});
  EXPECT_FALSE(query(F, 0x1008, "value", Audits).Value);
  Audits[0x1005].Undefined.clear();
  Audits[0x1005].Complete = false;
  EXPECT_FALSE(query(F, 0x1008, "value", Audits).Value);
}
TEST(RegisterFlow, InstructionTempsExpireAndFunctionTempsPersist) {
  auto F = fixture();
  const auto T = NdVar::tmp(100, 8);
  F.Blocks[0].Ops[0] = op(NdOp::COPY, 0x1000, T, {NdVar::scalar(42, 8)});
  F.Blocks[0].Ops[1] = op(NdOp::COPY, 0x1005, reg(), {T});
  EXPECT_FALSE(query(F, 0x1008, "value").Value);
  F.FunctionTemporaries.push_back({100, 8});
  EXPECT_EQ(query(F, 0x1008, "value").Value, 42u);
}
TEST(RegisterFlow, LoopsConvergeAndUnknownTransfersKill) {
  auto F = fixture();
  F.Blocks[0].Succs = {0};
  EXPECT_FALSE(query(F, 0x1000, "value").Value);
  F.Blocks[0].Ops[1] = op(NdOp::LOAD, 0x1005, reg(), {reg()});
  EXPECT_FALSE(query(F, 0x1008, "value").Value);
  auto R = query(F, 0x9999, "definitions");
  EXPECT_FALSE(R.Error.empty());
}
TEST(RegisterFlow, AArch64UsesSharedZeroExtensionAndProjectionRules) {
  auto F = fixture();
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  const auto X = NdVar::reg(TRI.IntReturnReg, 8),
             W = NdVar::reg(TRI.IntReturnReg, 4);
  F.Blocks[0].Ops = {op(NdOp::COPY, 0x1000, X, {NdVar::scalar(UINT64_MAX, 8)}),
                     op(NdOp::COPY, 0x1004, W, {NdVar::scalar(17, 4)}),
                     op(NdOp::RETURN, 0x1008, {}, {X})};
  std::map<va_t, RegisterValueAudit> Audits;
  for (const auto &O : F.Blocks[0].Ops)
    Audits[O.Addr].Complete = true;
  auto Q = queryRegisterFlow(F, Arch::AArch64, 0x1008, X, "value", Audits);
  EXPECT_EQ(Q.Value, 17u);
  F.Blocks[0].Ops[1] = op(NdOp::SUBBYTES, 0x1004, W, {X, NdVar::scalar(0, 1)});
  EXPECT_EQ(
      queryRegisterFlow(F, Arch::AArch64, 0x1008, X, "value", Audits).Value,
      UINT64_MAX);
}
} // namespace
