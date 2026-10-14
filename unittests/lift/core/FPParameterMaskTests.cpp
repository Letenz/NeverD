//===- FPParameterMaskTests.cpp - Masked vector input demand -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Recover only incoming vector bytes that survive the machine's masks.

#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/lift/ARMRegs.h"

namespace neverd {
void detectFPParams(
    MedFunc &Func, const MedBlock &Entry, const TargetRegInfo &TRI,
    const std::map<std::pair<uint64_t, uint16_t>, int> &RegVarMap,
    Arch TargetArch, BinaryFormat TargetFormat);
} // namespace neverd

using namespace neverd;

namespace {
MedVar value(MedVar::VarKind Kind, int Id, uint16_t Size,
             uint64_t Register = 0) {
  MedVar V;
  V.Kind = Kind;
  V.Id = Id;
  V.Size = Size;
  V.RegOff = Register;
  V.TheArch = Arch::ARM;
  return V;
}

MedFunc maskedInput(uint64_t Mask, bool ObserveBeforeMask,
                    bool DuplicateMask = false, uint64_t TransitReg = 0,
                    bool ReturnTransit = false,
                    NdOp MaskOpcode = NdOp::INT_AND) {
  MedFunc F;
  F.Blocks.emplace_back();
  auto &B = F.Blocks.front();
  B.Id = 0;
  auto Add = [&](NdOp Opcode, MedVar Output,
                 std::initializer_list<MedVar> Inputs) {
    MedOp Op;
    Op.Opcode = Opcode;
    Op.Output = Output;
    for (const auto &Input : Inputs)
      Op.addInput(Input);
    B.Ops.push_back(Op);
  };
  const auto Incoming = value(MedVar::Reg, 10, 8, armreg::D(4));
  const auto Byte = value(MedVar::Temp, 11, 1);
  const auto Wide = value(MedVar::Temp, 12, 4);
  const auto Shifted = value(MedVar::Temp, 13, 4);
  const auto MaskHalf = value(MedVar::Temp, 14, 8);
  const auto MaskReg = value(MedVar::Reg, 15, 16, armreg::D(16));
  const auto MaskLane = value(MedVar::Temp, 16, 4);
  const auto Masked = value(MedVar::Temp, 17, 4);
  const auto Transit = value(MedVar::Reg, 26, 4, TransitReg);
  Add(NdOp::COPY, Incoming, {Incoming});
  Add(NdOp::SUBBYTES, Byte, {Incoming, MedVar::makeConst(0, 4)});
  Add(NdOp::INT_ZEXT, Wide, {Byte});
  Add(NdOp::INT_LEFT, Shifted, {Wide, MedVar::makeConst(8, 4)});
  if (TransitReg)
    Add(NdOp::COPY, Transit, {Shifted});
  Add(NdOp::COPY, MaskHalf, {MedVar::makeConst(Mask, 8)});
  Add(NdOp::CONCAT, MaskReg, {MaskHalf, MaskHalf});
  if (DuplicateMask)
    Add(NdOp::COPY, MaskReg, {value(MedVar::Reg, 25, 16, armreg::D(18))});
  if (ObserveBeforeMask)
    Add(NdOp::STORE, {}, {MedVar::makeConst(0x1000, 4), Shifted});
  Add(NdOp::SUBBYTES, MaskLane, {MaskReg, MedVar::makeConst(0, 4)});
  Add(MaskOpcode, Masked, {TransitReg ? Transit : Shifted, MaskLane});
  if (TransitReg && !ReturnTransit)
    Add(NdOp::COPY, value(MedVar::Reg, 27, 4, TransitReg),
        {MedVar::makeConst(0, 4)});
  Add(NdOp::COPY, value(MedVar::Reg, 18, 4, armreg::R0), {Masked});
  Add(NdOp::RETURN, {}, {value(MedVar::Reg, 18, 4, armreg::R0)});
  detectFPParams(F, B, getTargetRegInfo(Arch::ARM),
                 {{{armreg::D(4), 8}, Incoming.Id}}, Arch::ARM,
                 BinaryFormat::ELF);
  return F;
}
} // namespace

TEST(FPParameterMask, ConstantVectorMaskDoesNotObserveTheClearedInputByte) {
  EXPECT_TRUE(maskedInput(0xff, false).Params.empty());
  EXPECT_EQ(maskedInput(0xffff, false).Params.size(), 1u);
}

TEST(FPParameterMask, EarlierStoresAndAmbiguousMaskDefinitionsKeepTheInput) {
  EXPECT_EQ(maskedInput(0xff, true).Params.size(), 1u);
  EXPECT_EQ(maskedInput(0xff, false, true).Params.size(), 1u);
}

TEST(FPParameterMask, ScratchTransportIncludesD0AndTheUpperNeonRegisters) {
  for (uint64_t Register :
       {armreg::D(0), armreg::D(16), armreg::D(30), armreg::D(31)}) {
    SCOPED_TRACE(Register);
    EXPECT_TRUE(getTargetRegInfo(Arch::ARM).isVectorReg(Register));
    EXPECT_TRUE(maskedInput(0xff, false, false, Register).Params.empty());
    EXPECT_EQ(maskedInput(0xffff, false, false, Register).Params.size(), 1u);
  }
  EXPECT_FALSE(getTargetRegInfo(Arch::ARM).isVectorReg(armreg::D(32)));
}

TEST(FPParameterMask, AnUnmaskedValueReachingTheFpReturnIsStillObserved) {
  EXPECT_EQ(maskedInput(0xff, false, false, armreg::D(0), true).Params.size(),
            1u);
  EXPECT_EQ(maskedInput(0xff, false, false, armreg::R4).Params.size(), 1u);
}

TEST(FPParameterMask, OnlyCompleteConstantBytesDiscardTheInput) {
  EXPECT_EQ(maskedInput(0x100, false).Params.size(), 1u);
  EXPECT_TRUE(
      maskedInput(0xff00, false, false, 0, false, NdOp::INT_OR).Params.empty());
  EXPECT_EQ(
      maskedInput(0xfe00, false, false, 0, false, NdOp::INT_OR).Params.size(),
      1u);
}

TEST(FPParameterMask, FpReturnDemandFollowsEveryPredecessor) {
  for (bool ClearBothPaths : {false, true}) {
    MedFunc F = maskedInput(0xff, false, false, armreg::D(0), true);
    MedOp Return = F.Blocks[0].Ops.back();
    F.Blocks[0].Ops.pop_back();
    F.Blocks.resize(4);
    for (int I = 0; I != 4; ++I)
      F.Blocks[I].Id = I;
    F.Blocks[0].Succs = {1, 2};
    for (int I : {1, 2}) {
      F.Blocks[I].Preds = {0};
      F.Blocks[I].Succs = {3};
      if (I == 1 || ClearBothPaths) {
        MedOp Clear;
        Clear.Opcode = NdOp::COPY;
        Clear.Output = value(MedVar::Reg, 30 + I, 4, armreg::D(0));
        Clear.addInput(MedVar::makeConst(0, 4));
        F.Blocks[I].Ops.push_back(Clear);
      }
    }
    F.Blocks[3].Preds = {1, 2};
    F.Blocks[3].Ops.push_back(Return);
    F.Params.clear();
    F.FPParamScalarBytes.clear();
    detectFPParams(F, F.Blocks[0], getTargetRegInfo(Arch::ARM),
                   {{{armreg::D(4), 8}, 10}}, Arch::ARM, BinaryFormat::ELF);
    EXPECT_EQ(F.Params.size(), ClearBothPaths ? 0u : 1u);
  }
}
