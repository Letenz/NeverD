//===- X86SystemStateTests.cpp - System completion regression tests -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Strict decode, explicit operands, conditional destinations and refusal to
/// invent hardware answers. No privileged host instruction is executed.
//===----------------------------------------------------------------------===//

#include "X86SystemStateFixtures.h"
#include "gtest/gtest.h"

#include "neverd/backend/llvm/X86SystemStateAsm.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Lifter.h"
#include "neverd/lift/X86Regs.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <string>
#include <utility>
#include <vector>

using namespace neverd;

namespace {

std::vector<LowOp> lift(Arch A, const std::vector<uint8_t> &Bytes) {
  Decoder D;
  EXPECT_TRUE(D.init(A));
  DecodedInsn I{};
  EXPECT_EQ(D.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, I),
            Bytes.size());
  std::vector<LowOp> Ops;
  try {
    D.liftToLow(I, Ops);
  } catch (const std::exception &E) {
    ADD_FAILURE() << E.what();
  }
  return Ops;
}

TEST(X86SystemState, InstructionsHaveValidatedEffectsAndNoInventedResult) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto &E : test::systemEncodings(A)) {
      SCOPED_TRACE(E.Mnemonic);
      SCOPED_TRACE(static_cast<unsigned>(A));
      const auto Ops = lift(A, E.Bytes);
      ASSERT_FALSE(Ops.empty());
      unsigned Count = 0;
      for (const auto &Op : Ops) {
        EXPECT_NE(Op.Opcode, NdOp::NOP);
        if (Op.Opcode != NdOp::INTRINSIC)
          continue;
        ++Count;
        EXPECT_EQ(Op.Inputs[0].Offset, static_cast<uint64_t>(E.Id));
        EXPECT_TRUE(
            x86SystemStateShapeIsValid(E.Id, x86SystemStateLowShape(Op, A)));
        EXPECT_TRUE(isSideeffectIntrinsic(E.Id));
        BinaryImage Image;
        Image.Arch = A;
        NdOpEmulator Emu(Image);
        Emu.setRegister(x86reg::RAX, 0xabcdef0123456789);
        EXPECT_FALSE(Emu.step(Op));
        EXPECT_EQ(Emu.getRegister(x86reg::RAX), 0xabcdef0123456789);
        auto Bad = Op;
        Bad.Output.Size = 3;
        EXPECT_FALSE(
            x86SystemStateShapeIsValid(E.Id, x86SystemStateLowShape(Bad, A)));
      }
      EXPECT_EQ(Count, 1u);
    }
}

TEST(X86SystemState, WaitReadsControlAndBothDeadlineHalvesAndClearsOtherFlags) {
  for (const auto Prefix : {0x66, 0xf2}) {
    const auto Ops =
        lift(Arch::X64, {static_cast<uint8_t>(Prefix), 0x0f, 0xae, 0xf1});
    ASSERT_FALSE(Ops.empty());
    const auto &Wait = Ops.front();
    ASSERT_EQ(Wait.Opcode, NdOp::INTRINSIC);
    ASSERT_EQ(Wait.NumInputs, 4);
    EXPECT_EQ(Wait.Inputs[1], NdVar::reg(x86reg::RCX, 4));
    EXPECT_EQ(Wait.Inputs[2], NdVar::reg(x86reg::RAX, 4));
    EXPECT_EQ(Wait.Inputs[3], NdVar::reg(x86reg::RDX, 4));
    EXPECT_EQ(Wait.Output, NdVar::reg(x86reg::CF, 1));
    for (uint64_t Result : {0, 1}) {
      BinaryImage Image;
      Image.Arch = Arch::X64;
      NdOpEmulator Emu(Image);
      for (const auto Flag : {x86reg::CF, x86reg::ZF, x86reg::OF, x86reg::SF,
                              x86reg::AF, x86reg::PF})
        Emu.setRegister(Flag, 1);
      Emu.setRegister(x86reg::CF, Result);
      for (size_t I = 1; I < Ops.size(); ++I)
        ASSERT_TRUE(Emu.step(Ops[I]));
      EXPECT_EQ(Emu.getRegister(x86reg::CF), Result);
      for (const auto Flag :
           {x86reg::ZF, x86reg::OF, x86reg::SF, x86reg::AF, x86reg::PF})
        EXPECT_EQ(Emu.getRegister(Flag), 0u);
    }
  }
}

