//===- MedConstantPropagationTests.cpp - Stable MedIR value propagation ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file Checks SSA value identity, widths, and bounded propagation.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedConstantPropagation.h"
#include "neverd/ir/med/MedIR.h"

#include <algorithm>
#include <limits>

using namespace neverd;
namespace {
MedVar variable(int Id, uint16_t Size = 8) {
  MedVar Value;
  Value.Id = Id;
  Value.SSAVer = 1;
  Value.Size = Size;
  return Value;
}
MedOp operation(NdOp Opcode, MedVar Output, std::vector<MedVar> Inputs) {
  MedOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  for (const auto &Input : Inputs)
    Op.addInput(Input);
  return Op;
}

MedFunc loop(MedVar Seed) {
  MedFunc Func;
  Func.Blocks.resize(4);
  for (int I = 0; I < 4; ++I)
    Func.Blocks[I].Id = I;
  auto &Entry = Func.Blocks[0];
  Entry.Succs = {1};
  Entry.Ops.push_back(operation(NdOp::COPY, variable(1, Seed.Size), {Seed}));
  auto &Header = Func.Blocks[1];
  Header.Preds = {0, 2};
  Header.Succs = {2, 3};
  Header.Phis.push_back(
      {variable(2, Seed.Size),
       {{0, variable(1, Seed.Size)}, {2, variable(3, Seed.Size)}}});
  auto &Backedge = Func.Blocks[2];
  Backedge.Preds = {1};
  Backedge.Succs = {1};
  Backedge.Ops.push_back(
      operation(NdOp::COPY, variable(3, Seed.Size), {variable(2, Seed.Size)}));
  auto &Exit = Func.Blocks[3];
  Exit.Preds = {1};
  Exit.Ops.push_back(operation(NdOp::RETURN, {}, {variable(2, Seed.Size)}));
  return Func;
}
MedBlock &block(MedFunc &Func, int Id) {
  return *std::find_if(Func.Blocks.begin(), Func.Blocks.end(),
                       [&](const auto &Block) { return Block.Id == Id; });
}
const MedVar &result(MedFunc &Func) {
  return block(Func, 3).Ops.back().Inputs[0];
}

TEST(MedConstantPropagation,
     SeededCyclesConvergeIndependentlyOfTraversalOrder) {
  for (uint16_t Size : {1, 2, 4, 8})
    for (bool ReverseBlocks : {false, true})
      for (bool ReverseArguments : {false, true}) {
        const auto Seed = MedVar::makeConst(73, Size);
        auto Func = loop(Seed);
        if (ReverseBlocks)
          std::reverse(Func.Blocks.begin(), Func.Blocks.end());
        auto &Arguments = block(Func, 1).Phis.front().Args;
        if (ReverseArguments)
          std::reverse(Arguments.begin(), Arguments.end());
        ASSERT_TRUE(propagateInvariantConstants(Func));
        EXPECT_EQ(result(Func), Seed);
        EXPECT_FALSE(propagateInvariantConstants(Func));
      }
}

TEST(MedConstantPropagation, UnknownAndUnseededArmsInvalidateProvisionalFacts) {
  for (bool ReverseBlocks : {false, true}) {
    auto Func = loop(MedVar::makeConst(73, 8));
    auto &Backedge = block(Func, 2);
    // A separate, unseeded cycle feeds an otherwise seeded merge. Pending
    // values cannot be treated as irrelevant alternatives to the known seed.
    Backedge.Ops = {operation(NdOp::COPY, variable(3), {variable(4)}),
                    operation(NdOp::COPY, variable(4), {variable(3)})};
    if (ReverseBlocks)
      std::reverse(Func.Blocks.begin(), Func.Blocks.end());
    propagateInvariantConstants(Func);
    EXPECT_FALSE(result(Func).isConst());
  }
  for (auto Opcode : {NdOp::COPY, NdOp::LOAD, NdOp::CALL, NdOp::INT_ADD}) {
    auto Func = loop(variable(9));
    auto &Seed = block(Func, 0).Ops.front();
    Seed.Opcode = Opcode;
    if (Opcode != NdOp::COPY)
      Seed.Inputs[0] = MedVar::makeConst(73, 8);
    if (Opcode == NdOp::INT_ADD)
      Seed.addInput(MedVar::makeConst(0, 8));
    propagateInvariantConstants(Func);
    EXPECT_FALSE(result(Func).isConst());
    EXPECT_EQ(Seed.Opcode, Opcode);
  }
}

TEST(MedConstantPropagation, EqualBitsRetainWidthProvenanceAndOwnerIdentity) {
  const auto Seed = MedVar::makeConst(
      0x123000, 8, ConstantAddressProvenance::DataAddress, 0x120000);
  auto Control = loop(Seed);
  ASSERT_TRUE(propagateInvariantConstants(Control));
  EXPECT_EQ(result(Control), Seed);
  for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
    auto Func = loop(Seed);
    auto Alternative = Seed;
    if (Mutation == 0)
      Alternative.ConstVal += 8;
    else if (Mutation == 1)
      Alternative.Provenance = ConstantAddressProvenance::Scalar;
    else if (Mutation == 2)
      Alternative.AddressOwnerVA += 8;
    else
      Alternative.Size = 4;
    block(Func, 2).Ops.front().Inputs[0] = Alternative;
    propagateInvariantConstants(Func);
    EXPECT_FALSE(result(Func).isConst()) << Mutation;
  }
}

