//===- MedCachedCallTargetTests.cpp - Captured call proofs ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exact cached-target and Win64 call-interface regression cases.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedCallTarget.h"
#include "neverd/lift/X86/X86Regs.h"

namespace {
using namespace neverd;

MedVar value(int Id, uint16_t Bytes = 8) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = Id;
  V.Size = Bytes;
  V.TheArch = Arch::X64;
  return V;
}

MedVar reg(int Id, uint64_t Offset, uint16_t Bytes = 8) {
  auto V = value(Id, Bytes);
  V.Kind = MedVar::Reg;
  V.RegOff = Offset;
  return V;
}

MedOp op(NdOp Opcode, MedVar Output, std::initializer_list<MedVar> Inputs) {
  MedOp O;
  O.Opcode = Opcode;
  O.Output = Output;
  for (const auto &V : Inputs)
    O.addInput(V);
  return O;
}

MedBlock cached(bool Import = true) {
  const auto SP = reg(1, x86reg::RSP);
  MedBlock B;
  B.Ops = {op(NdOp::INT_SUB, value(2), {SP, MedVar::makeConst(64, 8)}),
           op(NdOp::LOAD, value(3), {MedVar::makeConst(0x3000, 8)}),
           op(NdOp::STORE, {},
              {value(2), Import ? value(3) : MedVar::makeConst(0x1000, 8)}),
           op(NdOp::INT_ADD, SP, {value(2), MedVar::makeConst(16, 8)}),
           op(NdOp::INT_SUB, value(4), {SP, MedVar::makeConst(16, 8)}),
           op(NdOp::LOAD, value(5), {value(4)})};
  return B;
}

TEST(MedCachedCallTarget, ProvesMovingStackWithoutChangingRuntimeOperands) {
  for (bool Import : {false, true}) {
    auto B = cached(Import);
    const auto Original = B.Ops.back().Inputs[0];
    const auto Target = resolvePreSSAMedCallTarget(B, value(5), Arch::X64);
    ASSERT_TRUE(Target);
    EXPECT_EQ(Target->ThroughSlot, Import);
    EXPECT_EQ(Target->Address, Import ? 0x3000u : 0x1000u);
    EXPECT_EQ(Target->LoadIndex,
              Import ? std::optional<size_t>{1} : std::nullopt);
    EXPECT_EQ(B.Ops.back().Opcode, NdOp::LOAD);
    EXPECT_EQ(B.Ops.back().Inputs[0], Original);
  }
}

TEST(MedCachedCallTarget, RejectsInterveningCallsAndMayAliasWrites) {
  for (bool Call : {false, true}) {
    auto B = cached();
    B.Ops.insert(
        B.Ops.begin() + 3,
        Call ? op(NdOp::CALL, reg(6, x86reg::RAX),
                  {MedVar::makeConst(0x2000, 8)})
             : op(NdOp::STORE, {}, {value(99), MedVar::makeConst(0, 8)}));
    EXPECT_FALSE(resolvePreSSAMedCallTarget(B, value(5), Arch::X64));
  }
}

TEST(MedCachedCallTarget, RejectsPartialAndOrderedOverwrites) {
  for (unsigned Variant = 0; Variant < 3; ++Variant) {
    auto B = cached();
    if (Variant == 0)
      B.Ops.insert(B.Ops.begin() + 3,
                   op(NdOp::STORE, {}, {value(2), MedVar::makeConst(0, 4)}));
    else if (Variant == 1)
      B.Ops.back().MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
    else
      B.Ops[2].MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
    EXPECT_FALSE(resolvePreSSAMedCallTarget(B, value(5), Arch::X64));
  }
}

TEST(MedCachedCallTarget, DoesNotReuseAnOverwrittenRegisterAddress) {
  auto B = cached();
  B.Ops.insert(B.Ops.begin() + 4, op(NdOp::COPY, reg(1, x86reg::RSP, 4),
                                     {MedVar::makeConst(0x9000, 4)}));
  EXPECT_FALSE(resolvePreSSAMedCallTarget(B, value(5), Arch::X64));
}

