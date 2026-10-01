//===- AArch64SIMDTests.cpp - Independent checked ARM64 FP/SIMD results ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
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
      if (Unavailable)
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
    const auto Before = vector(LastVector);
    const auto Exit = run(Word);
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
        << Exit.Diagnostic;
    EXPECT_EQ(vector(LastVector), Before);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::NZCV)), InitialNZCV);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::PC)), Code);
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
                                     ExecutionBackendKind::WHP),
                     testing::Bool()));
} // namespace
} // namespace neverd::emulation