TEST(MedConstantPropagation,
     IncompleteEdgesAndDuplicateDefinitionsSupplyNoFacts) {
  for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
    auto Func = loop(MedVar::makeConst(73, 8));
    auto &Header = block(Func, 1);
    if (Mutation == 0)
      Header.Phis.front().Args.pop_back();
    else if (Mutation == 1)
      Header.Phis.front().Args.back().first = 0;
    else if (Mutation == 2)
      Header.Preds.pop_back();
    else if (Mutation == 3)
      block(Func, 0).Ops.push_back(
          operation(NdOp::COPY, variable(1), {MedVar::makeConst(91, 8)}));
    else
      block(Func, 2).Ops.front().MemoryOrdering = NdMemoryOrdering::Acquire;
    propagateInvariantConstants(Func);
    EXPECT_FALSE(result(Func).isConst()) << Mutation;
  }
  auto Func = loop(MedVar::makeConst(73, 8));
  block(Func, 0).Succs.push_back(99);
  EXPECT_FALSE(propagateInvariantConstants(Func));
  EXPECT_FALSE(result(Func).isConst());
}

TEST(MedConstantPropagation, EffectsAndOperandRolesSurviveConstantTransport) {
  auto Func = loop(MedVar::makeConst(73, 8));
  auto &Exit = block(Func, 3);
  Exit.Ops.insert(Exit.Ops.begin(),
                  operation(NdOp::STORE, {}, {variable(2), variable(9)}));
  Exit.Ops.insert(Exit.Ops.begin(),
                  operation(NdOp::CALL, variable(5), {variable(2)}));
  Exit.Ops.insert(Exit.Ops.begin(),
                  operation(NdOp::INT_ADD, variable(6),
                            {variable(2), MedVar::makeConst(1, 8)}));
  ASSERT_TRUE(propagateInvariantConstants(Func));
  ASSERT_EQ(Exit.Ops.size(), 4U);
  EXPECT_EQ(Exit.Ops[0].Inputs[0].Provenance,
            ConstantAddressProvenance::Scalar);
  EXPECT_EQ(Exit.Ops[1].Opcode, NdOp::CALL);
  EXPECT_EQ(Exit.Ops[1].Inputs[0].Provenance,
            ConstantAddressProvenance::Unknown);
  EXPECT_EQ(Exit.Ops[2].Opcode, NdOp::STORE);
  EXPECT_EQ(Exit.Ops[2].Inputs[0].Provenance,
            ConstantAddressProvenance::Unknown);
  EXPECT_EQ(Exit.Ops[2].Inputs[1], variable(9));
}

TEST(MedConstantPropagation, BudgetExhaustionLeavesAllOperandsUnchanged) {
  auto Func = loop(MedVar::makeConst(73, 8));
  for (int I = 10; I < 65546; ++I)
    block(Func, 0).Ops.push_back(
        operation(NdOp::COPY, variable(I), {variable(1)}));
  EXPECT_FALSE(propagateInvariantConstants(Func));
  EXPECT_EQ(result(Func), variable(2));
  EXPECT_EQ(block(Func, 0).Ops.back().Inputs[0], variable(1));
}

