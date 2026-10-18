//===- X86_64_LegacyVectorMoveAuditTests.cpp - Vector move audit ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Complete vector values and independent admission boundaries for legacy
/// moves.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/symbolic/SymExec.h"

#include <array>
#include <cstdint>
#include <vector>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

constexpr std::array<unsigned, 7> Flags = {x86reg::CF, x86reg::PF, x86reg::AF,
                                           x86reg::ZF, x86reg::SF, x86reg::DF,
                                           x86reg::OF};

struct MemoryCase {
  Arch Target;
  std::vector<uint8_t> ModRM;
  int Base, Index;
  unsigned Scale;
  int64_t Displacement;
  bool AddressPrefix = false;
  uint8_t Rex = 0;
};

void checkMove(Arch Target, const std::vector<uint8_t> &Bytes,
               unsigned Destination, unsigned Source, const MemoryCase *Memory,
               bool Store) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Target));
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn),
            int(Bytes.size()));
  SCOPED_TRACE(::testing::Message()
               << Insn.Raw->mnemonic << ' ' << Insn.Raw->op_str);
  std::vector<LowOp> Ops, Plain;
  LowInstructionUndefinedEffects Effects;
  LowInstructionPreservedState Preserved;
  Preserved.Audit = LowPreservedStateAudit::LegacyIntegerV1;
  Dec.liftToLow(Insn, Ops, {}, {}, &Effects, &Preserved);
  Dec.liftToLow(Insn, Plain);
  ASSERT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
      << Effects.Diagnostic;
  EXPECT_TRUE(Effects.Effects.empty());
  EXPECT_EQ(Effects.OpCount, Ops.size());
  EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Ops));
  EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Plain));
  EXPECT_EQ(Preserved, LowInstructionPreservedState{});

  for (unsigned Seed : {0u, 1u, 2u, 3u}) {
    SymContext Ctx;
    SymState State(Ctx);
    SymExec Exec(Ctx, State);
    std::array<uint64_t, 16> Registers;
    for (unsigned I = 0; I != Registers.size(); ++I) {
      Registers[I] = UINT64_C(0x100004003) + I * 0x40;
      State.write(SymSpace::Register, I * 8, Ctx.mkConst(64, Registers[I]));
    }
    std::vector<llvm::APInt> Vectors;
    for (unsigned I = 0; I != x86reg::VectorRegCount; ++I) {
      std::array<uint64_t, 8> Words;
      for (unsigned J = 0; J != Words.size(); ++J) {
        const uint64_t Pattern =
            UINT64_C(0xd7a319c05b8e42f1) * (1 + I + J * 37);
        Words[J] = Seed == 2   ? 0
                   : Seed == 3 ? UINT64_MAX
                   : Seed == 1 ? ~Pattern
                               : Pattern;
      }
      Vectors.emplace_back(512, llvm::ArrayRef<uint64_t>(Words));
      State.write(SymSpace::Register, x86reg::vectorReg(I),
                  Ctx.mkConst(Vectors.back()));
    }
    for (unsigned I = 0; I != Flags.size(); ++I)
      State.write(SymSpace::Register, Flags[I], Ctx.mkConst(8, (Seed + I) % 2));

    uint64_t Address = 0;
    std::array<uint8_t, 18> MemoryBytes;
    for (unsigned I = 0; I != MemoryBytes.size(); ++I)
      MemoryBytes[I] = uint8_t(0x87 + I * 29 + Seed * 71);
    if (Memory) {
      Address = Memory->Displacement;
      if (Memory->Base == -2)
        Address += 0x1000 + Bytes.size();
      else if (Memory->Base >= 0)
        Address += Registers[Memory->Base];
      if (Memory->Index >= 0)
        Address += Registers[Memory->Index] * Memory->Scale;
      const unsigned AddressSize = Target == Arch::X64
                                       ? (Memory->AddressPrefix ? 4 : 8)
                                       : (Memory->AddressPrefix ? 2 : 4);
      if (AddressSize < 8)
        Address &= (UINT64_C(1) << (AddressSize * 8)) - 1;
      ASSERT_GT(Address, 0U);
      ASSERT_LT(Address, UINT64_MAX - MemoryBytes.size());
      for (unsigned I = 0; I != MemoryBytes.size(); ++I)
        State.store(Ctx.mkConst(64, Address - 1 + I),
                    Ctx.mkConst(8, MemoryBytes[I]));
    }
    if (Memory && Store) {
      for (unsigned I = 0; I != 16; ++I)
        MemoryBytes[I + 1] = Vectors[Source].extractBitsAsZExtValue(8, I * 8);
    } else {
      llvm::APInt Value(128, 0);
      if (Memory) {
        for (unsigned I = 0; I != 16; ++I)
          Value.insertBits(llvm::APInt(8, MemoryBytes[I + 1]), I * 8);
      } else {
        Value = Vectors[Source].trunc(128);
      }
      Vectors[Destination].insertBits(Value, 0);
    }
    ASSERT_EQ(Exec.run(Ops), Ops.size());
    ASSERT_EQ(Exec.unmodelledCount(), 0U);
    for (unsigned I = 0; I != Vectors.size(); ++I) {
      auto Actual =
          Ctx.asConst(State.read(SymSpace::Register, x86reg::vectorReg(I), 64));
      ASSERT_TRUE(Actual);
      EXPECT_EQ(*Actual, Vectors[I]) << "complete vector " << I;
    }
    for (unsigned I = 0; I != Registers.size(); ++I) {
      auto Actual = Ctx.asConst(State.read(SymSpace::Register, I * 8, 8));
      ASSERT_TRUE(Actual);
      EXPECT_EQ(Actual->getZExtValue(), Registers[I]);
    }
    for (unsigned I = 0; I != Flags.size(); ++I) {
      auto Actual = Ctx.asConst(State.read(SymSpace::Register, Flags[I], 1));
      ASSERT_TRUE(Actual);
      EXPECT_EQ(Actual->getZExtValue(), (Seed + I) % 2);
    }
    if (Memory)
      for (unsigned I = 0; I != MemoryBytes.size(); ++I) {
        auto Actual =
            Ctx.asConst(State.load(Ctx.mkConst(64, Address - 1 + I), 1));
        ASSERT_TRUE(Actual);
        EXPECT_EQ(Actual->getZExtValue(), MemoryBytes[I])
            << "memory byte " << I;
      }
  }
}

