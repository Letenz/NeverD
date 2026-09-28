//===- MedStackAlignmentTests.cpp - Entry-stack alignment proofs --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedStackAlignment.h"

#include <optional>
#include <utility>

namespace {

using namespace neverd;

MedVar reg(unsigned Version, Arch Architecture = Arch::ARM) {
  MedVar V;
  V.Kind = MedVar::Reg;
  V.TheArch = Architecture;
  V.Id = 1;
  V.SSAVer = Version;
  V.Size = getTargetRegInfo(Architecture).PointerSize;
  V.RegOff = getTargetRegInfo(Architecture).StackPointer;
  return V;
}

MedVar temp(int Id, Arch Architecture = Arch::ARM) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.TheArch = Architecture;
  V.Id = Id;
  V.SSAVer = 1;
  V.Size = getTargetRegInfo(Architecture).PointerSize;
  return V;
}

MedOp op(NdOp Opcode, MedVar Output, MedVar First,
         std::optional<MedVar> Second = std::nullopt) {
  MedOp Result;
  Result.Opcode = Opcode;
  Result.Output = Output;
  Result.addInput(First);
  if (Second)
    Result.addInput(*Second);
  return Result;
}

MedFunc frameWithEntrySP(Arch Architecture = Arch::ARM) {
  MedFunc Func;
  Func.Entry = 0x1000;
  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = Func.Entry;
  Block.Ops.push_back(
      op(NdOp::COPY, reg(0, Architecture), reg(0, Architecture)));
  Func.Blocks.push_back(std::move(Block));
  return Func;
}

TEST(MedStackAlignment, PreservesExactAndRoundedARMFrameOffsets) {
  MedFunc Func = frameWithEntrySP();
  auto &Ops = Func.Blocks[0].Ops;
  Ops.push_back(op(NdOp::INT_SUB, temp(2), reg(0), MedVar::makeConst(96, 4)));
  Ops.push_back(
      op(NdOp::INT_AND, temp(3), temp(2), MedVar::makeConst(0xfffffff8, 4)));
  Ops.push_back(op(NdOp::INT_ADD, temp(4), temp(2), MedVar::makeConst(1, 4)));
  Ops.push_back(
      op(NdOp::INT_AND, temp(5), temp(4), MedVar::makeConst(0xfffffff8, 4)));

  simplifyProvenStackAlignment(Func, Arch::ARM, BinaryFormat::MachO);

  EXPECT_EQ(Ops[2].Opcode, NdOp::COPY);
  EXPECT_EQ(Ops[2].NumInputs, 1);
  EXPECT_EQ(Ops[2].Inputs[0], temp(2));
  EXPECT_EQ(Ops[4].Opcode, NdOp::INT_SUB);
  EXPECT_EQ(Ops[4].NumInputs, 2);
  EXPECT_EQ(Ops[4].Inputs[0], temp(4));
  EXPECT_TRUE(Ops[4].Inputs[1].isConst());
  EXPECT_EQ(Ops[4].Inputs[1].ConstVal, 1u);
}

TEST(MedStackAlignment, RejectsUnprovedOrStrongerThanABIAlignment) {
  MedFunc Func = frameWithEntrySP();
  auto &Block = Func.Blocks[0];
  auto &Ops = Block.Ops;
  Ops.push_back(op(NdOp::INT_SUB, temp(2), reg(0), MedVar::makeConst(96, 4)));
  Ops.push_back(
      op(NdOp::INT_AND, temp(3), temp(2), MedVar::makeConst(0xfffffff0, 4)));
  Ops.push_back(
      op(NdOp::INT_AND, temp(4), temp(9), MedVar::makeConst(0xfffffff8, 4)));
  PhiNode Phi;
  Phi.Output = temp(6);
  Phi.Args.push_back({0, temp(2)});
  Block.Phis.push_back(Phi);
  Ops.push_back(
      op(NdOp::INT_AND, temp(7), temp(6), MedVar::makeConst(0xfffffff8, 4)));

  simplifyProvenStackAlignment(Func, Arch::ARM, BinaryFormat::MachO);

  EXPECT_EQ(Ops[2].Opcode, NdOp::INT_AND);
  EXPECT_EQ(Ops[3].Opcode, NdOp::INT_AND);
  EXPECT_EQ(Ops[4].Opcode, NdOp::INT_AND);
}

TEST(MedStackAlignment, RequiresAuthenticatedEntryStackPointer) {
  MedFunc Func = frameWithEntrySP();
  auto &Ops = Func.Blocks[0].Ops;
  Ops.erase(Ops.begin());
  Ops.push_back(
      op(NdOp::INT_AND, temp(2), reg(0), MedVar::makeConst(0xfffffff8, 4)));

  simplifyProvenStackAlignment(Func, Arch::ARM, BinaryFormat::MachO);

  EXPECT_EQ(Ops[0].Opcode, NdOp::INT_AND);
}

TEST(MedStackAlignment, RejectsAnAmbiguousEntryStackDefinition) {
  MedFunc Func = frameWithEntrySP();
  auto &Ops = Func.Blocks[0].Ops;
  Ops.push_back(op(NdOp::COPY, reg(0), temp(9)));
  Ops.push_back(
      op(NdOp::INT_AND, temp(2), reg(0), MedVar::makeConst(0xfffffff8, 4)));

  simplifyProvenStackAlignment(Func, Arch::ARM, BinaryFormat::MachO);

  EXPECT_EQ(Ops[2].Opcode, NdOp::INT_AND);
}