TEST(MedConstantPropagation,
     FullSsaIdentityKeepsMaximumIdsAndVersionsDistinct) {
  MedFunc Func;
  Func.Blocks.resize(1);
  auto &Block = Func.Blocks.front();
  Block.Id = 0;
  auto A = variable(std::numeric_limits<int>::max());
  A.Kind = MedVar::Temp;
  A.SSAVer = std::numeric_limits<int>::max();
  auto B = A;
  B.Kind = MedVar::Reg;
  auto C = B;
  --C.SSAVer;
  auto Unknown = variable(-1);
  Unknown.Kind = MedVar::Temp;
  Unknown.SSAVer = -1;
  Block.Ops = {operation(NdOp::COPY, A, {MedVar::makeConst(7, 8)}),
               operation(NdOp::COPY, B, {MedVar::makeConst(11, 8)}),
               operation(NdOp::COPY, C, {MedVar::makeConst(13, 8)}),
               operation(NdOp::INT_ADD, variable(10), {A, B}),
               operation(NdOp::INT_ADD, variable(11), {Unknown, C}),
               operation(NdOp::RETURN, {}, {C})};
  ASSERT_TRUE(propagateInvariantConstants(Func));
  EXPECT_EQ(Block.Ops[3].Inputs[0],
            MedVar::makeConst(7, 8, ConstantAddressProvenance::Scalar));
  EXPECT_EQ(Block.Ops[3].Inputs[1],
            MedVar::makeConst(11, 8, ConstantAddressProvenance::Scalar));
  EXPECT_EQ(Block.Ops[4].Inputs[0], Unknown);
  EXPECT_EQ(Block.Ops[4].Inputs[1],
            MedVar::makeConst(13, 8, ConstantAddressProvenance::Scalar));
  EXPECT_EQ(Block.Ops.back().Inputs[0], MedVar::makeConst(13, 8));
}

TEST(MedConstantPropagation, ReverseWidthChainKeepsAtomicBudgetRefusal) {
  for (const int Count : {600, 2000}) {
    SCOPED_TRACE(Count);
    MedFunc Func;
    Func.Blocks.resize(1);
    auto &Block = Func.Blocks.front();
    Block.Id = 0;
    for (int Id = Count; Id >= 0; --Id)
      Block.Ops.push_back(operation(
          Id == Count ? NdOp::INT_ZEXT : NdOp::COPY, variable(Id, 4),
          {Id == Count ? MedVar::makeConst(7, 1) : variable(Id + 1, 4)}));
    Block.Ops.push_back(operation(NdOp::RETURN, {}, {variable(0, 4)}));
    const auto Original = Block.Ops;
    const bool Complete = Count == 600;
    EXPECT_EQ(propagateInvariantConstants(Func), Complete);
    EXPECT_EQ(Block.Ops.back().Inputs[0],
              Complete ? MedVar::makeConst(7, 4) : variable(0, 4));
    if (!Complete)
      for (size_t I = 0; I < Block.Ops.size(); ++I)
        EXPECT_EQ(Block.Ops[I].Inputs[0], Original[I].Inputs[0]);
  }
}

TEST(MedConstantPropagation,
     LowConversionPreservesOneValueAcrossLoopDefinitions) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::MachO, BinaryFormat::COFF}) {
      const auto &TRI = getTargetRegInfo(Architecture);
      const auto Held = NdVar::reg(TRI.IntReturnReg, TRI.PointerSize);
      const auto Anchor = NdVar::reg(TRI.IntParamRegs[1], TRI.PointerSize);
      const auto Count = NdVar::reg(TRI.IntParamRegs[2], TRI.PointerSize);
      const auto Condition = NdVar::tmp(0, 1);
      LowFunc Low;
      Low.Entry = 0x1000;
      Low.Name = "invariant_loop_return";
      Low.Blocks.resize(4);
      for (int I = 0; I < 4; ++I) {
        Low.Blocks[I].Id = I;
        Low.Blocks[I].StartAddr = 0x1000 + 4 * I;
        Low.Blocks[I].EndAddr = Low.Blocks[I].StartAddr + 4;
      }
      Low.Blocks[0].Succs = {1};
      Low.Blocks[1].Preds = {0, 2};
      Low.Blocks[1].Succs = {2, 3};
      Low.Blocks[2].Preds = {1};
      Low.Blocks[2].Succs = {1};
      Low.Blocks[3].Preds = {1};
      auto Add = [&](int Block, NdOp Opcode, NdVar Output,
                     std::initializer_list<NdVar> Inputs) {
        LowOp Op;
        Op.Opcode = Opcode;
        Op.Output = Output;
        Op.Addr = Low.Blocks[Block].StartAddr;
        for (const auto &Input : Inputs)
          Op.addInput(Input);
        Low.Blocks[Block].Ops.push_back(Op);
      };
      Add(0, NdOp::COPY, Held, {NdVar::cst(73, TRI.PointerSize)});
      Add(0, NdOp::COPY, Anchor, {Held});
      Add(0, NdOp::BRANCH, {}, {NdVar::cst(0x1004, TRI.PointerSize)});
      Add(1, NdOp::INT_EQUAL, Condition,
          {Count, NdVar::cst(0, TRI.PointerSize)});
      Add(1, NdOp::COND_BR, {},
          {NdVar::cst(0x100c, TRI.PointerSize), Condition});
      Add(2, NdOp::INT_SUB, Count, {Count, NdVar::cst(1, TRI.PointerSize)});
      Add(2, NdOp::COPY, Held, {Anchor});
      Add(2, NdOp::BRANCH, {}, {NdVar::cst(0x1004, TRI.PointerSize)});
      Add(3, NdOp::RETURN, {}, {Held});
      auto Med = LowToMedConverter().convert(Low, Architecture, Format);
      ASSERT_TRUE(verifyMedFunc(Med, "invariant-loop-return"));
      const auto &Return = Med.Blocks.back().Ops.back();
      ASSERT_EQ(Return.Opcode, NdOp::RETURN);
      ASSERT_EQ(Return.NumInputs, 1U);
      EXPECT_EQ(Return.Inputs[0], MedVar::makeConst(73, TRI.PointerSize));
    }
}

