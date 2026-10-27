//===- SymCallEffectTests.cpp - Explicit call register results ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Check complete call effects with several scalar outputs, partial register
/// writes, pre-call inputs and invalid register ranges.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymExec.h"

#include <limits>
#include <stdexcept>

using namespace neverd;
using namespace neverd::symbolic;

namespace {
LowOp call(NdOp Opcode = NdOp::CALL, NdVar Output = {}) {
  LowOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  Op.addInput(NdVar::reg(0, 8));
  return Op;
}
} // namespace

TEST(SymCallEffect,
     SuppliesMultipleOutputsWithoutPreservingOtherVolatileBytes) {
  for (auto Opcode : {NdOp::CALL, NdOp::INDIR_CALL}) {
    SymContext Ctx;
    SymState State(Ctx);
    SymExec Exec(Ctx, State);
    State.write(SymSpace::Register, 0, Ctx.mkConst(64, 0x2000));
    State.write(SymSpace::Register, 8, Ctx.mkConst(64, 0xabcdef));
    State.write(SymSpace::Register, 16, Ctx.mkConst(64, 0x1234));
    State.write(SymSpace::Register, 24, Ctx.mkConst(64, 0x5678));
    Exec.setCallPreservedRegisters({{24, 8}});
    const auto Address = Ctx.mkConst(64, 0x3000);
    State.store(Address, Ctx.mkConst(64, 0xfedcba));

    const auto Status = Ctx.mkFreshVar(64, "status");
    const auto Flags = Ctx.mkOr(Ctx.mkAnd(Status, Ctx.mkConst(64, 0xff)),
                                Ctx.mkConst(64, 0x100));
    SymCallEffect Effect;
    Effect.Memory = SymCallMemoryEffect::Preserve;
    Effect.Registers = {{8, Ctx.mkConst(64, 19)}, {32, Flags}};
    EXPECT_EQ(Exec.step(call(Opcode), &Effect), StepResult::Continue);
    EXPECT_EQ(Exec.branchTarget(), Ctx.mkConst(64, 0x2000));
    EXPECT_EQ(State.read(SymSpace::Register, 8, 8), Ctx.mkConst(64, 19));
    EXPECT_EQ(State.read(SymSpace::Register, 32, 8), Flags);
    EXPECT_EQ(State.read(SymSpace::Register, 24, 8), Ctx.mkConst(64, 0x5678));
    EXPECT_EQ(State.load(Address, 8), Ctx.mkConst(64, 0xfedcba));
    EXPECT_FALSE(Ctx.isConst(State.read(SymSpace::Register, 0, 8)));
    EXPECT_FALSE(Ctx.isConst(State.read(SymSpace::Register, 16, 8)));
    EXPECT_EQ(Exec.callHavocCount(), 0u);
    EXPECT_EQ(Exec.unmodelledCount(), 0u);
    for (unsigned Value = 0; Value != 256; ++Value)
      EXPECT_EQ(Ctx.eval(State.read(SymSpace::Register, 32, 8),
                         {llvm::APInt(64, Value)})
                    .getZExtValue(),
                Value | 0x100u);
  }
}

TEST(SymCallEffect, RegisterAndMemoryResultsUseCapturedPreCallInputs) {
  SymContext Ctx;
  SymState State(Ctx);
  SymExec Exec(Ctx, State);
  const auto Pointer = Ctx.mkConst(64, 0x4000);
  const auto Input = Ctx.mkVar("input", 64);
  State.write(SymSpace::Register, 0, Pointer);
  State.write(SymSpace::Register, 8, Input);
  SymCallEffect Effect;
  Effect.Writes = {{State.read(SymSpace::Register, 0, 8), Input}};
  Effect.Registers = {{0, State.read(SymSpace::Register, 8, 8)},
                      {8, State.read(SymSpace::Register, 0, 8)}};
  Effect.ReturnValue = Ctx.mkConst(16, 0xbeef);
  EXPECT_EQ(Exec.step(call(NdOp::CALL, NdVar::reg(0, 2)), &Effect),
            StepResult::Continue);
  EXPECT_EQ(Exec.branchTarget(), Pointer);
  EXPECT_EQ(State.load(Pointer, 8), Input);
  EXPECT_EQ(State.read(SymSpace::Register, 8, 8), Pointer);
  const auto Result = State.read(SymSpace::Register, 0, 8);
  for (uint64_t Value : {uint64_t(0), uint64_t(0x123456789abcdef0),
                         std::numeric_limits<uint64_t>::max()})
    EXPECT_EQ(Ctx.eval(Result, {llvm::APInt(64, Value)}).getZExtValue(),
              (Value & ~uint64_t(0xffff)) | 0xbeef);
}