TEST(X86LegacyVectorMoveAudit, FullValuesAliasesFlagsAndHighVectorBits) {
  for (Arch Target : {Arch::X86, Arch::X64})
    for (uint8_t Prefix : {uint8_t(0x66), uint8_t(0xf3)})
      for (uint8_t Opcode : {uint8_t(0x6f), uint8_t(0x7f)})
        for (unsigned Destination : {0u, 7u, 8u, 15u})
          for (unsigned Source : {0u, 7u, 8u, 15u}) {
            if (Target == Arch::X86 && (Destination >= 8 || Source >= 8))
              continue;
            const unsigned Reg = Opcode == 0x6f ? Destination : Source;
            const unsigned RM = Opcode == 0x6f ? Source : Destination;
            std::vector<uint8_t> Bytes{Prefix};
            const uint8_t Rex = (Reg >= 8 ? 4 : 0) | (RM >= 8 ? 1 : 0);
            if (Rex)
              Bytes.push_back(0x40 | Rex);
            Bytes.insert(
                Bytes.end(),
                {0x0f, Opcode, uint8_t(0xc0 | ((Reg & 7) << 3) | (RM & 7))});
            checkMove(Target, Bytes, Destination, Source, nullptr, false);
            if (testing::Test::HasFatalFailure())
              return;
          }
}