TEST(MedCachedCallTarget, AuxiliaryOutputsInvalidatePreviousTemporaryValues) {
  MedBlock B;
  B.Ops = {op(NdOp::COPY, value(2), {MedVar::makeConst(0x1000, 8)})};
  auto Effect = op(NdOp::INTRINSIC, {}, {});
  Effect.IntrinsicOutputs = {value(2)};
  B.Ops.push_back(Effect);
  EXPECT_FALSE(resolvePreSSAMedCallTarget(B, value(2), Arch::X64));
}

TEST(MedCachedCallTarget, ExhaustionPublishesNoPartialTarget) {
  auto B = cached();
  for (size_t Budget : {size_t(0), size_t(1)})
    EXPECT_FALSE(resolvePreSSAMedCallTarget(B, value(5), Arch::X64, &Budget));
  size_t Budget = 10000;
  ASSERT_TRUE(resolvePreSSAMedCallTarget(B, value(5), Arch::X64, &Budget));
  EXPECT_LT(Budget, 10000u);
}

TEST(MedCachedCallTarget, RetainsTheOriginalMutableImportObservation) {
  auto B = cached();
  // Updating the IAT does not change the already captured callable. This
  // observation must neither turn the call into a new load nor mutate IR.
  B.Ops.insert(B.Ops.begin() + 2, op(NdOp::STORE, {},
                                     {MedVar::makeConst(0x3000, 8),
                                      MedVar::makeConst(0x4000, 8)}));
  const auto Target = resolvePreSSAMedCallTarget(B, value(5), Arch::X64);
  ASSERT_TRUE(Target);
  EXPECT_EQ(Target->Address, 0x3000u);
  EXPECT_TRUE(Target->ThroughSlot);
  EXPECT_EQ(B.Ops.size(), 7u);
}

TEST(MedCachedCallTarget, WindowsVolatileTransportIsNotAnAggregateResult) {
  for (BinaryFormat Format : {BinaryFormat::COFF, BinaryFormat::ELF}) {
    LowFunc F;
    F.Name = "transport_after_call";
    F.Entry = 0x1000;
    LowBlock B;
    B.Id = 0;
    B.StartAddr = F.Entry;
    const auto Add = [&](NdOp Opcode, NdVar Out,
                         std::initializer_list<NdVar> Inputs) {
      LowOp O;
      O.Opcode = Opcode;
      O.Addr = F.Entry + B.Ops.size();
      O.Output = Out;
      for (const auto &V : Inputs)
        O.Inputs[O.NumInputs++] = V;
      B.Ops.push_back(O);
    };
    Add(NdOp::CALL, NdVar::reg(x86reg::RAX, 8), {NdVar::cst(0x2000, 8)});
    Add(NdOp::STORE, {}, {NdVar::cst(0x4000, 8), NdVar::reg(x86reg::RAX, 8)});
    Add(NdOp::STORE, {}, {NdVar::cst(0x4008, 8), NdVar::reg(x86reg::RDX, 8)});
    Add(NdOp::RETURN, {}, {NdVar::reg(x86reg::RAX, 8)});
    F.Blocks.push_back(B);
    LowToMedConverter Converter;
    const auto M = Converter.convert(F, Arch::X64, Format);
    bool Found = false;
    for (const auto &Block : M.Blocks)
      for (const auto &O : Block.Ops)
        if (O.Opcode == NdOp::CALL) {
          Found = true;
          EXPECT_EQ(O.Output.Size, Format == BinaryFormat::COFF ? 8 : 16);
        }
    EXPECT_TRUE(Found);
  }
}

