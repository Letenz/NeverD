//===- SymConcreteTests.cpp - Exact concrete shadow tests -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymConcrete.h"

#include <limits>
#include <vector>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

LowOp op(NdOp Opcode, NdVar Output, std::initializer_list<NdVar> Inputs) {
  LowOp Result;
  Result.Opcode = Opcode;
  Result.Output = Output;
  for (const NdVar &Input : Inputs)
    Result.addInput(Input);
  return Result;
}

std::optional<uint64_t> binaryValue(NdOp Opcode, uint16_t Bytes, uint64_t Left,
                                    uint64_t Right, uint16_t OutputBytes = 0) {
  constexpr uint64_t LeftRegister = 0;
  constexpr uint64_t RightRegister = 8;
  constexpr uint64_t OutputRegister = 16;

  SymExecConcreteShadow Shadow;
  const uint16_t ResultBytes = OutputBytes == 0 ? Bytes : OutputBytes;
  if (!Shadow.reset(llvm::endianness::little) ||
      !Shadow.setRegister(LeftRegister, Bytes, Left) ||
      !Shadow.setRegister(RightRegister, Bytes, Right) ||
      !Shadow.step(op(
          Opcode, NdVar::reg(OutputRegister, ResultBytes),
          {NdVar::reg(LeftRegister, Bytes), NdVar::reg(RightRegister, Bytes)})))
    return std::nullopt;
  return Shadow.value(NdVar::reg(OutputRegister, ResultBytes));
}

TEST(SymExecConcreteShadow, NarrowSignedCompareUsesTheDeclaredWidth) {
  constexpr uint64_t Input = 0;
  constexpr uint64_t Predicate = 16;

  SymExecConcreteShadow Shadow;
  ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
  ASSERT_TRUE(Shadow.setRegister(Input, 1, 0xFF));
  ASSERT_TRUE(Shadow.step(op(NdOp::INT_SLESS, NdVar::reg(Predicate, 1),
                             {NdVar::reg(Input, 1), NdVar::cst(0, 1)})));

  ASSERT_TRUE(Shadow.value(NdVar::reg(Predicate, 1)).has_value());
  EXPECT_EQ(*Shadow.value(NdVar::reg(Predicate, 1)), 1u);
}

TEST(SymExecConcreteShadow, CoreIntegerSemanticsUseEverySupportedSeedWidth) {
  for (uint16_t Bytes : {uint16_t(1), uint16_t(2), uint16_t(4), uint16_t(8)}) {
    SCOPED_TRACE(Bytes);
    const unsigned Bits = unsigned(Bytes) * 8;
    const uint64_t SignBit = uint64_t(1) << (Bits - 1);
    const uint64_t Mask = Bytes == sizeof(uint64_t)
                              ? std::numeric_limits<uint64_t>::max()
                              : (uint64_t(1) << Bits) - 1;

    EXPECT_EQ(binaryValue(NdOp::INT_EQUAL, Bytes, Mask, Mask, 1), 1u);
    EXPECT_EQ(binaryValue(NdOp::INT_SLESS, Bytes, SignBit, 1, 1), 1u);
    EXPECT_EQ(binaryValue(NdOp::INT_CARRY, Bytes, Mask, 1, 1), 1u);
    EXPECT_EQ(binaryValue(NdOp::INT_SOVF, Bytes, SignBit - 1, 1, 1), 1u);

    EXPECT_EQ(binaryValue(NdOp::INT_DIV, Bytes, Mask, 3), Mask / 3);
    EXPECT_EQ(binaryValue(NdOp::INT_SDIV, Bytes, Mask - 7, 2), Mask - 3)
        << "-8 / 2 == -4";
    EXPECT_EQ(binaryValue(NdOp::INT_REM, Bytes, Mask, 7), Mask % 7);
    EXPECT_EQ(binaryValue(NdOp::INT_SREM, Bytes, Mask - 8, 4), Mask)
        << "-9 % 4 == -1";

    EXPECT_EQ(binaryValue(NdOp::INT_LEFT, Bytes, Mask, 1), Mask - 1);
    EXPECT_EQ(binaryValue(NdOp::INT_RIGHT, Bytes, SignBit, 1), SignBit >> 1);
    EXPECT_EQ(binaryValue(NdOp::INT_ASHR, Bytes, SignBit, 1),
              SignBit | (SignBit >> 1));
  }
}

