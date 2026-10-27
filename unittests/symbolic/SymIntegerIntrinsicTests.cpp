//===- SymIntegerIntrinsicTests.cpp - Shuffle semantics ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymExec.h"
#include "neverd/symbolic/SymIntegerIntrinsics.h"

using namespace neverd;
using namespace neverd::symbolic;

namespace {

LowOp shuffle(unsigned Bytes, uint8_t Control) {
  LowOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.Output = NdVar::reg(0, Bytes);
  Op.addInput(NdVar::cst(static_cast<uint64_t>(Intrinsic::Shufps), 8));
  Op.addInput(NdVar::reg(0, Bytes));
  Op.addInput(NdVar::reg(128, Bytes));
  Op.addInput(NdVar::cst(Control, 1));
  return Op;
}

TEST(SymIntegerIntrinsics, EveryShuffleControlPreservesLaneBitsAndAliasing) {
  for (unsigned Bytes : {16u, 32u, 64u})
    for (bool Aliased : {false, true}) {
      SymContext Ctx;
      auto Left = Ctx.mkVar("shuffle_left", Bytes * 8);
      auto Right = Ctx.mkVar("shuffle_right", Bytes * 8);
      llvm::APInt A(Bytes * 8, 0), B(Bytes * 8, 0);
      for (unsigned I = 0; I < Bytes / 4; ++I) {
        A.insertBits(llvm::APInt(32, 0x7f800001u + I * 0x01010101u), I * 32);
        B.insertBits(llvm::APInt(32, 0xff800123u - I * 0x01010101u), I * 32);
      }
      for (unsigned Control = 0; Control < 256; ++Control) {
        SCOPED_TRACE(Bytes);
        SCOPED_TRACE(Aliased);
        SCOPED_TRACE(Control);
        auto Op = shuffle(Bytes, uint8_t(Control));
        if (Aliased)
          Op.Inputs[2] = Op.Inputs[1];
        auto Result = evaluateIntegerIntrinsic(Ctx, Op, [&](const NdVar &V) {
          return V.Offset == 0 ? Left : Right;
        });
        ASSERT_TRUE(Result);
        llvm::APInt Expected(Bytes * 8, 0);
        for (unsigned Lane = 0; Lane < Bytes / 16; ++Lane)
          for (unsigned Word = 0; Word < 4; ++Word) {
            const auto &Source = Word < 2 || Aliased ? A : B;
            const unsigned Selected = (Control >> (2 * Word)) & 3;
            Expected.insertBits(
                Source.extractBits(32, Lane * 128 + Selected * 32),
                Lane * 128 + Word * 32);
          }
        EXPECT_EQ(Ctx.eval(*Result, {A, B}), Expected);
      }
    }
}

TEST(SymIntegerIntrinsics, InvalidShapesDoNotReadInputs) {
  SymContext Ctx;
  for (unsigned Variant = 0; Variant < 14; ++Variant) {
    SCOPED_TRACE(Variant);
    auto Op = shuffle(16, 0xdd);
    switch (Variant) {
    case 0:
      Op.Opcode = NdOp::COPY;
      break;
    case 1:
      --Op.NumInputs;
      break;
    case 2:
      Op.Inputs[0] = NdVar::reg(0, 8);
      break;
    case 3:
      Op.Inputs[0] = NdVar::cst(static_cast<uint64_t>(Intrinsic::Shufpd), 8);
      break;
    case 4:
      Op.Output.Size = 8;
      break;
    case 5:
      Op.Inputs[1].Size = 8;
      break;
    case 6:
      Op.Inputs[2].Size = 8;
      break;
    case 7:
      Op.Inputs[3] = NdVar::reg(0, 1);
      break;
    case 8:
      Op.Inputs[3] = NdVar::cst(0x100, 2);
      break;
    case 9:
      Op.Inputs[3] = NdVar::cst(0x100, 1);
      break;
    case 10:
      Op.Inputs[3] = NdVar::cst(0xdd, 2);
      break;
    case 11:
      Op.Output = NdVar::ram(0x1000, 16);
      break;
    case 12:
      Op.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
      break;
    case 13:
      Op.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
      break;
    }
    unsigned Reads = 0;
    EXPECT_FALSE(evaluateIntegerIntrinsic(Ctx, Op, [&](const NdVar &) {
      ++Reads;
      return Ctx.mkZero(128);
    }));
    EXPECT_EQ(Reads, 0u);
  }
}

TEST(SymIntegerIntrinsics,
     ExecutorUsesTheSharedModelBeforeClobberingAliasedInput) {
  for (unsigned Bytes : {16u, 32u, 64u})
    for (unsigned Destination = 0; Destination < 3; ++Destination) {
      SCOPED_TRACE(Bytes);
      SCOPED_TRACE(Destination);
      SymContext Ctx;
      SymState State(Ctx);
      auto A = Ctx.mkVar("executor_shuffle_left", Bytes * 8);
      auto B = Ctx.mkVar("executor_shuffle_right", Bytes * 8);
      State.write(SymSpace::Register, 0, A);
      State.write(SymSpace::Register, 128, B);
      auto Op = shuffle(Bytes, 0xdd);
      Op.Output = Destination == 2 ? NdVar::tmp(0, Bytes)
                                   : NdVar::reg(Destination * 128, Bytes);
      SymExec Exec(Ctx, State);
      EXPECT_EQ(Exec.step(Op), StepResult::Continue);
      EXPECT_EQ(Exec.unmodelledCount(), 0u);
      auto Result = State.read(Destination == 2 ? SymSpace::Temporary
                                                : SymSpace::Register,
                               Op.Output.Offset, Bytes);
      // 0xdd selects A1,A3,B1,B3 independently in each 128-bit lane.
      for (unsigned Lane = 0; Lane < Bytes / 16; ++Lane) {
        const unsigned Base = Lane * 128;
        EXPECT_EQ(Ctx.mkExtract(Result, Base, 128),
                  Ctx.mkConcat({Ctx.mkExtract(B, Base + 96, 32),
                                Ctx.mkExtract(B, Base + 32, 32),
                                Ctx.mkExtract(A, Base + 96, 32),
                                Ctx.mkExtract(A, Base + 32, 32)}));
      }
    }
}

TEST(SymIntegerIntrinsics, OtherIntrinsicsRemainUnmodelled) {
  SymContext Ctx;
  SymState State(Ctx);
  auto Op = shuffle(16, 0xdd);
  Op.Inputs[0] = NdVar::cst(static_cast<uint64_t>(Intrinsic::Shufpd), 8);
  SymExec Exec(Ctx, State);
  EXPECT_EQ(Exec.step(Op), StepResult::Unmodelled);
  EXPECT_EQ(Exec.unmodelledCount(), 1u);
}

TEST(SymIntegerIntrinsics, MissingOrWrongWidthValuesRemainUnsupported) {
  SymContext Ctx;
  auto Op = shuffle(16, 0xdd);
  EXPECT_FALSE(evaluateIntegerIntrinsic(
      Ctx, Op, [&](const NdVar &) { return SymRef{}; }));
  EXPECT_FALSE(evaluateIntegerIntrinsic(
      Ctx, Op, [&](const NdVar &) { return Ctx.mkZero(64); }));
}

} // namespace
