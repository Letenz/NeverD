//===- X86_64_DisplacementDetailTests.cpp - Encoded address fields --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"

#include <cstdint>
#include <optional>
#include <vector>

using namespace neverd;

namespace {
constexpr va_t Entry = 0x1000;

int decode(Decoder &D, unsigned Route, const std::vector<uint8_t> &Bytes,
           DecodedInsn &Out) {
  if (Route == 0)
    return D.decodeOne(Bytes.data(), Bytes.size(), Entry, Out);
  if (Route == 1)
    return D.decodeOneForLift(Bytes.data(), Bytes.size(), Entry, Out);
  return D.decodeOneLight(Bytes.data(), Bytes.size(), Entry, Out);
}

TEST(X86DecodeDetail, LongModeDisplacementWidthFollowsAddressEncoding) {
  // Independently encoded MOV/SHL/ROL forms: base, SIB, RIP/EIP-relative,
  // absolute SIB, both address sizes and signed displacement extremes.
  struct Form {
    std::vector<uint8_t> Prefix;
    uint8_t Opcode, ModRM;
    bool SIB, Immediate;
  };
  const Form Forms[] = {
      {{0x66}, 0xc1, 0xa3, false, true},
      {{0x66}, 0xc1, 0xa4, true, true},
      {{0x66}, 0xc1, 0x84, true, true},
      {{0x66}, 0x8b, 0x05, false, false},
      {{0x66}, 0x8b, 0x04, true, false},
      {{0x66, 0x0f}, 0x6f, 0x84, true, false}, // MOVDQA has a mandatory 66.
      {{0x67, 0x66}, 0xc1, 0xa3, false, true},
      {{0x67, 0x66}, 0xc1, 0xa4, true, true},
      {{0x67, 0x66}, 0x8b, 0x05, false, false},
      {{0x67, 0x66}, 0x8b, 0x04, true, false},
      {{}, 0xc1, 0xa4, true, true},
      {{0x48}, 0xc1, 0xa4, true, true},
      {{0x66, 0x48}, 0xc1, 0xa4, true, true},
  };
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X64));
  for (unsigned Route = 0; Route != 3; ++Route)
    for (const auto &F : Forms)
      for (uint32_t Disp :
           {0U, 0x12345678U, 0x7fffffffU, 0x80000000U, 0xfffffff0U}) {
        auto Bytes = F.Prefix;
        Bytes.insert(Bytes.end(), {F.Opcode, F.ModRM});
        if (F.SIB)
          Bytes.push_back((F.ModRM >> 6) == 0 ? 0x4d : 0x4c);
        const auto Offset = Bytes.size();
        for (unsigned I = 0; I != 4; ++I)
          Bytes.push_back(Disp >> (I * 8));
        if (F.Immediate)
          Bytes.push_back(3);
        SCOPED_TRACE(testing::Message() << Route << ": " << Disp << ": "
                                        << testing::PrintToString(Bytes));
        DecodedInsn Insn{};
        ASSERT_EQ(decode(D, Route, Bytes, Insn), Bytes.size());
        const auto &X = Insn.Raw->detail->x86;
        EXPECT_EQ(X.encoding.disp_offset, Offset);
        EXPECT_EQ(X.encoding.disp_size, 4);
        EXPECT_EQ(X.disp, static_cast<int32_t>(Disp));
        EXPECT_EQ(X.encoding.imm_size, F.Immediate ? 1 : 0);
        EXPECT_EQ(X.encoding.imm_offset, F.Immediate ? Offset + 4 : 0);
      }
}

TEST(X86DecodeDetail, GenuineNarrowDisplacementsAndMoffsKeepTheirWidths) {
  struct Case {
    Arch Target;
    std::vector<uint8_t> Bytes;
    uint8_t Offset, Size;
  };
  const Case Cases[] = {
      {Arch::X64, {0x66, 0xc1, 0x63, 0xf0, 3}, 3, 1},
      {Arch::X64, {0x66, 0xc1, 0xe0, 3}, 0, 0},
      {Arch::X86, {0x67, 0xc1, 0xa0, 0x34, 0x12, 3}, 3, 2},
      {Arch::X86, {0x67, 0x66, 0xc1, 0xa0, 0x34, 0x12, 3}, 4, 2},
      {Arch::X86, {0x66, 0xc1, 0xa3, 0x34, 0x12, 0, 0, 3}, 3, 4},
      {Arch::X64, {0x66, 0xa1, 0x78, 0x56, 0x34, 0x12, 0, 0, 0, 0}, 2, 8},
      {Arch::X64, {0x67, 0x66, 0xa1, 0x78, 0x56, 0x34, 0x12}, 3, 4},
  };
  for (const auto &C : Cases)
    for (unsigned Route = 0; Route != 3; ++Route) {
      Decoder D;
      ASSERT_TRUE(D.init(C.Target));
      DecodedInsn Insn{};
      SCOPED_TRACE(testing::PrintToString(C.Bytes));
      ASSERT_EQ(decode(D, Route, C.Bytes, Insn), C.Bytes.size());
      EXPECT_EQ(Insn.Raw->detail->x86.encoding.disp_offset, C.Offset);
      EXPECT_EQ(Insn.Raw->detail->x86.encoding.disp_size, C.Size);
    }
}