TEST(SymExecConcreteShadow, WiderShiftCountCannotWrapIntoAnInRangeCount) {
  for (NdOp Opcode : {NdOp::INT_LEFT, NdOp::INT_RIGHT, NdOp::INT_ASHR}) {
    SymExecConcreteShadow Shadow;
    ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
    ASSERT_TRUE(Shadow.setRegister(0, 1, 0x91));
    ASSERT_TRUE(Shadow.setRegister(8, 2, 256));
    ASSERT_TRUE(Shadow.step(
        op(Opcode, NdVar::reg(16, 1), {NdVar::reg(0, 1), NdVar::reg(8, 2)})));
    EXPECT_EQ(Shadow.value(NdVar::reg(16, 1)),
              Opcode == NdOp::INT_ASHR ? 0xffu : 0u);
  }
}

TEST(SymExecConcreteShadow, SeedsAreCompleteNonOverlappingByteRanges) {
  SymExecConcreteShadow Shadow;

  ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
  EXPECT_FALSE(Shadow.setRegister(0, 0, 0));

  ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
  EXPECT_FALSE(Shadow.setRegister(0, 9, 0));

  ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
  EXPECT_FALSE(Shadow.setRegister(0, 1, 0x100));

  ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
  EXPECT_FALSE(Shadow.setRegister(std::numeric_limits<uint64_t>::max(), 2, 0));

  ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
  ASSERT_TRUE(Shadow.setRegister(0, 4, 0x55667788));
  EXPECT_FALSE(Shadow.setRegister(2, 4, 0x11223344));

  ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
  ASSERT_TRUE(Shadow.setRegister(0, 4, 0x55667788));
  ASSERT_TRUE(Shadow.setRegister(4, 4, 0x11223344));
  EXPECT_EQ(Shadow.value(NdVar::reg(0, 8)), 0x1122334455667788ULL);

  ASSERT_TRUE(Shadow.reset(llvm::endianness::big));
  ASSERT_TRUE(Shadow.setRegister(0, 4, 0x11223344));
  ASSERT_TRUE(Shadow.setRegister(4, 4, 0x55667788));
  EXPECT_EQ(Shadow.value(NdVar::reg(0, 8)), 0x1122334455667788ULL);
}

TEST(SymExecConcreteShadow, MissingRegisterAndTemporaryBytesFailClosed) {
  SymExecConcreteShadow Shadow;
  ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
  ASSERT_TRUE(Shadow.setRegister(0, 4, 0x12345678));
  EXPECT_FALSE(
      Shadow.step(op(NdOp::COPY, NdVar::reg(16, 8), {NdVar::reg(0, 8)})));
  EXPECT_FALSE(Shadow.value(NdVar::reg(16, 8)).has_value());

  ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
  EXPECT_FALSE(
      Shadow.step(op(NdOp::COPY, NdVar::reg(16, 8), {NdVar::tmp(0, 8)})));
}

TEST(SymExecConcreteShadow, UnseededMemoryAndApproximateEffectsFailClosed) {
  SymExecConcreteShadow Shadow;
  ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
  EXPECT_FALSE(Shadow.step(
      op(NdOp::LOAD, NdVar::reg(16, 4), {NdVar::cst(0x401000, 8)})));

  ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
  ASSERT_TRUE(Shadow.setRegister(0, 8, 4));
  EXPECT_FALSE(
      Shadow.step(op(NdOp::FLOAT_SQRT, NdVar::reg(16, 8), {NdVar::reg(0, 8)})));

  ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
  EXPECT_FALSE(Shadow.step(op(NdOp::CALL, NdVar{}, {NdVar::cst(0x402000, 8)})));
}

TEST(SymExecConcreteShadow, ExactStoreThenLoadKeepsTheShadowComplete) {
  SymExecConcreteShadow Shadow;
  ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
  ASSERT_TRUE(
      Shadow.step(op(NdOp::STORE, NdVar{},
                     {NdVar::cst(0x401000, 8), NdVar::cst(0x12345678, 4)})));
  ASSERT_TRUE(Shadow.step(
      op(NdOp::LOAD, NdVar::reg(16, 4), {NdVar::cst(0x401000, 8)})));
  EXPECT_EQ(Shadow.value(NdVar::reg(16, 4)), 0x12345678u);
}

