//===- X86_64_XaddAuditTests.cpp - Register XADD arithmetic audit --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/symbolic/SymExec.h"

#include <array>
#include <bit>
#include <cstdint>
#include <vector>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

constexpr std::array<unsigned, 7> Flags = {x86reg::CF, x86reg::PF, x86reg::AF,
                                           x86reg::ZF, x86reg::SF, x86reg::DF,
                                           x86reg::OF};

void checkXadd(Arch Target, unsigned Width, unsigned Destination,
               unsigned Source, unsigned ExtraRex = 0,
               bool ExhaustBytes = false) {
  SCOPED_TRACE(::testing::Message()
               << unsigned(Target) << '/' << Width << '/' << Destination << '/'
               << Source << '/' << ExtraRex);
  const unsigned Bits = Width * 8;
  const uint64_t Mask = Width == 8 ? UINT64_MAX : (UINT64_C(1) << Bits) - 1;
  const uint64_t Sign = UINT64_C(1) << (Bits - 1);
  const uint8_t Rex = Width == 8 || Destination >= 8 || Source >= 8 || ExtraRex
                          ? 0x40 | (Width == 8 ? 8 : 0) |
                                (Destination >= 8 ? 1 : 0) |
                                (Source >= 8 ? 4 : 0) | ExtraRex
                          : 0;
  std::vector<uint8_t> Bytes;
  if (Width == 2)
    Bytes.push_back(0x66);
  if (Rex)
    Bytes.push_back(Rex);
  Bytes.insert(Bytes.end(),
               {0x0f, uint8_t(Width == 1 ? 0xc0 : 0xc1),
                uint8_t(0xc0 | ((Source % 8) << 3) | Destination % 8)});
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Target));
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn),
            int(Bytes.size()));
  std::vector<LowOp> Ops, Plain;
  LowInstructionUndefinedEffects Effects;
  Dec.liftToLow(Insn, Ops, {}, {}, &Effects);
  Dec.liftToLow(Insn, Plain);
  ASSERT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
      << Effects.Diagnostic;
  EXPECT_TRUE(Effects.Effects.empty());
  EXPECT_EQ(Effects.OpCount, Ops.size());
  EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Ops));
  EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Plain));
  const auto Offset = [&](unsigned Number) {
    return Width == 1 && !Rex && Number >= 4 ? (Number - 4) * 8 + 1
                                             : Number * 8;
  };
  const unsigned D = Offset(Destination), S = Offset(Source);
  std::vector<uint64_t> Values = {
      0,        1,        15,
      16,       Sign - 1, Sign,
      Mask - 1, Mask,     UINT64_C(0xa5c319087fed204b) & Mask};
  if (ExhaustBytes) {
    Values.clear();
    for (unsigned I = 0; I != 256; ++I)
      Values.push_back(I);
  }
  for (uint64_t DV : Values)
    for (uint64_t SV : Values)
      for (unsigned Old : {0u, 127u}) {
        if (ExhaustBytes && Old != 0)
          continue;
        const unsigned OldFlags = ExhaustBytes ? (DV + SV) % 128 : Old;
        SCOPED_TRACE(::testing::Message()
                     << DV << '/' << SV << '/' << OldFlags);
        std::array<uint64_t, 16> Registers;
        for (unsigned I = 0; I != Registers.size(); ++I) {
          Registers[I] = UINT64_C(0xfedcba9876543210) + I;
          if (Target == Arch::X86)
            Registers[I] &= UINT32_MAX;
        }
        const auto Write = [&](unsigned O, uint64_t V, bool Commit) {
          const unsigned Shift = (O % 8) * 8;
          const uint64_t Field = Mask << Shift;
          auto &R = Registers[O / 8];
          R = (R & ~Field) | ((V & Mask) << Shift);
          if (Commit && Width == 4 && Target == Arch::X64)
            R &= UINT32_MAX;
        };
        Write(D, DV, false);
        Write(S, SV, false);
        // Read both original register slices after setting overlapping inputs.
        const uint64_t A = (Registers[D / 8] >> ((D % 8) * 8)) & Mask;
        const uint64_t B = (Registers[S / 8] >> ((S % 8) * 8)) & Mask;
        SymContext Ctx;
        SymState State(Ctx);
        SymExec Exec(Ctx, State);
        for (unsigned I = 0; I != Registers.size(); ++I)
          State.write(SymSpace::Register, I * 8, Ctx.mkConst(64, Registers[I]));
        for (unsigned I = 0; I != Flags.size(); ++I)
          State.write(SymSpace::Register, Flags[I],
                      Ctx.mkConst(8, (OldFlags >> I) & 1));
        const uint64_t Sum = (A + B) & Mask;
        // Range comparisons avoid the lifter's XOR-based flag formulas and
        // avoid host signed overflow, including the 64-bit carry boundary.
        const bool Overflow =
            (A < Sign) == (B < Sign) && (Sum < Sign) != (A < Sign);
        const std::array<unsigned, 7> ExpectedFlags = {
            unsigned(B > Mask - A),
            unsigned((std::popcount(uint8_t(Sum)) & 1) == 0),
            unsigned((A % 16) + (B % 16) >= 16),
            unsigned(Sum == 0),
            unsigned(Sum >= Sign),
            (OldFlags >> 5) & 1,
            unsigned(Overflow)};
        Write(S, A, true);
        Write(D, Sum, true);
        ASSERT_EQ(Exec.run(Ops), Ops.size());
        ASSERT_EQ(Exec.unmodelledCount(), 0u);
        for (unsigned I = 0; I != Registers.size(); ++I) {
          const auto Actual =
              Ctx.asConst(State.read(SymSpace::Register, I * 8, 8));
          ASSERT_TRUE(Actual);
          EXPECT_EQ(Actual->getZExtValue(), Registers[I]) << "register " << I;
        }
        for (unsigned I = 0; I != Flags.size(); ++I) {
          const auto Actual =
              Ctx.asConst(State.read(SymSpace::Register, Flags[I], 1));
          ASSERT_TRUE(Actual);
          EXPECT_EQ(Actual->getZExtValue(), ExpectedFlags[I])
              << "flag " << Flags[I];
        }
      }
}

TEST(X86XaddAudit, ArithmeticFlagsAndBothRegisterWritesAcrossAliases) {
  for (Arch Target : {Arch::X86, Arch::X64})
    for (unsigned Width : {1u, 2u, 4u, 8u}) {
      if (Target == Arch::X86 && Width == 8)
        continue;
      for (const auto [D, S] : {std::pair{0u, 1u},
                                {0u, 0u},
                                {0u, 4u},
                                {4u, 0u},
                                {4u, 4u},
                                {5u, 7u},
                                {7u, 5u},
                                {9u, 10u},
                                {8u, 8u},
                                {0u, 9u},
                                {9u, 0u},
                                {15u, 15u}}) {
        if (Target == Arch::X86 && (D >= 8 || S >= 8))
          continue;
        for (unsigned Rex : {0u, 0x40u, 0x48u}) {
          if (Rex && (Target == Arch::X86 || Width != 1))
            continue;
          checkXadd(Target, Width, D, S, Rex);
          if (testing::Test::HasFatalFailure())
            return;
        }
      }
    }
}

TEST(X86XaddAudit, EveryByteOperandPairMatchesArithmeticOracle) {
  checkXadd(Arch::X64, 1, 0, 1, 0, true);
}

} // namespace