TEST(MedStackAlignment, HonorsEntryResiduesOnOtherArchitectures) {
  struct Case {
    Arch Architecture;
    BinaryFormat Format;
    uint64_t EntrySubtract;
    uint64_t Mask;
  };
  const Case Cases[] = {
      {Arch::X64, BinaryFormat::COFF, 8, UINT64_C(0xfffffffffffffff0)},
      {Arch::X86, BinaryFormat::MachO, 12, UINT32_C(0xfffffff0)},
      {Arch::AArch64, BinaryFormat::ELF, 16, UINT64_C(0xfffffffffffffff0)},
  };
  for (const Case &C : Cases) {
    SCOPED_TRACE(static_cast<int>(C.Architecture));
    MedFunc Func = frameWithEntrySP(C.Architecture);
    auto &Ops = Func.Blocks[0].Ops;
    const uint16_t Width = getTargetRegInfo(C.Architecture).PointerSize;
    Ops.push_back(op(NdOp::INT_SUB, temp(2, C.Architecture),
                     reg(0, C.Architecture),
                     MedVar::makeConst(C.EntrySubtract, Width)));
    Ops.push_back(op(NdOp::INT_AND, temp(3, C.Architecture),
                     temp(2, C.Architecture),
                     MedVar::makeConst(C.Mask, Width)));
    Ops.push_back(op(NdOp::INT_SUB, temp(4, C.Architecture),
                     reg(0, C.Architecture),
                     MedVar::makeConst(C.EntrySubtract - 1, Width)));
    Ops.push_back(op(NdOp::INT_AND, temp(5, C.Architecture),
                     temp(4, C.Architecture),
                     MedVar::makeConst(C.Mask, Width)));

    simplifyProvenStackAlignment(Func, C.Architecture, C.Format);

    EXPECT_EQ(Ops[2].Opcode, NdOp::COPY);
    EXPECT_EQ(Ops[4].Opcode, NdOp::INT_SUB);
    EXPECT_EQ(Ops[4].Inputs[1].ConstVal, 1u);
  }
}

TEST(MedStackAlignment, ZeroMasksDoNotProveStackAlignment) {
  for (Arch Architecture : {Arch::X64, Arch::AArch64, Arch::X86, Arch::ARM}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    for (bool Reversed : {false, true}) {
      SCOPED_TRACE(Reversed);
      MedFunc Func = frameWithEntrySP(Architecture);
      auto &Ops = Func.Blocks[0].Ops;
      const uint16_t Width = getTargetRegInfo(Architecture).PointerSize;
      const MedVar Stack = reg(0, Architecture);
      const MedVar Zero =
          MedVar::makeConst(0, Width, ConstantAddressProvenance::Scalar);
      Ops.push_back(op(NdOp::INT_AND, temp(2, Architecture),
                       Reversed ? Zero : Stack, Reversed ? Stack : Zero));
      const MedOp Original = Ops.back();

      simplifyProvenStackAlignment(Func, Architecture, BinaryFormat::MachO);

      // Clearing every bit produces zero, never a stack-relative value.
      // In particular, complement(mask) + 1 must not wrap into an accepted
      // zero alignment for a 64-bit operand.
      EXPECT_EQ(Ops.back().Opcode, NdOp::INT_AND);
      ASSERT_EQ(Ops.back().NumInputs, 2);
      EXPECT_EQ(Ops.back().Inputs[0], Original.Inputs[0]);
      EXPECT_EQ(Ops.back().Inputs[1], Original.Inputs[1]);
    }
  }
}

TEST(MedStackAlignment, ZeroMaskedValuesDoNotBecomeFrameAliases) {
  for (Arch Architecture : {Arch::X64, Arch::AArch64, Arch::X86, Arch::ARM}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    for (bool Reversed : {false, true}) {
      SCOPED_TRACE(Reversed);
      MedFunc Func = frameWithEntrySP(Architecture);
      auto &Ops = Func.Blocks[0].Ops;
      const uint16_t Width = getTargetRegInfo(Architecture).PointerSize;
      const MedVar Stack = reg(0, Architecture);
      const MedVar Zero =
          MedVar::makeConst(0, Width, ConstantAddressProvenance::Scalar);
      const MedVar Zeroed = temp(2, Architecture);
      const uint64_t Mask =
          Width == 8 ? UINT64_C(0xfffffffffffffff8) : UINT32_C(0xfffffff8);
      const MedVar Alignment =
          MedVar::makeConst(Mask, Width, ConstantAddressProvenance::Scalar);
      Ops.push_back(op(NdOp::INT_AND, Zeroed, Reversed ? Zero : Stack,
                       Reversed ? Stack : Zero));
      Ops.push_back(op(NdOp::INT_AND, temp(3, Architecture),
                       Reversed ? Alignment : Zeroed,
                       Reversed ? Zeroed : Alignment));
      const MedOp Original = Ops.back();

      simplifyProvenStackAlignment(Func, Architecture, BinaryFormat::MachO);

      // A later valid alignment mask does not authenticate its zero-valued
      // input as an entry-stack alias. Neither recursive proof nor rewrites
      // of an earlier definition may introduce that relationship.
      EXPECT_EQ(Ops.back().Opcode, NdOp::INT_AND);
      ASSERT_EQ(Ops.back().NumInputs, 2);
      EXPECT_EQ(Ops.back().Inputs[0], Original.Inputs[0]);
      EXPECT_EQ(Ops.back().Inputs[1], Original.Inputs[1]);
    }
  }
}

} // namespace