TEST(X86SystemState, LarFailurePreservesTheCompleteDestination) {
  const auto Ops = lift(Arch::X64, {0x0f, 0x02, 0xc1});
  ASSERT_FALSE(Ops.empty());
  ASSERT_EQ(Ops.front().Opcode, NdOp::INTRINSIC);
  for (uint64_t Success : {0, 1}) {
    BinaryImage Image;
    Image.Arch = Arch::X64;
    NdOpEmulator Emu(Image);
    Emu.setRegister(x86reg::RAX, 0xabcdef0123456789);
    // Supply a completion at the explicit environment boundary, then check
    // only the lifter's independent destination and flag projection.
    LowOp Completion = Ops.front();
    Completion.Opcode = NdOp::COPY;
    Completion.NumInputs = 1;
    Completion.Inputs[0] = NdVar::scalar((Success << 32) | 0x809b00, 8);
    ASSERT_TRUE(Emu.step(Completion));
    for (size_t I = 1; I < Ops.size(); ++I)
      ASSERT_TRUE(Emu.step(Ops[I]));
    EXPECT_EQ(Emu.getRegister(x86reg::RAX),
              Success ? 0x809b00 : 0xabcdef0123456789);
    EXPECT_EQ(Emu.getRegister(x86reg::ZF), Success);
  }
}

TEST(X86SystemState,
     InstructionOwnedMemoryIsNeverPreloadedOrUnconditionallyStored) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto &E : test::systemEncodings(A)) {
      if (x86SystemStateContract(E.Id).MemoryArgument < 0)
        continue;
      SCOPED_TRACE(E.Mnemonic);
      const auto Ops = lift(A, E.Bytes);
      EXPECT_TRUE(std::none_of(Ops.begin(), Ops.end(), [](const LowOp &Op) {
        return Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE;
      }));
    }
}

TEST(X86SystemMemory, SharedContractRetainsAddressesWidthAndRealCompletion) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto &E : test::systemMemoryEncodings(A)) {
      SCOPED_TRACE(E.Mnemonic);
      SCOPED_TRACE(static_cast<unsigned>(A));
      const auto Ops = lift(A, E.Bytes);
      const auto It = std::find_if(Ops.begin(), Ops.end(), [&](const auto &Op) {
        return Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
               Op.Inputs[0].Offset == static_cast<uint64_t>(E.Id);
      });
      ASSERT_NE(It, Ops.end());
      EXPECT_TRUE(std::none_of(Ops.begin(), Ops.end(), [](const LowOp &Op) {
        return Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE ||
               Op.Opcode == NdOp::NOP;
      }));
      const auto Shape = x86SystemStateLowShape(*It, A);
      EXPECT_TRUE(x86SystemStateShapeIsValid(E.Id, Shape));
      const bool Enqueue =
          E.Id == Intrinsic::Enqcmd || E.Id == Intrinsic::Enqcmds;
      const bool Block = Enqueue || E.Id == Intrinsic::X86DirectStore64B;
      EXPECT_EQ(It->Output.Size, Enqueue ? 1 : 0);
      const auto Plan = x86SystemStateAsm(
          E.Id, A, x86SystemStateWidth(E.Id, Shape), Shape.MemoryAddressSpace);
      ASSERT_TRUE(Plan);
      EXPECT_TRUE(Plan->Text.starts_with(E.Mnemonic));
      EXPECT_NE(Plan->Constraints.find("~{memory}"), std::string::npos);
      EXPECT_EQ(Plan->ChangesFlags, Enqueue);
      EXPECT_EQ(Plan->Text.find("setz") != std::string::npos, Enqueue);
      EXPECT_EQ(
          x86SystemStateMemoryBytes(E.Id, x86SystemStateWidth(E.Id, Shape)),
          Block ? 64 : It->Inputs[2].Size);
      for (unsigned Fault = 0; Fault != 7; ++Fault) {
        auto Bad = Shape;
        switch (Fault) {
        case 0:
          Bad.ValidId = false;
          break;
        case 1:
          Bad.Arguments = 1;
          break;
        case 2:
          Bad.InputSizes[0] = 4;
          break;
        case 3:
          Bad.InputSizes[1] = 2;
          break;
        case 4:
          Bad.ScalarInputs[1] = false;
          break;
        case 5:
          Bad.OutputSize = 4;
          break;
        case 6:
          Bad.MemoryAddressSpace = static_cast<NdMemoryAddressSpace>(255);
          break;
        }
        EXPECT_FALSE(x86SystemStateShapeIsValid(E.Id, Bad));
      }
      if (Block || It->Inputs[2].Size == 8) {
        auto Bad = Shape;
        Bad.TargetArch = Arch::X86;
        EXPECT_FALSE(x86SystemStateShapeIsValid(E.Id, Bad));
      }
    }
}