TEST(X86LegacyVectorMoveAudit,
     UnalignedMemoryAndAddressFormsPreserveOtherState) {
  const MemoryCase Cases[] = {
      {Arch::X64, {0x00}, 0, -1, 1, 0},
      {Arch::X64, {0x46, 0xfd}, 6, -1, 1, -3},
      {Arch::X64, {0x84, 0x94, 0x99, 0xba, 0xff, 0xff}, 4, 2, 4, -17767},
      {Arch::X64, {0x04, 0xe4}, 4, -1, 1, 0},
      {Arch::X64, {0x04, 0x4d, 0, 0x20, 0, 0}, -1, 1, 2, 0x2000},
      {Arch::X64, {0x05, 0xe1, 0xff, 0xff, 0xff}, -2, -1, 1, -31},
      {Arch::X64, {0x05, 0xe1, 0xff, 0xff, 0xff}, -2, -1, 1, -31, true},
      {Arch::X64, {0x00}, 8, -1, 1, 0, false, 1},
      {Arch::X64, {0x04, 0x00}, 0, 8, 1, 0, false, 2},
      {Arch::X64, {0x04, 0x4b}, 3, 1, 2, 0, true},
      {Arch::X86, {0x04, 0x4b}, 3, 1, 2, 0},
      {Arch::X86, {0x00}, 3, 6, 1, 0, true},
      {Arch::X86, {0x47, 0xfd}, 3, -1, 1, -3, true},
      {Arch::X86, {0x06, 0x01, 0x90}, -1, -1, 1, int16_t(0x9001), true},
  };
  for (const auto &C : Cases)
    for (bool Store : {false, true})
      for (unsigned Vector : {0u, 7u, 8u, 15u}) {
        if (C.Target == Arch::X86 && Vector >= 8)
          continue;
        std::vector<uint8_t> Bytes{0xf3};
        if (C.AddressPrefix)
          Bytes.push_back(0x67);
        uint8_t Rex = C.Rex | (Vector >= 8 ? 4 : 0);
        if (Rex)
          Bytes.push_back(0x40 | Rex);
        Bytes.insert(Bytes.end(), {0x0f, uint8_t(Store ? 0x7f : 0x6f)});
        auto ModRM = C.ModRM;
        ModRM[0] |= (Vector & 7) << 3;
        Bytes.insert(Bytes.end(), ModRM.begin(), ModRM.end());
        checkMove(C.Target, Bytes, Vector, Vector, &C, Store);
        if (testing::Test::HasFatalFailure())
          return;
      }
}

void expectUnaudited(Decoder &Dec, DecodedInsn &Insn) {
  LowInstructionUndefinedEffects Effects;
  Effects.Coverage = LowUndefinedCoverage::Complete;
  Effects.OperationDigest = "stale";
  Effects.Effects.push_back({1, NdVar::reg(x86reg::AF, 1), 0, 1, {}});
  LowInstructionPreservedState Preserved;
  Preserved.Audit = LowPreservedStateAudit::LegacyIntegerV1;
  std::vector<LowOp> Ops;
  try {
    Dec.liftToLow(Insn, Ops, {}, {}, &Effects, &Preserved);
  } catch (const UnliftedInstruction &) {
    // Rejection by the semantic lifter is also fail-closed admission.
  }
  EXPECT_NE(Effects.Coverage, LowUndefinedCoverage::Complete);
  EXPECT_TRUE(Effects.Effects.empty());
  EXPECT_NE(Effects.OperationDigest, "stale");
  EXPECT_EQ(Preserved, LowInstructionPreservedState{});
}

