//===- MedDceImplicitSpTests.cpp - Implicit call-frame liveness ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/LowToMed.h"

#include <algorithm>
#include <limits>

namespace {
using namespace neverd;

MedVar reg(int Id, int Version, uint16_t Size, uint64_t Offset) {
  MedVar Value;
  Value.Kind = MedVar::Reg;
  Value.TheArch = Arch::X86;
  Value.Id = Id;
  Value.SSAVer = Version;
  Value.Size = Size;
  Value.RegOff = Offset;
  return Value;
}

TEST(MedDceImplicitSp, KeepsPredecessorStackPointerAtCall) {
  const auto &TRI = getTargetRegInfo(Arch::X86);
  MedFunc Func;
  Func.Blocks.resize(2);
  MedBlock &Setup = Func.Blocks[0];
  Setup.Id = 0;
  Setup.Succs = {1};
  const MedVar EntrySP = reg(1, 0, 4, TRI.StackPointer);
  const MedVar CallSP = reg(1, 1, 4, TRI.StackPointer);
  MedOp Adjust;
  Adjust.Opcode = NdOp::INT_SUB;
  Adjust.Output = CallSP;
  Adjust.addInput(EntrySP);
  Adjust.addInput(MedVar::makeConst(4, 4));
  Setup.Ops.push_back(Adjust);

  MedBlock &CallBlock = Func.Blocks[1];
  CallBlock.Id = 1;
  CallBlock.Preds = {0};
  MedOp DeadCopy;
  DeadCopy.Opcode = NdOp::COPY;
  DeadCopy.Output.Kind = MedVar::Temp;
  DeadCopy.Output.Id = 2;
  DeadCopy.Output.SSAVer = 1;
  DeadCopy.Output.Size = 4;
  DeadCopy.addInput(MedVar::makeConst(7, 4));
  CallBlock.Ops.push_back(DeadCopy);
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Output.Kind = MedVar::Temp;
  Call.Output.Id = 3;
  Call.Output.SSAVer = 1;
  Call.Output.Size = 4;
  Call.addInput(MedVar::makeConst(0x2000, 4));
  CallBlock.Ops.push_back(Call);

  LowToMedConverter().runRegisterDce(Func, Arch::X86);
  ASSERT_EQ(Setup.Ops.size(), 1u);
  EXPECT_EQ(Setup.Ops.front().Output, CallSP);
  ASSERT_EQ(CallBlock.Ops.size(), 1u);
  EXPECT_EQ(CallBlock.Ops.front().Opcode, NdOp::CALL);
}

TEST(MedDCE, KeepsLivePhiCyclesAndDropsDeadCyclesInEitherBlockOrder) {
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
    for (bool Reverse : {false, true}) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(Reverse);
      // Sparse identifiers keep liveness independent of numbering density.
      auto value = [&](int Id) {
        MedVar V;
        V.Kind = MedVar::Temp;
        V.TheArch = Architecture;
        V.Id = Id;
        V.SSAVer = std::numeric_limits<int>::max();
        V.Size = 4;
        return V;
      };
      const MedVar LivePhi = value(7);
      const MedVar LiveNext = value(std::numeric_limits<int>::max());
      const MedVar DeadPhi = value(4000);
      const MedVar DeadNext = value(8000);
      MedFunc Func;
      Func.Blocks.resize(2);
      Func.Blocks[0].Id = 0;
      Func.Blocks[0].Succs = {1};
      auto &Loop = Func.Blocks[1];
      Loop.Id = 1;
      Loop.Preds = {0, 1};
      Loop.Succs = {1};
      for (const auto &[Phi, Next] :
           {std::pair{LivePhi, LiveNext}, std::pair{DeadPhi, DeadNext}}) {
        Loop.Phis.push_back({Phi, {{0, MedVar::makeConst(0, 4)}, {1, Next}}});
        MedOp Add;
        Add.Opcode = NdOp::INT_ADD;
        Add.Output = Next;
        Add.addInput(Phi);
        Add.addInput(MedVar::makeConst(1, 4));
        Loop.Ops.push_back(Add);
      }
      MedOp Store;
      Store.Opcode = NdOp::STORE;
      Store.Output.Id = -1;
      const uint16_t PointerBytes =
          Architecture == Arch::X64 || Architecture == Arch::AArch64 ? 8 : 4;
      Store.addInput(MedVar::makeConst(0x2000, PointerBytes));
      Store.addInput(LiveNext);
      Loop.Ops.push_back(Store);
      if (Reverse)
        std::reverse(Func.Blocks.begin(), Func.Blocks.end());

      LowToMedConverter().runRegisterDce(Func, Architecture);
      const auto &Kept =
          *std::find_if(Func.Blocks.begin(), Func.Blocks.end(),
                        [](const MedBlock &Block) { return Block.Id == 1; });
      ASSERT_EQ(Kept.Phis.size(), 1u);
      EXPECT_EQ(Kept.Phis.front().Output, LivePhi);
      ASSERT_EQ(Kept.Ops.size(), 2u);
      EXPECT_EQ(Kept.Ops.front().Output, LiveNext);
      EXPECT_EQ(Kept.Ops.back().Opcode, NdOp::STORE);
      EXPECT_EQ(Kept.Ops.back().Inputs[1], LiveNext);
    }
}

TEST(MedDCE, NegativeConstantNumberingDoesNotNameADefinition) {
  for (const auto &[Id, Version] :
       {std::pair{-1, -1}, std::pair{-2, -2}, std::pair{-3, 0}}) {
    MedFunc Func;
    Func.Blocks.resize(1);
    auto &Block = Func.Blocks.front();
    Block.Id = 0;
    MedVar Constant = MedVar::makeConst(0x4000, 4);
    Constant.Id = Id;
    Constant.SSAVer = Version;
    MedOp SetSP;
    SetSP.Opcode = NdOp::COPY;
    SetSP.Output = reg(7, 1, 4, getTargetRegInfo(Arch::X86).StackPointer);
    SetSP.addInput(Constant);
    Block.Ops.push_back(SetSP);
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Output.Id = -1;
    Block.Ops.push_back(Return);

    LowToMedConverter().runRegisterDce(Func, Arch::X86);
    ASSERT_EQ(Block.Ops.size(), 2u);
    EXPECT_EQ(Block.Ops.front().Output, SetSP.Output);
    EXPECT_EQ(Block.Ops.front().Inputs[0], Constant);
  }
}
} // namespace