TEST(SymExecConcreteShadow, WideCopiesAndMemoryPreserveEveryByte) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    for (uint16_t Bytes : {uint16_t(16), uint16_t(32)}) {
      SCOPED_TRACE(Bytes);
      SCOPED_TRACE(Order == llvm::endianness::little ? "little" : "big");
      std::vector<uint8_t> Input(Bytes);
      for (uint16_t I = 0; I < Bytes; ++I)
        Input[I] = uint8_t(0x81 + 7 * I);

      SymExecConcreteShadow Shadow;
      ASSERT_TRUE(Shadow.reset(Order));
      ASSERT_TRUE(Shadow.setMemory(0x1000, Input));
      ASSERT_TRUE(Shadow.setMemory(0x2000, {0x5A}));
      ASSERT_TRUE(Shadow.setMemory(0x2001 + Bytes, {0xA5}));
      ASSERT_TRUE(Shadow.step(
          op(NdOp::LOAD, NdVar::reg(0, Bytes), {NdVar::cst(0x1000, 8)})));
      ASSERT_TRUE(Shadow.step(
          op(NdOp::COPY, NdVar::tmp(0, Bytes), {NdVar::reg(0, Bytes)})));
      ASSERT_TRUE(
          Shadow.step(op(NdOp::STORE, NdVar{},
                         {NdVar::cst(0x2001, 8), NdVar::tmp(0, Bytes)})));
      ASSERT_TRUE(Shadow.step(
          op(NdOp::LOAD, NdVar::reg(64, Bytes), {NdVar::cst(0x2001, 8)})));
      for (uint16_t I = 0; I < Bytes; ++I)
        EXPECT_EQ(Shadow.value(NdVar::reg(64 + I, 1)), Input[I]);
      ASSERT_TRUE(Shadow.step(
          op(NdOp::LOAD, NdVar::reg(128, 1), {NdVar::cst(0x2000, 8)})));
      EXPECT_EQ(Shadow.value(NdVar::reg(128, 1)), 0x5Au);
      ASSERT_TRUE(Shadow.step(
          op(NdOp::LOAD, NdVar::reg(128, 1), {NdVar::cst(0x2001 + Bytes, 8)})));
      EXPECT_EQ(Shadow.value(NdVar::reg(128, 1)), 0xA5u);
    }
  }
}

TEST(SymExecConcreteShadow, WideInputsWithMissingBytesStillFailClosed) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    for (NdOp Opcode : {NdOp::COPY, NdOp::INT_XOR, NdOp::STORE}) {
      SCOPED_TRACE(static_cast<unsigned>(Opcode));
      SymExecConcreteShadow Shadow;
      ASSERT_TRUE(Shadow.reset(Order));
      for (uint64_t I = 0; I < 15; ++I)
        ASSERT_TRUE(Shadow.setRegister(I, 1, 0x80 + I));
      const auto Wide = NdVar::reg(0, 16);
      const LowOp Operation =
          Opcode == NdOp::STORE
              ? op(Opcode, NdVar{}, {NdVar::cst(0x1000, 8), Wide})
          : Opcode == NdOp::INT_XOR
              ? op(Opcode, NdVar::reg(32, 16), {Wide, Wide})
              : op(Opcode, NdVar::reg(32, 16), {Wide});
      // XOR could fold to zero, but its complete input is still required.
      EXPECT_FALSE(Shadow.step(Operation));
      EXPECT_FALSE(Shadow.value(NdVar::reg(0, 1)).has_value());
    }
  }
}

TEST(SymExecConcreteShadow, WideAddressOperandsRemainUnsupported) {
  for (NdOp Opcode : {NdOp::LOAD, NdOp::STORE}) {
    SymExecConcreteShadow Shadow;
    ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
    ASSERT_TRUE(Shadow.setRegister(0, 8, 0x1000));
    ASSERT_TRUE(Shadow.setRegister(8, 8, 0));
    ASSERT_TRUE(Shadow.setMemory(0x1000, {0x5A}));
    const auto Address = NdVar::reg(0, 16);
    EXPECT_FALSE(
        Shadow.step(Opcode == NdOp::LOAD
                        ? op(Opcode, NdVar::reg(32, 1), {Address})
                        : op(Opcode, NdVar{}, {Address, NdVar::cst(0xA5, 1)})));
    EXPECT_FALSE(Shadow.value(NdVar::reg(0, 8)).has_value());
  }
}

