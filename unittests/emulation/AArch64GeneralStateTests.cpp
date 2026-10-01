//===- AArch64GeneralStateTests.cpp - ARM64 state capture tests -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "arch/aarch64/AArch64GeneralState.h"
#include "core/ExecutionDiagnostics.h"
#include "gtest/gtest.h"

#include <tuple>

namespace neverd::emulation {
namespace {
#define NEVERD_AARCH64_STATE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_AARCH64_STATE_TEXT(Name, Value) constexpr char Name[] = Value;
#include "AArch64GeneralStateCases.def"
#undef NEVERD_AARCH64_STATE_TEXT
#undef NEVERD_AARCH64_STATE_VALUE

uint64_t captured(AArch64Register Register) {
  return Register == AArch64Register::NZCV
             ? RawPState
             : CapturedScalar + unsigned(Register);
}
class AArch64GeneralState
    : public testing::TestWithParam<std::tuple<bool, bool>> {
protected:
  AArch64MachineState State;
  void SetUp() override {
    State.UserMode = std::get<0>(GetParam());
    for (unsigned I = 0; I < State.Registers.size(); ++I)
      State.Registers[I] = InitialScalar + I;
    State.Vectors.fill({InitialVectorLow, InitialVectorHigh});
  }
  AArch64Register lastCapturedRegister() const {
    return std::get<1>(GetParam()) ? AArch64Register::FPSR
                                   : AArch64Register::TPIDR_EL0;
  }
  llvm::Error capture(AArch64RegisterReader Read) {
    return std::get<1>(GetParam()) ? captureAArch64ScalarState(State, Read)
                                   : captureAArch64GeneralState(State, Read);
  }
  void expectPreserved(const AArch64MachineState &Before) {
    EXPECT_EQ(State.UserMode, Before.UserMode);
    EXPECT_EQ(State.Registers, Before.Registers);
    EXPECT_EQ(State.Vectors, Before.Vectors);
  }
  void expectCaptured(const AArch64MachineState &Before) {
    EXPECT_EQ(State.UserMode, Before.UserMode);
    EXPECT_EQ(State.Vectors, Before.Vectors);
    // Public identities independently bound the two capture inventories.
    // Native capture preserves the extra software-only fields exactly.
    for (unsigned I = 0; I < State.Registers.size(); ++I) {
      const auto Register = AArch64Register(I);
      const auto Expected = I > unsigned(lastCapturedRegister())
                                ? Before.Registers[I]
                            : Register == AArch64Register::NZCV ? CapturedNZCV
                            : Register == AArch64Register::FPCR ||
                                    Register == AArch64Register::FPSR
                                ? captured(Register) & Scalar32Mask
                                : captured(Register);
      EXPECT_EQ(State.Registers[I], Expected);
    }
  }
};
TEST_P(AArch64GeneralState,
       CompleteCaptureNormalizesFlagsAndPreservesOtherState) {
  const auto Before = State;
  unsigned Reads = 0;
  EXPECT_EQ(llvm::toString(capture(
                [&](AArch64Register Register) -> llvm::Expected<uint64_t> {
                  ++Reads;
                  return captured(Register);
                })),
            "");
  EXPECT_EQ(Reads, unsigned(lastCapturedRegister()) + 1);
  expectCaptured(Before);
}
TEST_P(AArch64GeneralState, EveryFailedReadRetainsAllFieldsAndAllowsRetry) {
  const auto Before = State;
  for (unsigned I = 0; I <= unsigned(lastCapturedRegister()); ++I) {
    SCOPED_TRACE(I);
    State = Before;
    unsigned Reads = 0;
    auto E = capture([&](AArch64Register Register) -> llvm::Expected<uint64_t> {
      ++Reads;
      if (unsigned(Register) == I)
        return diagnostic::error(ReadFailure);
      return captured(Register);
    });
    EXPECT_EQ(llvm::toString(std::move(E)), ReadFailure);
    EXPECT_EQ(Reads, I + 1);
    expectPreserved(Before);
    EXPECT_EQ(llvm::toString(capture(
                  [](AArch64Register Register) -> llvm::Expected<uint64_t> {
                    return captured(Register);
                  })),
              "");
    expectCaptured(Before);
  }
}
TEST_P(AArch64GeneralState, MissingReaderCannotPublishState) {
  const auto Before = State;
  EXPECT_EQ(llvm::toString(capture({})), diagnostic::Register);
  expectPreserved(Before);
}
INSTANTIATE_TEST_SUITE_P(Privileges, AArch64GeneralState,
                         testing::Combine(testing::Bool(), testing::Bool()));
} // namespace
} // namespace neverd::emulation
