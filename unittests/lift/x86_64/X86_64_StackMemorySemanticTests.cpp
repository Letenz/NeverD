#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/symbolic/SymExec.h"

#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

constexpr uint64_t InitialSp = 0x7800;
constexpr uint64_t Frame = 0x7300;
constexpr uint64_t Value = UINT64_C(0x1122334455667788);

struct Machine {
  SymContext Ctx;
  SymState State{Ctx};
  SymExec Exec{Ctx, State};
  void reg(uint64_t Offset, uint64_t Value) {
    State.write(SymSpace::Register, Offset, Ctx.mkConst(64, Value));
  }
  void memory(uint64_t Address, uint64_t Value, unsigned Size = 8) {
    State.store(Ctx.mkConst(64, Address), Ctx.mkConst(Size * 8, Value));
  }
  uint64_t value(SymRef Ref) {
    auto Constant = Ctx.asConst(Ref);
    EXPECT_TRUE(Constant.has_value()) << Ctx.toString(Ref);
    return Constant ? Constant->getZExtValue() : 0;
  }
  uint64_t getReg(uint64_t Offset, unsigned Size = 8) {
    return value(State.read(SymSpace::Register, Offset, Size));
  }
  uint64_t getMemory(uint64_t Address, unsigned Size = 8) {
    return value(State.load(Ctx.mkConst(64, Address), Size));
  }
  void run(Arch Target, std::initializer_list<uint8_t> Code) {
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Target));
    Dec.setStrict(true);
    std::vector<uint8_t> Bytes(Code);
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn),
              static_cast<int>(Bytes.size()));
    std::vector<LowOp> Ops;
    Dec.liftToLow(Insn, Ops);
    for (const auto &Op : Ops)
      Exec.step(Op);
    EXPECT_EQ(Exec.unmodelledCount(), 0u);
  }
};

TEST(X86StackMemorySemantics, PopToFrameMemoryUsesActualDestination) {
  Machine M;
  M.reg(x86reg::RSP, InitialSp);
  M.reg(x86reg::RBP, Frame);
  M.memory(InitialSp, Value);
  M.memory(Frame, uint64_t(0));
  M.run(Arch::X64, {0x8f, 0x45, 0x00});
  EXPECT_EQ(M.getMemory(Frame), Value);
  EXPECT_EQ(M.getReg(x86reg::RSP), InitialSp + 8);
}

TEST(X86StackMemorySemantics, PopToStackMemoryUsesIncrementedStackPointer) {
  Machine M;
  M.reg(x86reg::RSP, InitialSp);
  M.memory(InitialSp, Value);
  M.memory(InitialSp + 8, uint64_t(0));
  M.run(Arch::X64, {0x8f, 0x04, 0x24});
  EXPECT_EQ(M.getMemory(InitialSp + 8), Value);
  EXPECT_EQ(M.getMemory(InitialSp), Value);
  EXPECT_EQ(M.getReg(x86reg::RSP), InitialSp + 8);
}

TEST(X86StackMemorySemantics, PopToIndexedStackMemoryUsesIncrementedBase) {
  Machine M;
  M.reg(x86reg::RSP, InitialSp);
  M.reg(x86reg::RCX, 3);
  M.memory(InitialSp, Value);
  M.memory(InitialSp + 8 + 12 + 16, uint64_t(0));
  M.run(Arch::X64, {0x8f, 0x44, 0x8c, 0x10});
  EXPECT_EQ(M.getMemory(InitialSp + 8 + 12 + 16), Value);
}

TEST(X86StackMemorySemantics, PopWordToMemoryPreservesAdjacentBytes) {
  Machine M;
  M.reg(x86reg::RSP, InitialSp);
  M.reg(x86reg::RBP, Frame);
  M.memory(InitialSp, Value);
  M.memory(Frame, UINT64_C(0xaabbccddeeff0000));
  M.run(Arch::X64, {0x66, 0x8f, 0x45, 0x00});
  EXPECT_EQ(M.getMemory(Frame), UINT64_C(0xaabbccddeeff7788));
  EXPECT_EQ(M.getReg(x86reg::RSP), InitialSp + 2);
}

TEST(X86StackMemorySemantics, PopMemoryX86UsesFourByteStackWidth) {
  Machine M;
  M.reg(x86reg::RSP, InitialSp);
  M.reg(x86reg::RBP, Frame);
  M.memory(InitialSp, Value);
  M.memory(Frame, uint64_t(0));
  M.run(Arch::X86, {0x8f, 0x45, 0x00});
  EXPECT_EQ(M.getMemory(Frame), UINT64_C(0x55667788));
  EXPECT_EQ(M.getReg(x86reg::RSP, 4), InitialSp + 4);
}

TEST(X86StackMemorySemantics, PopStackRegisterKeepsPoppedValue) {
  Machine M;
  M.reg(x86reg::RSP, InitialSp);
  M.memory(InitialSp, Value);
  M.run(Arch::X64, {0x5c});
  EXPECT_EQ(M.getReg(x86reg::RSP), Value);
}

