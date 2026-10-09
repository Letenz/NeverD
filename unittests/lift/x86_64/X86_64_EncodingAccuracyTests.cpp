//===- X86_64_EncodingAccuracyTests.cpp - Encoding regressions ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"

#include <algorithm>
#include <vector>

using namespace neverd;

TEST(X86EncodingAccuracy, InvalidLockAndMovCsCannotPublishEffects) {
  const std::vector<std::vector<uint8_t>> Invalid = {
      {0xf0, 0x33, 0x02},       // LOCK XOR EAX,[EDX/RDX]: register destination.
      {0xf0, 0x01, 0xc0},       // LOCK ADD EAX,EAX.
      {0xf0, 0x89, 0x02},       // LOCK MOV [EDX/RDX],EAX.
      {0xf0, 0x39, 0x02},       // LOCK CMP [EDX/RDX],EAX.
      {0xf0, 0x0f, 0xa3, 0x02}, // LOCK BT: no write.
      {0x8e, 0xc8},             // MOV CS,AX.
      {0x8e, 0x0a},             // MOV CS,[EDX/RDX].
      {0x66, 0x8e, 0xc8}};
  for (Arch Target : {Arch::X86, Arch::X64})
    for (bool Strict : {false, true}) {
      Decoder Decode;
      ASSERT_TRUE(Decode.init(Target));
      Decode.setStrict(Strict);
      for (const auto &Bytes : Invalid) {
        SCOPED_TRACE(static_cast<unsigned>(Target));
        SCOPED_TRACE(Strict);
        SCOPED_TRACE(testing::PrintToString(Bytes));
        DecodedInsn Instruction;
        std::vector<LowOp> Ops(1);
        Ops[0].Addr = 0x50;
        if (Decode.decodeOne(Bytes.data(), Bytes.size(), 0x1000, Instruction))
          EXPECT_THROW(Decode.liftToLow(Instruction, Ops), UnliftedInstruction);
        ASSERT_EQ(Ops.size(), 1U);
        EXPECT_EQ(Ops[0].Addr, 0x50U);
      }
    }
}

TEST(X86EncodingAccuracy, LegalMemoryLockAndSegmentMovesStillLift) {
  const std::vector<std::vector<uint8_t>> Valid = {{0xf0, 0x31, 0x02},
                                                   {0xf0, 0x01, 0x02},
                                                   {0xf0, 0x0f, 0xab, 0x02},
                                                   {0xf0, 0x0f, 0xb1, 0x02},
                                                   {0xf0, 0x0f, 0xc1, 0x02},
                                                   {0xf0, 0x87, 0x02},
                                                   {0x8c, 0xc8},
                                                   {0x8e, 0xe0},
                                                   {0x8e, 0xe8}};
  for (Arch Target : {Arch::X86, Arch::X64}) {
    Decoder Decode;
    ASSERT_TRUE(Decode.init(Target));
    for (const auto &Bytes : Valid) {
      SCOPED_TRACE(testing::PrintToString(Bytes));
      DecodedInsn Instruction;
      ASSERT_EQ(
          Decode.decodeOne(Bytes.data(), Bytes.size(), 0x1000, Instruction),
          static_cast<int>(Bytes.size()));
      std::vector<LowOp> Ops;
      EXPECT_NO_THROW(Decode.liftToLow(Instruction, Ops));
      EXPECT_FALSE(Ops.empty());
    }
  }
}