TEST(SymCallEffect, OrderedPartialWritesRespectBothByteOrders) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymContext Ctx;
    SymState State(Ctx, Order);
    SymExec Exec(Ctx, State);
    State.write(SymSpace::Register, 8, Ctx.mkConst(64, 0x0123456789abcdef));
    Exec.setCallPreservedRegisters({{8, 8}});
    SymCallEffect Effect;
    Effect.Registers = {{10, Ctx.mkConst(16, 0xa1b2)},
                        {11, Ctx.mkConst(8, 0xc3)}};
    EXPECT_EQ(Exec.step(call(), &Effect), StepResult::Continue);
    const uint64_t Expected = Order == llvm::endianness::little
                                  ? 0x01234567c3b2cdef
                                  : 0x0123a1c389abcdef;
    EXPECT_EQ(State.read(SymSpace::Register, 8, 8), Ctx.mkConst(64, Expected));
    EXPECT_FALSE(Ctx.isConst(State.read(SymSpace::Register, 16, 8)));
  }
}

TEST(SymCallEffect, RegisterResultsDoNotPreserveUnknownMemory) {
  SymContext Ctx;
  SymState State(Ctx);
  SymExec Exec(Ctx, State);
  const auto Address = Ctx.mkConst(64, 0x5000);
  State.store(Address, Ctx.mkConst(32, 0x11223344));
  SymCallEffect Effect;
  Effect.Registers = {{8, Ctx.mkConst(64, 7)}};
  EXPECT_EQ(Exec.step(call(), &Effect), StepResult::Continue);
  EXPECT_EQ(State.read(SymSpace::Register, 8, 8), Ctx.mkConst(64, 7));
  EXPECT_FALSE(Ctx.isConst(State.load(Address, 4)));
  EXPECT_TRUE(State.memoryIsUnknown());
}

TEST(SymCallEffect, InvalidRegisterRangesRejectBeforeAnyStateMutation) {
  SymContext Ctx;
  const SymCallRegisterWrite Invalid[] = {
      {8, {}},
      {8, Ctx.mkConst(7, 0)},
      {std::numeric_limits<uint64_t>::max(), Ctx.mkConst(16, 0)},
      {8, Ctx.mkZero(65536 * 8)}};
  for (auto Bad : Invalid) {
    SymState State(Ctx);
    SymExec Exec(Ctx, State);
    const auto Value = Ctx.mkConst(64, 0x12345678);
    const auto Address = Ctx.mkConst(64, 0x6000);
    State.write(SymSpace::Register, 8, Value);
    State.store(Address, Value);
    ASSERT_TRUE(
        State.setX64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS, Value));
    const auto Live = State.numLiveBytes();
    SymCallEffect Effect;
    Effect.Registers = {{8, Ctx.mkZero(64)}, Bad};
    // Capture after constructing the proposed contract, before admission.
    const auto BeforeNodes = Ctx.numNodes();
    EXPECT_THROW(Exec.step(call(NdOp::INDIR_CALL), &Effect),
                 std::invalid_argument);
    EXPECT_EQ(State.numLiveBytes(), Live);
    EXPECT_EQ(Ctx.numNodes(), BeforeNodes);
    EXPECT_EQ(State.read(SymSpace::Register, 8, 8), Value);
    EXPECT_EQ(State.load(Address, 8), Value);
    EXPECT_EQ(State.x64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS),
              Value);
    EXPECT_FALSE(Exec.branchTarget().isValid());
    EXPECT_EQ(Exec.unmodelledCount(), 0u);
  }
}

TEST(SymCallEffect, NonCallOperationsIgnoreTheCallContract) {
  SymContext Ctx;
  SymState State(Ctx);
  SymExec Exec(Ctx, State);
  SymCallEffect Effect;
  Effect.Registers = {{0, {}}};
  LowOp Op;
  Op.Opcode = NdOp::COPY;
  Op.Output = NdVar::reg(0, 8);
  Op.addInput(NdVar::cst(9, 8));
  EXPECT_EQ(Exec.step(Op, &Effect), StepResult::Continue);
  EXPECT_EQ(State.read(SymSpace::Register, 0, 8), Ctx.mkConst(64, 9));
}