TEST(SymExecConcreteShadow, WideValueQueriesRetainTheScalarWidthLimit) {
  for (uint64_t High : {uint64_t(0), uint64_t(1) << 63}) {
    SymExecConcreteShadow Shadow;
    ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
    ASSERT_TRUE(Shadow.setRegister(0, 8, 0x1234));
    ASSERT_TRUE(Shadow.setRegister(8, 8, High));
    EXPECT_EQ(Shadow.value(NdVar::reg(0, 8)), 0x1234u);
    EXPECT_FALSE(Shadow.value(NdVar::reg(0, 16)).has_value());
    EXPECT_FALSE(Shadow.value(NdVar::reg(0, 8)).has_value());
  }
}

TEST(SymExecConcreteShadow, EntryMemoryUsesAddressOrderAndIndependentBanks) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SymExecConcreteShadow Shadow;
    ASSERT_TRUE(Shadow.reset(Order));
    ASSERT_TRUE(Shadow.setRegister(0, 8, 0x87654321));
    ASSERT_TRUE(Shadow.setMemory(2, {0x33, 0x44}));
    ASSERT_TRUE(Shadow.setMemory(0, {0x11, 0x22}));
    ASSERT_TRUE(
        Shadow.step(op(NdOp::LOAD, NdVar::reg(16, 4), {NdVar::cst(0, 8)})));
    EXPECT_EQ(Shadow.value(NdVar::reg(16, 4)),
              Order == llvm::endianness::little ? 0x44332211u : 0x11223344u);
    EXPECT_EQ(Shadow.value(NdVar::reg(0, 8)), 0x87654321u);
  }
}

TEST(SymExecConcreteShadow, SnapshotBytesAreMutableAndLoadsKeepAliases) {
  SymExecConcreteShadow Shadow;
  ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
  ASSERT_TRUE(Shadow.setRegister(0, 8, 0x1001));
  ASSERT_TRUE(Shadow.setMemory(0x1000, {0x11, 0x22, 0x33, 0x44}));
  ASSERT_TRUE(Shadow.step(
      op(NdOp::STORE, NdVar{}, {NdVar::reg(0, 8), NdVar::cst(0xBBAA, 2)})));
  ASSERT_TRUE(
      Shadow.step(op(NdOp::LOAD, NdVar::reg(16, 4), {NdVar::cst(0x1000, 8)})));
  EXPECT_EQ(Shadow.value(NdVar::reg(16, 4)), 0x44BBAA11u);
}

TEST(SymExecConcreteShadow,
     MemorySeedsRejectEmptyWrappedOverlappingAndLateData) {
  SymExecConcreteShadow Shadow;
  ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
  EXPECT_FALSE(Shadow.setMemory(0, {}));
  EXPECT_FALSE(Shadow.setMemory(0, {1}));

  ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
  EXPECT_FALSE(Shadow.setMemory(UINT64_MAX, {1, 2}));

  for (uint64_t Address : {0x1000u, 0x1001u, 0x1003u}) {
    ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
    ASSERT_TRUE(Shadow.setMemory(0x1001, {1, 2, 3}));
    EXPECT_FALSE(Shadow.setMemory(Address, {1, 2}));
  }

  ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
  ASSERT_TRUE(Shadow.setMemory(UINT64_MAX, {0x5A}));
  ASSERT_TRUE(Shadow.step(
      op(NdOp::LOAD, NdVar::reg(16, 1), {NdVar::cst(UINT64_MAX, 8)})));
  EXPECT_EQ(Shadow.value(NdVar::reg(16, 1)), 0x5Au);
  EXPECT_FALSE(Shadow.setMemory(0, {1}));
  EXPECT_FALSE(Shadow.value(NdVar::reg(16, 1)).has_value());
}

TEST(SymExecConcreteShadow, MemorySeedsNeverFillHolesOrSurviveReset) {
  SymExecConcreteShadow Shadow;
  ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
  ASSERT_TRUE(Shadow.setMemory(0x1000, {1}));
  ASSERT_TRUE(Shadow.setMemory(0x1002, {3, 4}));
  EXPECT_FALSE(
      Shadow.step(op(NdOp::LOAD, NdVar::reg(16, 4), {NdVar::cst(0x1000, 8)})));

  ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
  ASSERT_TRUE(Shadow.setMemory(0x1000, {1, 2, 3, 4}));
  ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
  EXPECT_FALSE(
      Shadow.step(op(NdOp::LOAD, NdVar::reg(16, 4), {NdVar::cst(0x1000, 8)})));
}

} // namespace