TEST(MedConstantPropagation,
     DefaultWordAbsorptionRequiresTheSameSSAValueAndWidth) {
  for (auto Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::MachO, BinaryFormat::COFF})
      for (auto Outer : {NdOp::INT_AND, NdOp::INT_OR})
        for (bool Reverse : {false, true})
          for (unsigned Mutation = 0; Mutation < 3; ++Mutation) {
            SCOPED_TRACE(testing::Message()
                         << unsigned(Architecture) << ':' << unsigned(Format)
                         << ':' << unsigned(Outer) << ':' << Reverse << ':'
                         << Mutation);
            const auto &TRI = getTargetRegInfo(Architecture);
            const auto Width = TRI.PointerSize;
            const auto Arguments = TRI.integerArgumentLayout(Format).Registers;
            const auto Input = NdVar::reg(Arguments.empty() ? TRI.IntReturnReg
                                                            : Arguments.front(),
                                          Width);
            const auto Result = NdVar::reg(TRI.IntReturnReg, Width);
            LowFunc Low;
            Low.Entry = 0x1000;
            Low.Name = "word_absorption";
            Low.Blocks.resize(1);
            auto &Block = Low.Blocks.front();
            Block.Id = 0;
            Block.StartAddr = Low.Entry;
            Block.EndAddr = Low.Entry + 4;
            const auto Add = [&](NdOp Opcode, NdVar Output,
                                 std::initializer_list<NdVar> Inputs) {
              LowOp Op;
              Op.Opcode = Opcode;
              Op.Output = Output;
              Op.Addr = Low.Entry;
              for (const auto &Value : Inputs)
                Op.addInput(Value);
              Block.Ops.push_back(Op);
            };
            const auto Other = NdVar::tmp(0, Width);
            const auto Nested = NdVar::tmp(1, Width);
            Add(NdOp::INT_ADD, Other, {Input, Input});
            Add(Outer == NdOp::INT_AND ? NdOp::INT_OR : NdOp::INT_AND, Nested,
                {Reverse ? Other : Input, Reverse ? Input : Other});
            auto NestedOperand = Nested;
            if (Mutation == 1)
              Add(NdOp::INT_XOR, Input, {Input, NdVar::cst(0x55, Width)});
            if (Mutation == 2) {
              const auto Narrow = NdVar::tmp(2, Width / 2);
              NestedOperand = NdVar::tmp(3, Width);
              Add(NdOp::SUBBYTES, Narrow, {Nested, NdVar::cst(0, Width)});
              Add(NdOp::INT_ZEXT, NestedOperand, {Narrow});
            }
            Add(Outer, Result,
                {Reverse ? Input : NestedOperand,
                 Reverse ? NestedOperand : Input});
            Add(NdOp::RETURN, {}, {Result});
            const auto Med =
                LowToMedConverter().convert(Low, Architecture, Format);
            ASSERT_TRUE(verifyMedFunc(Med, "default-word-absorption"));
            bool RetainsOuter = false;
            for (const auto &B : Med.Blocks)
              for (const auto &Op : B.Ops)
                if (!Op.Dead)
                  RetainsOuter |= Op.Opcode == Outer;
            EXPECT_EQ(RetainsOuter, Mutation != 0);
          }
}
} // namespace