TEST(X86LegacyVectorMoveAudit, OtherEncodingsAndAlignedMemoryRemainUnaudited) {
  const std::vector<std::vector<uint8_t>> Cases = {
      {0x66, 0x0f, 0x6f,
       0x00}, // aligned load needs a separate alignment contract
      {0x66, 0x0f, 0x7f, 0x00},
      {0xc5, 0xfa, 0x6f, 0xc1},             // VEX.128
      {0xc5, 0xfe, 0x6f, 0xc1},             // VEX.256
      {0x62, 0xf1, 0x7e, 0x48, 0x6f, 0xc1}, // EVEX.512
      {0xf3, 0xf3, 0x0f, 0x6f, 0xc1},
      {0x66, 0x66, 0x0f, 0x6f, 0xc1},
      {0x66, 0xf3, 0x0f, 0x6f, 0xc1},
      {0xf3, 0x40, 0x40, 0x0f, 0x6f, 0xc1},
      {0xf3, 0x67, 0x67, 0x0f, 0x6f, 0x00},
      {0xf3, 0x64, 0x0f, 0x6f, 0x00},
      {0x0f, 0x6f, 0xc1},       // MMX
      {0xf3, 0x0f, 0x10, 0xc1}, // scalar merge, not a full move
  };
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  for (const auto &Bytes : Cases) {
    DecodedInsn Insn{};
    const int Size =
        Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn);
    if (Size <= 0)
      continue;
    ASSERT_EQ(Size, int(Bytes.size()));
    SCOPED_TRACE(::testing::Message()
                 << Insn.Raw->mnemonic << ' ' << Insn.Raw->op_str);
    expectUnaudited(Dec, Insn);
  }
}

TEST(X86LegacyVectorMoveAudit, MalformedRawAndDetailCannotAuthorizeCoverage) {
  using Mutation = void (*)(cs_insn &);
  const Mutation RegisterMutations[] = {
      [](cs_insn &I) { I.bytes[3] ^= 8; },
      [](cs_insn &I) { I.bytes[2] = 0x7f; },
      [](cs_insn &I) { I.bytes[I.size++] = 0x90; },
      [](cs_insn &I) { I.detail->x86.op_count = 1; },
      [](cs_insn &I) { I.detail->x86.operands[0].size = 8; },
      [](cs_insn &I) { I.detail->x86.operands[1].reg = X86_REG_XMM2; },
      [](cs_insn &I) { I.detail->x86.rex = 0x44; },
      [](cs_insn &I) { I.detail->x86.prefix[0] = 0xf3; },
      [](cs_insn &I) { I.detail->x86.prefix[2] = 0x66; },
      [](cs_insn &I) { I.detail->x86.addr_size = 4; },
      [](cs_insn &I) { I.detail->x86.encoding.modrm_offset = 2; },
      [](cs_insn &I) { I.detail->x86.operands[0].avx_zero_opmask = true; },
      [](cs_insn &I) { I.detail->x86.operands[1].avx_bcast = X86_AVX_BCAST_4; },
  };
  const Mutation MemoryMutations[] = {
      [](cs_insn &I) { I.detail->x86.operands[1].mem.base = X86_REG_RDX; },
      [](cs_insn &I) { I.detail->x86.operands[1].mem.index = X86_REG_RSI; },
      [](cs_insn &I) { I.detail->x86.operands[1].mem.scale = 3; },
      [](cs_insn &I) { I.detail->x86.operands[1].mem.segment = X86_REG_FS; },
      [](cs_insn &I) { I.detail->x86.operands[1].mem.disp += 1; },
      [](cs_insn &I) { I.detail->x86.encoding.disp_offset -= 1; },
      [](cs_insn &I) { I.detail->x86.encoding.disp_size = 4; },
      [](cs_insn &I) { I.detail->x86.operands[1].size = 8; },
      [](cs_insn &I) { I.bytes[4] ^= 8; },
  };
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  for (bool Memory : {false, true}) {
    const std::vector<uint8_t> Bytes =
        Memory ? std::vector<uint8_t>{0xf3, 0x0f, 0x6f, 0x44, 0x4b, 0xfd}
               : std::vector<uint8_t>{0xf3, 0x0f, 0x6f, 0xc1};
    const auto Mutations = Memory ? llvm::ArrayRef<Mutation>(MemoryMutations)
                                  : llvm::ArrayRef<Mutation>(RegisterMutations);
    for (unsigned Index = 0; Index != Mutations.size(); ++Index) {
      SCOPED_TRACE(::testing::Message() << Memory << '/' << Index);
      DecodedInsn Insn{};
      ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn),
                int(Bytes.size()));
      Mutations[Index](*Insn.Raw);
      expectUnaudited(Dec, Insn);
    }
  }
}

} // namespace