TEST(X86DecodeDetail, CorrectedFieldBindsOnlyExactRelocationOccurrence) {
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X64));
  const std::vector<uint8_t> Bytes = {0x66, 0x8b, 0x05, 0x78,
                                      0x56, 0x34, 0x12}; // MOV AX,[RIP+disp32].
  for (unsigned Route = 0; Route != 3; ++Route)
    for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
      DecodedInsn Insn{};
      ASSERT_EQ(decode(D, Route, Bytes, Insn), Bytes.size());
      RelocatedAddressOperand R;
      R.FieldVA = Entry + 3;
      R.EncodedValue = 0x12345678;
      R.TargetVA = 0x4000;
      R.Width = 4;
      R.Provenance = ConstantAddressProvenance::DataAddress;
      R.PCRelativeFromInstructionEnd = true;
      if (Mutation == 1)
        R.Width = 2;
      if (Mutation == 2)
        ++R.FieldVA;
      if (Mutation == 3)
        ++R.EncodedValue;
      std::vector<LowOp> Ops;
      D.liftToLow(Insn, Ops, {R});
      std::optional<NdVar> RelocatedEA;
      for (const auto &Op : Ops)
        if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
            Op.Inputs[0].isConst() &&
            Op.Inputs[0].Provenance == ConstantAddressProvenance::DataAddress) {
          EXPECT_EQ(Op.Inputs[0].Offset, R.TargetVA);
          RelocatedEA = Op.Output;
        }
      EXPECT_EQ(RelocatedEA.has_value(), Mutation == 0);
      bool Found = false;
      for (const auto &Op : Ops)
        if (Op.Opcode == NdOp::LOAD) {
          Found = true;
          ASSERT_EQ(Op.NumInputs, 1);
          EXPECT_EQ(Op.Output.Size, 2);
          if (RelocatedEA)
            EXPECT_EQ(Op.Inputs[0], *RelocatedEA);
        }
      EXPECT_TRUE(Found);
    }
}

TEST(X86DecodeDetail,
     CorrectedShiftsRetainCompleteEffectsAndRejectStaleDetail) {
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X64));
  const std::vector<uint8_t> Bytes = {0x66, 0xc1, 0xa4, 0x4c, 0x20, 0, 0, 0, 3};
  for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
    DecodedInsn Insn{};
    ASSERT_EQ(decode(D, 1, Bytes, Insn), Bytes.size());
    auto &X = Insn.Raw->detail->x86;
    if (Mutation == 1)
      X.encoding.disp_size = 2;
    if (Mutation == 2)
      ++X.encoding.disp_offset;
    std::vector<LowOp> Ops;
    LowInstructionUndefinedEffects Effects;
    D.liftToLow(Insn, Ops, {}, {}, &Effects);
    EXPECT_EQ(Effects.Coverage, Mutation == 0 ? LowUndefinedCoverage::Complete
                                              : LowUndefinedCoverage::Missing);
    EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Ops));
    EXPECT_EQ(Effects.Effects.size(), Mutation == 0 ? 2U : 0U);
  }
}

TEST(X86DecodeDetail, TruncationAndDetailFreeReuseDoNotManufactureFields) {
  const std::vector<uint8_t> Bytes = {0x66, 0xc1, 0xa4, 0x4c, 0x20, 0, 0, 0, 3};
  for (unsigned Route = 0; Route != 3; ++Route) {
    Decoder D;
    ASSERT_TRUE(D.init(Arch::X64));
    DecodedInsn Insn{};
    ASSERT_EQ(decode(D, Route, Bytes, Insn), Bytes.size());
    for (size_t Size = 0; Size != Bytes.size(); ++Size)
      EXPECT_EQ(decode(D, Route, {Bytes.begin(), Bytes.begin() + Size}, Insn),
                0);
    D.setDetail(false);
    EXPECT_EQ(decode(D, Route, Bytes, Insn), Bytes.size());
    D.setDetail(true);
    D.setText(false);
    EXPECT_EQ(decode(D, Route, Bytes, Insn), Bytes.size());
    D.setText(true);
    ASSERT_EQ(decode(D, Route, Bytes, Insn), Bytes.size());
    EXPECT_EQ(Insn.Raw->detail->x86.encoding.disp_size, 4);
  }
}
} // namespace