TEST(X86DirectStore, ScalarMemoryProjectionKeepsWidthWrappingSegmentsAndFlags) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto &E : test::directStoreEncodings(A)) {
      if (E.Id != Intrinsic::X86DirectStore)
        continue;
      const auto Ops = lift(A, E.Bytes);
      ASSERT_FALSE(Ops.empty());
      const auto &Store = Ops.back();
      ASSERT_EQ(Store.Opcode, NdOp::INTRINSIC);
      const unsigned Width = Store.Inputs[2].Size;
      const uint64_t Base =
          Store.MemoryAddressSpace == NdMemoryAddressSpace::Default ? 0
                                                                    : 0x100000;
      for (unsigned Misalignment : {0, 1}) {
        BinaryImage Image;
        Image.Arch = A;
        Segment Memory;
        Memory.VA = Base + 0x3000;
        Memory.Size = Memory.FileSz = 64;
        Memory.Data.assign(64, 0x5a);
        Memory.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
        Image.Segments.push_back(Memory);
        NdOpEmulator Emu(Image);
        Emu.setStrictMode(true);
        if (Base)
          ASSERT_TRUE(
              Emu.setMemoryAddressSpaceBase(Store.MemoryAddressSpace, Base));
        Emu.setRegister(x86reg::RAX, 0x3010 + Misalignment);
        Emu.setRegister(x86reg::RBX, 0xfff0);
        Emu.setRegister(x86reg::RSI, 0x3020 + Misalignment);
        constexpr uint64_t Value = 0xfedcba9876543210;
        Emu.setRegister(x86reg::RCX, Value);
        for (uint64_t F : {x86reg::CF, x86reg::ZF, x86reg::OF, x86reg::SF,
                           x86reg::AF, x86reg::PF})
          Emu.setRegister(F, 1);
        ASSERT_EQ(Emu.run(Ops), Ops.size());
        LowOp Read;
        Read.Opcode = NdOp::LOAD;
        Read.Output = NdVar::reg(x86reg::XMM0, 64);
        Read.addInput(NdVar::cst(Memory.VA, 8));
        ASSERT_TRUE(Emu.step(Read));
        const auto Actual = Emu.getRegisterBytes(x86reg::XMM0);
        ASSERT_TRUE(Actual);
        auto Expected = Memory.Data;
        for (unsigned I = 0; I != Width; ++I)
          Expected[16 + Misalignment + I] = Value >> (8 * I);
        EXPECT_EQ(*Actual, Expected);
        for (uint64_t F : {x86reg::CF, x86reg::ZF, x86reg::OF, x86reg::SF,
                           x86reg::AF, x86reg::PF})
          EXPECT_EQ(Emu.getRegister(F), 1u);
        EXPECT_FALSE(Emu.skips().any());
      }
    }
}

