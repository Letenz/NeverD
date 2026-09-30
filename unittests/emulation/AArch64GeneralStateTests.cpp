//===- AArch64GeneralStateTests.cpp - ARM64 state capture tests -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "arch/aarch64/AArch64GeneralState.h"
#include "core/ExecutionDiagnostics.h"
#include "gtest/gtest.h"

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
class AArch64GeneralState : public testing::TestWithParam<bool> {
protected:
  AArch64MachineState State;
  void SetUp() override {
    State.UserMode = GetParam();
    for (unsigned I = 0; I < State.Registers.size(); ++I)
      State.Registers[I] = InitialScalar + I;
    State.Vectors.fill({InitialVectorLow, InitialVectorHigh});
  }
  void expectPreserved(const AArch64MachineState &Before) {
    EXPECT_EQ(State.UserMode, Before.UserMode);
    EXPECT_EQ(State.Registers, Before.Registers);
    EXPECT_EQ(State.Vectors, Before.Vectors);
  }
  void expectCaptured(const AArch64MachineState &Before) {
    EXPECT_EQ(State.UserMode, Before.UserMode);
    EXPECT_EQ(State.Vectors, Before.Vectors);
    // Public register identities independently bound the admitted core state.
    // Registers after TPIDR_EL0 belong to other state and remain untouched.
    for (unsigned I = 0; I < State.Registers.size(); ++I) {
      const auto Register = AArch64Register(I);
      EXPECT_EQ(State.Registers[I],
                I > unsigned(AArch64Register::TPIDR_EL0) ? Before.Registers[I]
                : Register == AArch64Register::NZCV      ? CapturedNZCV
                                                         : captured(Register));
    }
  }
};
TEST_P(AArch64GeneralState,
       CompleteCaptureNormalizesFlagsAndPreservesOtherState) {
  const auto Before = State;
  unsigned Reads = 0;
  EXPECT_EQ(llvm::toString(captureAArch64GeneralState(
                State,
                [&](AArch64Register Register) -> llvm::Expected<uint64_t> {
                  ++Reads;
                  return captured(Register);
                })),
            "");
  EXPECT_EQ(Reads, unsigned(AArch64Register::TPIDR_EL0) + 1);
  expectCaptured(Before);
}
TEST_P(AArch64GeneralState, EveryFailedReadRetainsAllFieldsAndAllowsRetry) {
  const auto Before = State;
  for (unsigned I = 0; I <= unsigned(AArch64Register::TPIDR_EL0); ++I) {
    SCOPED_TRACE(I);
    State = Before;
    unsigned Reads = 0;
    auto E = captureAArch64GeneralState(
        State, [&](AArch64Register Register) -> llvm::Expected<uint64_t> {
          ++Reads;
          if (unsigned(Register) == I)
            return diagnostic::error(ReadFailure);
          return captured(Register);
        });
    EXPECT_EQ(llvm::toString(std::move(E)), ReadFailure);
    EXPECT_EQ(Reads, I + 1);
    expectPreserved(Before);
    EXPECT_EQ(llvm::toString(captureAArch64GeneralState(
                  State,
                  [](AArch64Register Register) -> llvm::Expected<uint64_t> {
                    return captured(Register);
                  })),
              "");
    expectCaptured(Before);
  }
}
TEST_P(AArch64GeneralState, MissingReaderCannotPublishState) {
  const auto Before = State;
  EXPECT_EQ(llvm::toString(captureAArch64GeneralState(State, {})),
            diagnostic::Register);
  expectPreserved(Before);
}
INSTANTIATE_TEST_SUITE_P(Privileges, AArch64GeneralState,
                         testing::Values(false, true));
} // namespace
} // namespace neverd::emulation
