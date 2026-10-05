//===- AArch64SIMDTests.cpp - Independent checked ARM64 FP/SIMD results ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"

#include "llvm/Support/Endian.h"

#include <tuple>

namespace neverd::emulation {
namespace {
#define NEVERD_AARCH64_FP_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "AArch64FPCases.def"
#undef NEVERD_AARCH64_FP_VALUE
struct FPCase {
  const char *Name;
  uint32_t Word;
  uint64_t First, Second, Result;
  uint32_t Control, Status;
};
constexpr FPCase FloatingCases[] = {
#define NEVERD_AARCH64_FP_INSTRUCTION(Name, Word, First, Second, Result,       \
                                      Control, Status)                         \
  {#Name, Word, First, Second, Result, Control, Status},
#include "AArch64FPCases.def"
#undef NEVERD_AARCH64_FP_INSTRUCTION
};

class AArch64SIMD
    : public testing::TestWithParam<std::tuple<ExecutionBackendKind, bool>> {
protected:
  static constexpr unsigned ScalarCount = unsigned(AArch64Register::FPSR) + 1;
  using ScalarState = std::array<uint64_t, ScalarCount>;
  using VectorState = std::array<RegisterValue, LastVector + 1>;
  std::unique_ptr<ExecutionBackend> CPU;
  void SetUp() override {
    const bool User = std::get<1>(GetParam());
    auto Created =
        createExecutionBackend(std::get<0>(GetParam()),
                               User ? ExecutionContract::CheckedUserAArch64
                                    : ExecutionContract::CheckedAArch64,
                               Limit, GuestArchitecture::AArch64);
    if (!Created) {
      auto E = Created.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Text = llvm::toString(std::move(E));
      if (Unavailable &&
          !requireHvf(std::get<0>(GetParam()), GuestArchitecture::AArch64))
        GTEST_SKIP() << Text;
      FAIL() << Text;
    }
    CPU = std::move(Created->CPU);
    llvm::cantFail(
        CPU->map(Code, PageSize, Read | Write | Execute | UserAccessible));
    llvm::cantFail(CPU->map(Stack, PageSize, Read | Write | UserAccessible));
    llvm::cantFail(
        CPU->setReg(AArch64Register::SP, Stack + PageSize - StackAlignment));
    llvm::cantFail(CPU->setReg(AArch64Register::NZCV, InitialNZCV));
    for (unsigned Index = 0; Index <= LastVector; ++Index)
      llvm::cantFail(CPU->writeRegister(
          vectorRegister(GuestArchitecture::AArch64, Index),
          {InitialVectorLow + Index, InitialVectorHigh - Index}));
  }
  RegisterValue vector(unsigned Index) {
    return llvm::cantFail(
        CPU->readRegister(vectorRegister(GuestArchitecture::AArch64, Index)));
  }
  void vector(unsigned Index, RegisterValue Value) {
    llvm::cantFail(CPU->writeRegister(
        vectorRegister(GuestArchitecture::AArch64, Index), Value));
  }
  ScalarState scalars() {
    ScalarState State;
    for (unsigned Index = 0; Index < State.size(); ++Index)
      State[Index] = llvm::cantFail(CPU->reg(AArch64Register(Index)));
    return State;
  }
  VectorState vectors() {
    VectorState State;
    for (unsigned Index = 0; Index < State.size(); ++Index)
      State[Index] = vector(Index);
    return State;
  }
  ExecutionExit run(uint32_t Word) {
    uint8_t Bytes[WordBytes];
    llvm::support::endian::write32le(Bytes, Word);
    llvm::support::endian::write32le(Bytes + sizeof(uint32_t), Nop);
    llvm::cantFail(CPU->write(Code, Bytes));
    BackendHooks Hooks;
    Hooks.Instruction = [&](uint64_t PC, uint32_t) {
      if (PC != Code)
        CPU->stop();
    };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
    return llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  }
  void expectSuccess(const ExecutionExit &Exit) {
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::PC)),
              Code + sizeof(uint32_t));
  }
  void expectRejected(uint32_t Word) {
    auto ScalarBefore = scalars();
    const auto VectorBefore = vectors();
    ScalarBefore[unsigned(AArch64Register::PC)] = Code;
    const auto Exit = run(Word);
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
        << Exit.Diagnostic;
    EXPECT_EQ(vectors(), VectorBefore);
    EXPECT_EQ(scalars(), ScalarBefore);
  }
  void expectWrappedPairRejected(uint32_t Word, unsigned Width) {
    llvm::cantFail(CPU->map(LastPage, PageSize, Read | Write | UserAccessible));
    llvm::cantFail(CPU->map(0, PageSize, Read | Write | UserAccessible));
    const auto Address = LastPage + PageSize - Width;
    llvm::cantFail(CPU->setReg(AArch64Register::X1, Address));
    std::array<uint8_t, ByteCount> Original{}, High{}, Low{};
    Original.fill(ByteMask);
    llvm::cantFail(
        CPU->write(Address, llvm::ArrayRef(Original).take_front(Width)));
    llvm::cantFail(CPU->write(0, Original));
    expectRejected(Word);
    llvm::cantFail(CPU->snapshotBacking(
        Address, llvm::MutableArrayRef(High).take_front(Width)));
    llvm::cantFail(CPU->snapshotBacking(0, Low));
    EXPECT_EQ(Low, Original);
    EXPECT_EQ(llvm::ArrayRef(High).take_front(Width),
              llvm::ArrayRef(Original).take_front(Width));
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::X1)), Address);
  }
};
TEST_P(AArch64SIMD, LogicalInstructionsUseBothIndependentLanes) {
  const auto First = vector(FirstOperand), Second = vector(SecondOperand);
  for (auto Word : {VectorXor, VectorAnd, VectorOr}) {
    SCOPED_TRACE(Word);
    ASSERT_NO_FATAL_FAILURE(expectSuccess(run(Word)));
    const auto Actual = vector(LastVector);
    for (unsigned Lane = 0; Lane < Actual.size(); ++Lane) {
      const auto Expected = Word == VectorXor   ? First[Lane] ^ Second[Lane]
                            : Word == VectorAnd ? First[Lane] & Second[Lane]
                                                : First[Lane] | Second[Lane];
      EXPECT_EQ(Actual[Lane], Expected);
    }
    EXPECT_EQ(vector(FirstOperand), First);
    EXPECT_EQ(vector(SecondOperand), Second);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::NZCV)), InitialNZCV);
  }
}
TEST_P(AArch64SIMD, PackedArithmeticWrapsEachByteWithoutCrossLaneCarry) {
  const auto First = vector(FirstOperand), Second = vector(SecondOperand);
  for (auto Word : {VectorAddBytes, VectorSubBytes}) {
    SCOPED_TRACE(Word);
    RegisterValue Expected{};
    for (unsigned Byte = 0; Byte < ByteCount; ++Byte) {
      const unsigned Lane = Byte / WordBytes;
      const unsigned Shift = (Byte % WordBytes) * BytesPerBit;
      const auto Left = uint8_t(First[Lane] >> Shift);
      const auto Right = uint8_t(Second[Lane] >> Shift);
      const auto Value = Word == VectorAddBytes ? uint8_t(Left + Right)
                                                : uint8_t(Left - Right);
      Expected[Lane] |= uint64_t(Value) << Shift;
    }
    ASSERT_NO_FATAL_FAILURE(expectSuccess(run(Word)));
    EXPECT_EQ(vector(LastVector), Expected);
  }
}
TEST_P(AArch64SIMD, IntegerDuplicationFillsBothLanes) {
  llvm::cantFail(CPU->setReg(AArch64Register::X0, InitialScalar));
  ASSERT_NO_FATAL_FAILURE(expectSuccess(run(DuplicateInteger)));
  EXPECT_EQ(vector(LastVector), (RegisterValue{InitialScalar, InitialScalar}));
}
TEST_P(AArch64SIMD, CMHIProducesUnsignedElementMasksAndPreservesOtherState) {
  struct Comparison {
    const char *Name;
    uint32_t Word;
    unsigned Bits, Elements, Destination, Left, Right;
  };
  constexpr Comparison Cases[] = {
#define NEVERD_AARCH64_CMHI_CASE(Name, Word, Bits, Count, Dest, Left, Right)   \
  {#Name, Word, Bits, Count, Dest, Left, Right},
#include "AArch64CMHICases.def"
#undef NEVERD_AARCH64_CMHI_CASE
  };
  struct Operands {
    RegisterValue Left, Right;
  };
  constexpr Operands Inputs[] = {
      {{0x80ff7f000102ff80ULL, 0xffffffff00000001ULL},
       {0x7fff80000102fe80ULL, 0x7fffffff00000002ULL}},
      {{UINT64_MAX, 0x8000000000000000ULL}, {0, 0x7fffffffffffffffULL}},
      {{0, 0x7fffffffffffffffULL}, {UINT64_MAX, 0x8000000000000000ULL}},
      {{0x8877665544332211ULL, 0xfedcba9876543210ULL},
       {0x8877665544332211ULL, 0xfedcba9876543210ULL}},
  };
  for (unsigned Index = 0; Index <= unsigned(AArch64Register::X30); ++Index)
    llvm::cantFail(
        CPU->setReg(AArch64Register(Index), 0xabcdef0000000000ULL + Index));
  for (auto Register :
       {AArch64Register::TPIDR_EL0, AArch64Register::TPIDRRO_EL0,
        AArch64Register::TPIDR_EL1})
    llvm::cantFail(
        CPU->setReg(Register, 0x1234567800000000ULL + unsigned(Register)));
  llvm::cantFail(CPU->setReg(AArch64Register::FPCR,
                             DefaultNaN | FlushToZero | (1u << RoundingShift)));
  llvm::cantFail(CPU->setReg(AArch64Register::FPSR, InitialFPSR));
  std::array<uint8_t, 64> StackBefore;
  StackBefore.fill(0xa5);
  llvm::cantFail(CPU->write(Stack, StackBefore));

  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    for (const auto &Input : Inputs) {
      for (unsigned Index = 0; Index <= LastVector; ++Index)
        vector(Index, {InitialVectorLow + Index, InitialVectorHigh - Index});
      vector(1, Input.Left);
      vector(2, Input.Right);
      auto ExpectedVectors = vectors();
      auto ExpectedScalars = scalars();
      ExpectedScalars[unsigned(AArch64Register::PC)] = Code + sizeof(uint32_t);
      const auto Left = ExpectedVectors[Case.Left];
      const auto Right = ExpectedVectors[Case.Right];
      const uint64_t Mask =
          Case.Bits == 64 ? UINT64_MAX : (uint64_t(1) << Case.Bits) - 1;
      RegisterValue Expected{};
      for (unsigned Element = 0; Element < Case.Elements; ++Element) {
        const unsigned Word = Element * Case.Bits / 64;
        const unsigned Shift = Element * Case.Bits % 64;
        const uint64_t A = (Left[Word] >> Shift) & Mask;
        const uint64_t B = (Right[Word] >> Shift) & Mask;
        if (A > B)
          Expected[Word] |= Mask << Shift;
      }
      ExpectedVectors[Case.Destination] = Expected;
      ASSERT_NO_FATAL_FAILURE(expectSuccess(run(Case.Word)));
      EXPECT_EQ(vectors(), ExpectedVectors);
      EXPECT_EQ(scalars(), ExpectedScalars);
      std::array<uint8_t, 64> StackAfter;
      llvm::cantFail(CPU->snapshotBacking(Stack, StackAfter));
      EXPECT_EQ(StackAfter, StackBefore);
    }
  }
}
TEST_P(AArch64SIMD, CMHIReservedSingleLaneVectorIsRejected) {
  expectRejected(0x2ee2343f);
}
TEST_P(AArch64SIMD, CMHIReservedScalarByteIsRejected) {
  expectRejected(0x7e22343f);
}
TEST_P(AArch64SIMD, CMHIReservedScalarHalfIsRejected) {
  expectRejected(0x7e62343f);
}
TEST_P(AArch64SIMD, CMHIReservedScalarWordIsRejected) {
  expectRejected(0x7ea2343f);
}
TEST_P(AArch64SIMD, ScalarFloatingResultsRoundingAndStatusMatchIEEEBits) {
  for (const auto &Case : FloatingCases) {
    SCOPED_TRACE(Case.Name);
    vector(FirstOperand, {Case.First, InitialVectorHigh});
    vector(SecondOperand, {Case.Second, InitialVectorHigh});
    vector(LastVector, {InitialVectorLow, InitialVectorHigh});
    llvm::cantFail(CPU->setReg(AArch64Register::FPCR, Case.Control));
    const auto Sticky = InitialFPSR & ~InputDenormal;
    llvm::cantFail(CPU->setReg(AArch64Register::FPSR, Sticky));
    ASSERT_NO_FATAL_FAILURE(expectSuccess(run(Case.Word)));
    EXPECT_EQ(vector(LastVector), (RegisterValue{Case.Result, 0}));
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::FPCR)), Case.Control);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::FPSR)),
              Sticky | Case.Status);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::NZCV)), InitialNZCV);
  }
}
TEST_P(AArch64SIMD, UnsupportedFPStateCannotBeInstalledOrTruncated) {
  auto Control = CPU->setReg(AArch64Register::FPCR, InvalidFPControl);
  ASSERT_TRUE(bool(Control));
  llvm::consumeError(std::move(Control));
  EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::FPCR)), 0u);
  auto Status = CPU->setReg(AArch64Register::FPSR, InvalidFPStatus);
  ASSERT_TRUE(bool(Status));
  llvm::consumeError(std::move(Status));
  EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::FPSR)), 0u);
  vector(FirstOperand, {DoubleOne, InitialVectorHigh});
  vector(SecondOperand, {DoubleTwo, InitialVectorHigh});
  ASSERT_NO_FATAL_FAILURE(expectSuccess(run(AddDouble)));
  EXPECT_EQ(vector(LastVector), (RegisterValue{DoubleThree, 0}));
}
TEST_P(AArch64SIMD, HalfPrecisionRequiresAnExplicitExtendedProfile) {
  expectRejected(HalfPrecisionAdd);
}
TEST_P(AArch64SIMD, ScalarIntegerConversionsPreserveBitsRoundingAndStatus) {
  struct Conversion {
    uint32_t Word;
    uint64_t Input, Output;
    uint32_t Control, Status;
  };
  // Independently assembled SCVTF/UCVTF s31,w0 and d31,x0. Results
  // explicitly distinguish signed input, W-register width and rounding.
  const Conversion Cases[] = {
      {0x1e22001f, UINT64_C(0xfffffffffffffffd), 0xc0400000, 0, 0},
      {0x1e23001f, UINT64_C(0xffffffff00000003), 0x40400000, 0, 0},
      {0x1e23001f, 0x1000001, 0x4b800000, 0, 0x10},
      {0x1e23001f, 0x1000001, 0x4b800001, 1u << RoundingShift, 0x10},
      {0x9e62001f, UINT64_C(0xfffffffffffffffd), UINT64_C(0xc008000000000000),
       0, 0},
      {0x9e63001f, UINT64_C(0x8000000000000000), UINT64_C(0x43e0000000000000),
       0, 0},
  };
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Word);
    llvm::cantFail(CPU->setReg(AArch64Register::X0, C.Input));
    llvm::cantFail(CPU->setReg(AArch64Register::FPCR, C.Control));
    llvm::cantFail(CPU->setReg(AArch64Register::FPSR, 2));
    vector(LastVector, {InitialVectorLow, InitialVectorHigh});
    ASSERT_NO_FATAL_FAILURE(expectSuccess(run(C.Word)));
    EXPECT_EQ(vector(LastVector), (RegisterValue{C.Output, 0}));
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::X0)), C.Input);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::FPCR)), C.Control);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::FPSR)), C.Status | 2);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::NZCV)), InitialNZCV);
  }
}
TEST_P(AArch64SIMD, ScalarFloatConversionsSaturateAndKeepSourceState) {
  struct Conversion {
    uint32_t Word;
    uint64_t Input, Output;
    uint32_t Status;
  };
  // FCVTZS/FCVTZU w0,s29 and x0,d29 use round-toward-zero, even when
  // FPCR requests round-up. W results must clear the upper word.
  const Conversion Cases[] = {
      {0x1e3803a0, 0xc0700000, 0xfffffffd, 0x10},
      {0x1e3903a0, 0x40700000, 3, 0x10},
      {0x1e3903a0, 0xbf800000, 0, 1},
      {0x1e3903a0, 0x7fc00000, 0, 1},
      {0x1e3903a0, 0x7f800000, 0xffffffff, 1},
      {0x9e7803a0, UINT64_C(0xc3e0000000000000), UINT64_C(0x8000000000000000),
       0},
      {0x9e7903a0, UINT64_C(0x7ff0000000000000), UINT64_MAX, 1},
      {0x9e7803a0, UINT64_C(0x7ff8000000000000), 0, 1},
  };
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Word);
    const RegisterValue Input{C.Input, InitialVectorHigh};
    vector(29, Input);
    llvm::cantFail(CPU->setReg(AArch64Register::X0, UINT64_MAX));
    llvm::cantFail(CPU->setReg(AArch64Register::FPCR, 1u << RoundingShift));
    llvm::cantFail(CPU->setReg(AArch64Register::FPSR, 2));
    ASSERT_NO_FATAL_FAILURE(expectSuccess(run(C.Word)));
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::X0)), C.Output);
    EXPECT_EQ(vector(29), Input);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::FPSR)), C.Status | 2);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::NZCV)), InitialNZCV);
  }
}
TEST_P(AArch64SIMD, ScalarFloatResizeRetainsRoundingAndClearsUpperLanes) {
  struct Conversion {
    uint32_t Word;
    uint64_t Input, Output;
    uint32_t Control, Status;
  };
  // FCVT d31,s29 and s31,d29.
  const Conversion Cases[] = {
      {0x1e22c3bf, 0x3fc00000, UINT64_C(0x3ff8000000000000), 0, 0},
      {0x1e6243bf, UINT64_C(0x3ff0000010000000), 0x3f800000, 0, 0x10},
      {0x1e6243bf, UINT64_C(0x3ff0000010000000), 0x3f800001,
       1u << RoundingShift, 0x10},
  };
  for (const auto &C : Cases) {
    const RegisterValue Input{C.Input, InitialVectorHigh};
    vector(29, Input);
    vector(LastVector, {InitialVectorLow, InitialVectorHigh});
    llvm::cantFail(CPU->setReg(AArch64Register::FPCR, C.Control));
    llvm::cantFail(CPU->setReg(AArch64Register::FPSR, 2));
    ASSERT_NO_FATAL_FAILURE(expectSuccess(run(C.Word)));
    EXPECT_EQ(vector(LastVector), (RegisterValue{C.Output, 0}));
    EXPECT_EQ(vector(29), Input);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::FPSR)), C.Status | 2);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::NZCV)), InitialNZCV);
  }
}
TEST_P(AArch64SIMD, HalfPrecisionConversionsRemainRejected) {
  expectRejected(0x1ee3001f);
}
TEST_P(AArch64SIMD, FixedPointConversionsRemainRejected) {
  expectRejected(0x1e03fc1f);
}
TEST_P(AArch64SIMD, PackedConversionsRemainRejected) {
  expectRejected(0x6e21dbbf);
}
TEST_P(AArch64SIMD, ScalableVectorsCannotEnterTheFixedWidthProfile) {
  expectRejected(ScalableVectorAdd);
}
TEST_P(AArch64SIMD, UnsupportedFPControlWriteCannotChangePublicState) {
  llvm::cantFail(CPU->setReg(AArch64Register::X0, InvalidFPControl));
  expectRejected(WriteFPCR);
  EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::FPCR)), 0u);
}
TEST_P(AArch64SIMD, ComparePublishesArchitecturalFlagsWithoutChangingVectors) {
  vector(FirstOperand, {DoubleOne, InitialVectorHigh});
  vector(SecondOperand, {DoubleTwo, InitialVectorHigh});
  const auto Before = vector(LastVector);
  ASSERT_NO_FATAL_FAILURE(expectSuccess(run(CompareDouble)));
  EXPECT_EQ(vector(LastVector), Before);
  EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::NZCV)), LessNZCV);
}
TEST_P(AArch64SIMD, VectorFloatingAddPreservesIndependentDoubleLanes) {
  vector(FirstOperand, {DoubleOne, DoubleTwo});
  vector(SecondOperand, {DoubleTwo, DoubleOne});
  ASSERT_NO_FATAL_FAILURE(expectSuccess(run(VectorFloatingAdd)));
  EXPECT_EQ(vector(LastVector), (RegisterValue{DoubleThree, DoubleThree}));
}
TEST_P(AArch64SIMD,
       RestoredContextReinstallsVectorRoundingAndCumulativeStatus) {
  vector(FirstOperand, {DoubleOne, InitialVectorHigh});
  vector(SecondOperand, {DoubleHalfULP, InitialVectorHigh});
  llvm::cantFail(CPU->setReg(AArch64Register::FPCR, 1u << RoundingShift));
  llvm::cantFail(CPU->setReg(AArch64Register::FPSR, InitialFPSR));
  auto Context = llvm::cantFail(CPU->saveContext());
  ASSERT_NO_FATAL_FAILURE(expectSuccess(run(AddDouble)));
  EXPECT_EQ(vector(LastVector), (RegisterValue{DoubleOne + 1, 0}));
  llvm::cantFail(CPU->setReg(AArch64Register::FPCR, 0));
  llvm::cantFail(CPU->setReg(AArch64Register::FPSR, 0));
  vector(SecondOperand, {DoubleOne, 0});
  ASSERT_NO_FATAL_FAILURE(expectSuccess(run(AddDouble)));
  EXPECT_EQ(vector(LastVector), (RegisterValue{DoubleTwo, 0}));
  llvm::cantFail(CPU->restoreContext(*Context));
  ASSERT_NO_FATAL_FAILURE(expectSuccess(run(AddDouble)));
  EXPECT_EQ(vector(LastVector), (RegisterValue{DoubleOne + 1, 0}));
  EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::FPCR)),
            1u << RoundingShift);
  EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::FPSR)),
            InitialFPSR | Inexact);
}
TEST_P(AArch64SIMD, PairVectorAddressWrapCannotAliasLowerPages) {
  expectWrappedPairRejected(PairVectorStore, ByteCount);
}
TEST_P(AArch64SIMD, PairIntegerAddressWrapCannotAliasLowerPages) {
  expectWrappedPairRejected(PairIntegerStore, WordBytes);
}
TEST_P(AArch64SIMD, FPControlInstructionsUseTheSamePublicState) {
  const auto Rounding = 1u << RoundingShift;
  llvm::cantFail(CPU->setReg(AArch64Register::X0, Rounding));
  ASSERT_NO_FATAL_FAILURE(expectSuccess(run(WriteFPCR)));
  EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::FPCR)), Rounding);
  llvm::cantFail(CPU->setReg(AArch64Register::X0, 0));
  ASSERT_NO_FATAL_FAILURE(expectSuccess(run(ReadFPCR)));
  EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::X0)), Rounding);
  llvm::cantFail(CPU->setReg(AArch64Register::X0, InitialFPSR));
  ASSERT_NO_FATAL_FAILURE(expectSuccess(run(WriteFPSR)));
  EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::FPSR)), InitialFPSR);
  llvm::cantFail(CPU->setReg(AArch64Register::X0, 0));
  ASSERT_NO_FATAL_FAILURE(expectSuccess(run(ReadFPSR)));
  EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::X0)), InitialFPSR);
}
INSTANTIATE_TEST_SUITE_P(
    Transports, AArch64SIMD,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF),
                     testing::Bool()));
} // namespace
} // namespace neverd::emulation