TEST(X86DirectStore, ForgedOperandDetailsRefuseBeforeEmission) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Arch::X64));
  for (const auto &E : test::directStoreEncodings(Arch::X64)) {
    DecodedInsn I{};
    ASSERT_EQ(
        Decode.decodeOneForLift(E.Bytes.data(), E.Bytes.size(), 0x1000, I),
        E.Bytes.size());
    const bool Block = E.Id == Intrinsic::X86DirectStore64B;
    for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
      cs_insn Raw = *I.Raw;
      cs_detail Detail = *Raw.detail;
      Raw.detail = &Detail;
      auto &X = Detail.x86;
      switch (Mutation) {
      case 0:
        X.operands[Block ? 0 : 1].reg = X86_REG_XMM0;
        break;
      case 1:
        X.operands[Block ? 1 : 0].size = 2;
        break;
      case 2:
        X.operands[Block ? 1 : 0].mem.base = X86_REG_R15;
        break;
      case 3:
        X.op_count = 1;
        break;
      case 4:
        X.addr_size = 2;
        break;
      case 5:
        X.prefix[0] = 0xf0;
        break;
      }
      X86Lifter L(Arch::X64);
      L.setStrict(true);
      std::vector<LowOp> Ops;
      EXPECT_THROW(L.lift(&Raw, Ops), UnliftedInstruction);
      EXPECT_TRUE(Ops.empty());
    }
  }
}

TEST(X86MaskedStore, SelectedLanesWriteWithoutTouchingMaskedUnmappedMemory) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const bool Integer : {false, true})
      for (const bool Qword : {false, true})
        for (const bool Wide : {false, true}) {
          const uint16_t Lane = Qword ? 8 : 4;
          const uint16_t Size = Wide ? 32 : 16;
          // [eax/rax], xmm/ymm5, xmm/ymm2. Floating forms select element width
          // by opcode; integer forms select it by VEX.W.
          const auto Ops =
              lift(A, {0xc4, 0xe2,
                       static_cast<uint8_t>(0x51 | (Wide ? 4 : 0) |
                                            (Integer && Qword ? 0x80 : 0)),
                       static_cast<uint8_t>(Integer ? 0x8e
                                            : Qword ? 0x2f
                                                    : 0x2e),
                       0x10});
          ASSERT_FALSE(Ops.empty());
          BinaryImage Image;
          Image.Arch = A;
          Segment Memory;
          Memory.VA = 0x4000;
          Memory.Size = Memory.FileSz = 2 * Lane;
          Memory.Data.assign(2 * Lane, 0x55);
          Memory.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
          Image.Segments.push_back(std::move(Memory));
          NdOpEmulator Emu(Image);
          Emu.setStrictMode(true);
          Emu.setRegister(x86reg::RAX, 0x4000);
          std::vector<uint8_t> Mask(Size, 0x7f), Data(Size, 0xa5);
          Mask[Lane - 1] = 0x80;
          Emu.setRegisterBytes(x86reg::XMM5, Mask);
          Emu.setRegisterBytes(x86reg::XMM2, Data);
          for (const auto &Op : Ops) {
            EXPECT_FALSE(Op.Output.isReg() && Op.Output.Offset == x86reg::RAX);
            ASSERT_TRUE(Emu.step(Op));
          }
          EXPECT_EQ(Emu.getRegister(x86reg::RAX), 0x4000u);
          LowOp Read;
          Read.Opcode = NdOp::LOAD;
          Read.Output = NdVar::reg(x86reg::XMM0, 2 * Lane);
          Read.addInput(NdVar::scalar(0x4000, 8));
          ASSERT_TRUE(Emu.step(Read));
          const auto Bytes = Emu.getRegisterBytes(x86reg::XMM0);
          ASSERT_TRUE(Bytes);
          for (unsigned I = 0; I != 2 * Lane; ++I)
            EXPECT_EQ((*Bytes)[I], I < Lane ? 0xa5 : 0x55);
        }
}

} // namespace