TEST(X86EncodingAccuracy, Ud1ConsumesCompleteAddressAndAllRoutesAgree) {
  const std::vector<std::vector<uint8_t>> Complete = {
      {0x0f, 0xb9, 0x0a},
      {0x0f, 0xb9, 0x5e, 0x7e},
      {0x0f, 0xb9, 0xbd, 0xdd, 0xcc, 0xbb, 0xaa},
      {0x0f, 0xb9, 0x24, 0x50},
      {0x0f, 0xb9, 0x4c, 0x9a, 0x7e},
      {0x0f, 0xb9, 0x9c, 0xf6, 0xdd, 0xcc, 0xbb, 0xaa},
      {0x0f, 0xb9, 0xfd},
      {0x0f, 0xb9, 0x05, 1, 2, 3, 4},
      {0x0f, 0xb9, 0x04, 0x25, 1, 2, 3, 4}};
  for (Arch Target : {Arch::X86, Arch::X64})
    for (bool Detail : {false, true})
      for (bool Text : {false, true})
        for (unsigned Route : {0U, 1U, 2U}) {
          Decoder Decode;
          ASSERT_TRUE(Decode.init(Target));
          Decode.setDetail(Detail);
          Decode.setText(Text);
          for (const auto &Bytes : Complete) {
            SCOPED_TRACE(testing::PrintToString(Bytes));
            const auto Read = [&](const uint8_t *Data, size_t Size,
                                  DecodedInsn &Out) {
              if (Route == 0)
                return Decode.decodeOne(Data, Size, 0x1000, Out);
              if (Route == 1)
                return Decode.decodeOneLight(Data, Size, 0x1000, Out);
              return Decode.decodeOneForLift(Data, Size, 0x1000, Out);
            };
            DecodedInsn Instruction;
            for (unsigned Truncated = 0; Truncated < Bytes.size(); ++Truncated)
              EXPECT_EQ(Read(Bytes.data(), Truncated, Instruction), 0);
            auto Followed = Bytes;
            Followed.push_back(0x90);
            ASSERT_EQ(Read(Followed.data(), Followed.size(), Instruction),
                      static_cast<int>(Bytes.size()));
            EXPECT_EQ(Instruction.Id, X86_INS_UD1);
            EXPECT_EQ(Instruction.Size, Bytes.size());
            EXPECT_TRUE(
                std::equal(Bytes.begin(), Bytes.end(), Instruction.Raw->bytes));
            ASSERT_EQ(Read(Followed.data() + Instruction.Size, 1, Instruction),
                      1);
            EXPECT_EQ(Instruction.Id, X86_INS_NOP);
          }
        }
}

TEST(X86EncodingAccuracy, Ud1AddressOverridesAndLengthLimitAreExplicit) {
  const std::vector<std::vector<uint8_t>> Address16 = {
      {0x67, 0x0f, 0xb9, 0x06, 0x34, 0x12},
      {0x67, 0x0f, 0xb9, 0x86, 0x34, 0x12},
      {0x67, 0x0f, 0xb9, 0x44, 0x12}};
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Arch::X86));
  DecodedInsn Instruction;
  for (const auto &Bytes : Address16) {
    ASSERT_EQ(Decode.decodeOne(Bytes.data(), Bytes.size(), 0x1000, Instruction),
              static_cast<int>(Bytes.size()));
    EXPECT_EQ(Instruction.Raw->detail->x86.addr_size, 2);
  }
  ASSERT_TRUE(Decode.init(Arch::X64));
  const uint8_t Address32[] = {0x67, 0x0f, 0xb9, 0x84, 0x25, 1, 2, 3, 4};
  ASSERT_EQ(Decode.decodeOne(Address32, sizeof(Address32), 0x1000, Instruction),
            9);
  EXPECT_EQ(Instruction.Raw->detail->x86.addr_size, 4);
  EXPECT_EQ(Instruction.Raw->detail->x86.encoding.disp_offset, 5);
  EXPECT_EQ(Instruction.Raw->detail->x86.encoding.disp_size, 4);
  std::vector<uint8_t> Maximum(12, 0x66);
  Maximum.insert(Maximum.end(), {0x0f, 0xb9, 0xc0});
  ASSERT_EQ(
      Decode.decodeOne(Maximum.data(), Maximum.size(), 0x1000, Instruction),
      15);
  Maximum.insert(Maximum.begin(), 0x66);
  EXPECT_EQ(
      Decode.decodeOne(Maximum.data(), Maximum.size(), 0x1000, Instruction), 0);
}