TEST(X86StackMemorySemantics, PushStackRegisterCapturesOriginalValue) {
  Machine M;
  M.reg(x86reg::RSP, InitialSp);
  M.memory(InitialSp - 8, uint64_t(0));
  M.run(Arch::X64, {0x54});
  EXPECT_EQ(M.getMemory(InitialSp - 8), InitialSp);
  EXPECT_EQ(M.getReg(x86reg::RSP), InitialSp - 8);
}

TEST(X86StackMemorySemantics, PushStackRegisterX86CapturesOriginalValue) {
  Machine M;
  M.reg(x86reg::RSP, InitialSp);
  M.memory(InitialSp - 4, uint64_t(0));
  M.run(Arch::X86, {0x54});
  EXPECT_EQ(M.getMemory(InitialSp - 4, 4), InitialSp);
  EXPECT_EQ(M.getReg(x86reg::RSP, 4), InitialSp - 4);
}

TEST(X86StackMemorySemantics, PushStackMemoryReadsBeforeStackAdjustment) {
  Machine M;
  M.reg(x86reg::RSP, InitialSp);
  M.memory(InitialSp, Value);
  M.run(Arch::X64, {0xff, 0x34, 0x24});
  EXPECT_EQ(M.getMemory(InitialSp - 8), Value);
  EXPECT_EQ(M.getReg(x86reg::RSP), InitialSp - 8);
}

struct FlagsStackCase {
  Arch Target;
  bool Word;
  unsigned Width;
};

class X86FlagsStackSemantics : public testing::TestWithParam<FlagsStackCase> {};

constexpr std::pair<uint64_t, unsigned> TestedFlagBits[] = {
    {x86reg::CF, 0}, {x86reg::PF, 2},  {x86reg::AF, 4}, {x86reg::ZF, 6},
    {x86reg::SF, 7}, {x86reg::DF, 10}, {x86reg::OF, 11}};

void setArithmeticAndDirectionFlags(Machine &M, uint64_t Bits) {
  for (auto [Offset, Bit] : TestedFlagBits)
    M.State.write(SymSpace::Register, Offset,
                  M.Ctx.mkConst(8, (Bits >> Bit) & 1));
}

TEST_P(X86FlagsStackSemantics, PushWritesOnlyOperandWidth) {
  const auto P = GetParam();
  const uint64_t Sp = P.Target == Arch::X64 ? UINT64_C(0x100007800) : InitialSp;
  for (uint64_t Flags : {UINT64_C(0x2), UINT64_C(0xcd7)}) {
    SCOPED_TRACE(Flags);
    Machine M;
    M.reg(x86reg::RSP, Sp);
    setArithmeticAndDirectionFlags(M, Flags);
    M.memory(Sp - 16, UINT64_C(0xaaaaaaaaaaaaaaaa));
    M.memory(Sp - 8, UINT64_C(0xaaaaaaaaaaaaaaaa));
    M.memory(Sp, UINT64_C(0xaaaaaaaaaaaaaaaa));
    if (P.Word)
      M.run(P.Target, {0x66, 0x9c});
    else
      M.run(P.Target, {0x9c});
    EXPECT_EQ(M.getReg(x86reg::RSP, P.Target == Arch::X64 ? 8 : 4),
              Sp - P.Width);
    EXPECT_EQ(M.getMemory(Sp - P.Width, P.Width), Flags);
    for (uint64_t Address = Sp - 16; Address < Sp + 8; ++Address)
      if (Address < Sp - P.Width || Address >= Sp)
        EXPECT_EQ(M.getMemory(Address, 1), 0xaau) << Address;
  }
}

TEST_P(X86FlagsStackSemantics, PopAdvancesByOperandWidth) {
  const auto P = GetParam();
  const uint64_t Sp = P.Target == Arch::X64 ? UINT64_C(0x100007800) : InitialSp;
  for (uint64_t Flags : {UINT64_C(0x2), UINT64_C(0xcd7)}) {
    SCOPED_TRACE(Flags);
    Machine M;
    M.reg(x86reg::RSP, Sp);
    setArithmeticAndDirectionFlags(M, ~Flags);
    M.memory(Sp, UINT64_C(0xaaaaaaaaaaaaaaaa));
    M.memory(Sp, Flags, P.Width);
    M.memory(Sp + 8, UINT64_C(0xbbbbbbbbbbbbbbbb));
    if (P.Word)
      M.run(P.Target, {0x66, 0x9d});
    else
      M.run(P.Target, {0x9d});
    EXPECT_EQ(M.getReg(x86reg::RSP, P.Target == Arch::X64 ? 8 : 4),
              Sp + P.Width);
    for (auto [Offset, Bit] : TestedFlagBits)
      EXPECT_EQ(M.getReg(Offset, 1), (Flags >> Bit) & 1) << Offset;
    EXPECT_EQ(M.getMemory(Sp, P.Width), Flags);
    EXPECT_EQ(M.getMemory(Sp + 8), UINT64_C(0xbbbbbbbbbbbbbbbb));
  }
}

INSTANTIATE_TEST_SUITE_P(OperandWidths, X86FlagsStackSemantics,
                         testing::Values(FlagsStackCase{Arch::X64, true, 2},
                                         FlagsStackCase{Arch::X64, false, 8},
                                         FlagsStackCase{Arch::X86, true, 2},
                                         FlagsStackCase{Arch::X86, false, 4}));
} // namespace