TEST(MedCachedCallTarget, HighIRCallsTheCapturedValueAfterTheSlotChanges) {
  for (bool Spilled : {false, true}) {
    SCOPED_TRACE(Spilled);
    MedFunc F;
    F.Entry = 0x1000;
    F.Name = "captured_call";
    F.FrameSize = 64;
    MedBlock B;
    B.Id = 0;
    B.StartAddr = F.Entry;
    const auto SP = reg(1, x86reg::RSP);
    B.Ops = {op(NdOp::INT_SUB, value(2), {SP, MedVar::makeConst(16, 8)}),
             op(NdOp::LOAD, value(3), {MedVar::makeConst(0x3000, 8)}),
             op(NdOp::STORE, {},
                {MedVar::makeConst(0x3000, 8), MedVar::makeConst(0x4000, 8)}),
             op(NdOp::STORE, {}, {value(2), value(3)}),
             op(NdOp::LOAD, value(5), {value(2)}),
             op(NdOp::INDIR_CALL, value(6), {Spilled ? value(5) : value(3)}),
             op(NdOp::RETURN, {}, {value(6)})};
    for (size_t I = 0; I < B.Ops.size(); ++I)
      B.Ops[I].Addr = F.Entry + I;
    F.Blocks = {B};
    const std::map<va_t, std::string> Names{{0x3000, "observed_import"}};
    MedToHighConverter Converter;
    Converter.setFuncNames(&Names);
    auto High = Converter.convert(F, Arch::X64);
    bool Found = false;
    walkStmts(High.Body, [&](const HighStmt &S) {
      forEachRhsExpr(S, [&](const ExprPtr &Root) {
        const auto Find = [&](auto &&Self, const ExprPtr &E) -> void {
          if (!E)
            return;
          if (E->Kind == ExprKind::Call) {
            Found = true;
            EXPECT_TRUE(E->IsIndirectCall);
            EXPECT_TRUE(E->IndirectTarget);
            EXPECT_EQ(E->CallAddr, 0x3000u);
          }
          E->forEachChildExpr([&](const ExprPtr &Child) { Self(Self, Child); });
        };
        Find(Find, Root);
      });
    });
    EXPECT_TRUE(Found);
  }
}
TEST(MedCachedCallTarget, MachineSummaryAdapterKeepsTheOriginalCallOperand) {
  for (bool Import : {false, true}) {
    const auto M = cached(Import);
    LowBlock B;
    const auto Convert = [](const MedVar &V) {
      return V.isConst()             ? NdVar::cst(V.ConstVal, V.Size)
             : V.Kind == MedVar::Reg ? NdVar::reg(V.RegOff, V.Size)
                                     : NdVar::tmp(V.Id, V.Size);
    };
    for (const auto &Op : M.Ops) {
      LowOp O;
      O.Opcode = Op.Opcode;
      O.Output = Convert(Op.Output);
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        O.addInput(Convert(Op.Inputs[I]));
      B.Ops.push_back(O);
    }
    LowOp Call;
    Call.Opcode = NdOp::INDIR_CALL;
    Call.addInput(NdVar::tmp(5, 8));
    B.Ops.push_back(Call);
    const auto Targets = resolveLowBlockCallTargets(B, Arch::X64);
    ASSERT_EQ(Targets.size(), 1u);
    EXPECT_EQ(Targets.begin()->first, B.Ops.size() - 1);
    EXPECT_EQ(Targets.begin()->second.Address, Import ? 0x3000u : 0x1000u);
    EXPECT_EQ(Targets.begin()->second.ThroughSlot, Import);
    EXPECT_EQ(B.Ops.back(), Call);
    size_t Work = 1;
    EXPECT_TRUE(resolveLowBlockCallTargets(B, Arch::X64, &Work).empty());
    LowOp Clobber;
    Clobber.Opcode = NdOp::STORE;
    Clobber.addInput(NdVar::tmp(2, 8));
    Clobber.addInput(NdVar::cst(0, 4));
    B.Ops.insert(B.Ops.begin() + 3, Clobber);
    EXPECT_TRUE(resolveLowBlockCallTargets(B, Arch::X64).empty());
  }
}
} // namespace
