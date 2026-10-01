//===- StringTransferTests.cpp - Ordered repeated memory semantics --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/analysis/interpreter/StringTransfer.h"
#include "gtest/gtest.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/symbolic/SymExec.h"

#include <array>
#include <limits>

using namespace neverd;
using namespace neverd::analysis::detail;
using namespace neverd::symbolic;

namespace {

LowOp transfer(Intrinsic Id, unsigned Width, bool Fill) {
  LowOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.Output = NdVar::tmp(1024, 8);
  Op.addInput(NdVar::scalar(static_cast<uint16_t>(Id), 2));
  Op.addInput(NdVar::reg(0, 8));
  Op.addInput(NdVar::reg(8, 8));
  Op.addInput(NdVar::reg(16, Fill ? Width : 8));
  Op.addInput(NdVar::reg(24, 1));
  return Op;
}

TEST(StringTransfer, ElementOrderMatchesIndependentOverlappingCopyAndFill) {
  const Intrinsic Moves[] = {Intrinsic::Movsb, Intrinsic::Movsw,
                             Intrinsic::Movsd, Intrinsic::Movsq};
  const Intrinsic Fills[] = {Intrinsic::Stosb, Intrinsic::Stosw,
                             Intrinsic::Stosd, Intrinsic::Stosq};
  constexpr uint64_t Base = 0x10000;
  constexpr uint64_t FillValue = 0x9371ab05d2e648fc;
  for (unsigned SizeIndex = 0; SizeIndex < 4; ++SizeIndex) {
    const unsigned Width = 1u << SizeIndex;
    for (bool Fill : {false, true})
      for (bool Backward : {false, true})
        for (unsigned Count : {0, 1, 3, 7})
          for (int Gap : {-32, -8, -1, 0, 1, 8, 32}) {
            SCOPED_TRACE(::testing::Message()
                         << Width << ":" << Fill << ":" << Backward << ":"
                         << Count << ":" << Gap);
            std::array<uint8_t, 256> Expected;
            SymContext Ctx;
            SymState State(Ctx);
            for (unsigned I = 0; I < Expected.size(); ++I) {
              Expected[I] = static_cast<uint8_t>(I * 29 + I / 7);
              State.store(Ctx.mkConst(64, Base + I),
                          Ctx.mkConst(8, Expected[I]));
            }
            const int Source = Backward ? 160 : 64;
            const int Destination = Source + Gap;
            for (unsigned I = 0; I < Count; ++I) {
              const int Delta = (Backward ? -1 : 1) * int(I * Width);
              std::array<uint8_t, 8> Element;
              for (unsigned B = 0; B < Width; ++B)
                Element[B] = Fill ? static_cast<uint8_t>(FillValue >> (8 * B))
                                  : Expected[Source + Delta + B];
              for (unsigned B = 0; B < Width; ++B)
                Expected[Destination + Delta + B] = Element[B];
            }
            const LowOp Op = transfer(
                Fill ? Fills[SizeIndex] : Moves[SizeIndex], Width, Fill);
            State.write(SymSpace::Register, 0,
                        Ctx.mkConst(64, Base + (Fill ? Destination : Source)));
            State.write(SymSpace::Register, 8,
                        Ctx.mkConst(64, Fill ? Count : Base + Destination));
            State.write(
                SymSpace::Register, 16,
                Ctx.mkConst(Fill ? Width * 8 : 64, Fill ? FillValue : Count));
            State.write(SymSpace::Register, 24, Ctx.mkConst(8, Backward));
            auto Lowered =
                lowerStringTransfer(Op, Count, Backward, {Op}, 4096, 1024);
            ASSERT_TRUE(static_cast<bool>(Lowered))
                << llvm::toString(Lowered.takeError());
            SymExec Exec(Ctx, State);
            for (const auto &Scalar : *Lowered)
              ASSERT_EQ(Exec.step(Scalar), StepResult::Continue);
            for (unsigned I = 0; I < Expected.size(); ++I) {
              const auto Actual =
                  Ctx.asConst(State.load(Ctx.mkConst(64, Base + I), 1));
              ASSERT_TRUE(Actual.has_value());
              EXPECT_EQ(Actual->getZExtValue(), Expected[I]);
            }
            const auto Output = Ctx.asConst(
                State.read(SymSpace::Temporary, Op.Output.Offset, 8));
            ASSERT_TRUE(Output.has_value());
            EXPECT_TRUE(Output->isZero());
            EXPECT_EQ(Exec.opaqueOperationCount(), 0u);
            if (!Count)
              for (const auto &Scalar : *Lowered) {
                EXPECT_NE(Scalar.Opcode, NdOp::LOAD);
                EXPECT_NE(Scalar.Opcode, NdOp::STORE);
              }
          }
  }
}

TEST(StringTransfer, ExactBudgetScratchAndMalformedShapes) {
  const auto Op = transfer(Intrinsic::Movsq, 8, false);
  auto Good = lowerStringTransfer(Op, 7, true, {Op}, 4096, 29);
  ASSERT_TRUE(static_cast<bool>(Good)) << llvm::toString(Good.takeError());
  const auto Refused = [&](const LowOp &Candidate, uint64_t Count,
                           uint64_t Limit, uint64_t Budget) {
    auto Result = lowerStringTransfer(Candidate, Count, false, {Candidate},
                                      Limit, Budget);
    EXPECT_FALSE(static_cast<bool>(Result));
    if (!Result)
      llvm::consumeError(Result.takeError());
  };
  Refused(Op, 7, 4096, 28);
  Refused(Op, std::numeric_limits<uint64_t>::max(), 4096,
          std::numeric_limits<uint64_t>::max());
  Refused(Op, std::numeric_limits<uint64_t>::max() / 4, 4096,
          std::numeric_limits<uint64_t>::max());
  Refused(Op, 1, 1055, 5);
  auto Zero = lowerStringTransfer(Op, 0, true, {Op}, 1032, 1);
  ASSERT_TRUE(static_cast<bool>(Zero)) << llvm::toString(Zero.takeError());
  for (unsigned Kind = 0; Kind < 6; ++Kind) {
    LowOp Bad = Op;
    switch (Kind) {
    case 0:
      Bad.NumInputs = 4;
      break;
    case 1:
      Bad.Inputs[3].Size = 4;
      break;
    case 2:
      Bad.Inputs[4].Size = 8;
      break;
    case 3:
      Bad.Output = NdVar::reg(32, 8);
      break;
    case 4:
      Bad.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      break;
    case 5:
      Bad.Inputs[0] = NdVar::scalar(static_cast<uint16_t>(Intrinsic::Lodsb), 2);
      break;
    }
    Refused(Bad, 0, 4096, 1);
  }
}

TEST(StringTransfer, ScratchCannotClobberAnotherInstructionOperand) {
  auto Op = transfer(Intrinsic::Movsb, 1, false);
  Op.Inputs[1] = NdVar::tmp(2048, 8);
  LowOp Later;
  Later.Opcode = NdOp::COPY;
  Later.Output = NdVar::tmp(2080, 8);
  Later.addInput(Op.Output);
  auto Lowered = lowerStringTransfer(Op, 1, false, {Op, Later}, 4096, 5);
  ASSERT_TRUE(static_cast<bool>(Lowered))
      << llvm::toString(Lowered.takeError());
  for (const auto &Scalar : *Lowered)
    if (Scalar.Output.Size && Scalar.Output != Op.Output)
      EXPECT_GE(Scalar.Output.Offset, 2088u);
}

} // namespace
