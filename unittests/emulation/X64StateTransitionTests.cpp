//===- X64StateTransitionTests.cpp - Native state across entry boundaries -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "arch/x86_64/X64ExceptionMonitor.h"
#include "backends/MachineFactories.h"
#include "core/MemoryProjection.h"
#include "gtest/gtest.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Endian.h"

namespace neverd::emulation {
namespace {
#define NEVERD_X64_TRANSITION_VALUE(Name, Value)                               \
  constexpr uint64_t Name = Value;
#define NEVERD_X64_TRANSITION_CODE(Name, ...)                                  \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64StateTransitionCases.def"
#undef NEVERD_X64_TRANSITION_CODE
#undef NEVERD_X64_TRANSITION_VALUE
constexpr uint64_t Levels[] = {
#define NEVERD_X64_TRANSITION_LEVEL(Value) Value,
#include "X64StateTransitionCases.def"
#undef NEVERD_X64_TRANSITION_LEVEL
};

class X64StateTransition : public testing::TestWithParam<ExecutionBackendKind> {
protected:
  std::unique_ptr<MemoryProjection> Memory;
  std::unique_ptr<X64Machine> Machine;
  X64MachineState State;
  uint64_t Root = 0;

  void SetUp() override {
    Memory = llvm::cantFail(MemoryProjection::create(Limit));
    auto Created = GetParam() == ExecutionBackendKind::KVM
                       ? createKvmMachine(*Memory)
                       : createWhpMachine(*Memory);
    if (!Created) {
      auto E = Created.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Reason = llvm::toString(std::move(E));
      if (Unavailable)
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    Machine = std::move(*Created);
    llvm::cantFail(
        Memory->map(Code, PageSize, Read | Write | Execute | UserAccessible));
    llvm::cantFail(Memory->map(Data, PageSize, Read | Write | UserAccessible));
    llvm::cantFail(Memory->write(Code, LoadTLS));
    llvm::cantFail(Memory->write(Code + FaultOffset, Divide));
    uint8_t Bytes[2 * WordBytes];
    llvm::support::endian::write64le(Bytes, First);
    llvm::support::endian::write64le(Bytes + WordBytes, Second);
    llvm::cantFail(Memory->write(Data, Bytes));
    State.reg(X64Register::PC) = Code;
    State.reg(X64Register::SP) = Data + PageSize - WordBytes;
    State.reg(X64Register::FLAGS) = InitialFlags;
  }

  llvm::Error step(const std::atomic<bool> *Stop = nullptr) {
    if (auto E = Memory->beginRun())
      return E;
    auto Release = llvm::scope_exit([&] { Memory->endRun(); });
    Root = llvm::cantFail(buildX64PageTables(
        *Memory, Root, State.UserMode, Machine->requiresExceptionMonitor()));
    return Machine->step(
        State, Root,
        {std::chrono::steady_clock::now() + std::chrono::microseconds(Timeout),
         Stop});
  }

  void expectUnchanged(const X64MachineState &Before) {
    EXPECT_EQ(State.Registers, Before.Registers);
    EXPECT_EQ(State.FSBase, Before.FSBase);
    EXPECT_EQ(State.GSBase, Before.GSBase);
    EXPECT_EQ(State.UserMode, Before.UserMode);
    EXPECT_EQ(State.Xmm, Before.Xmm);
    EXPECT_EQ(State.MXCSR, Before.MXCSR);
  }

  void expectDivideFault() {
    State.reg(X64Register::PC) = Code + FaultOffset;
    State.reg(X64Register::BX) = State.reg(X64Register::DX) = 0;
    const auto Before = State;
    bool Caught = false;
    auto Remaining =
        llvm::handleErrors(step(), [&](const X64ExceptionError &E) {
          Caught = true;
          EXPECT_EQ(E.exception().Vector,
                    unsigned(x64::ExceptionVector::Divide));
          EXPECT_EQ(E.exception().ErrorCode, std::nullopt);
        });
    EXPECT_EQ(llvm::toString(std::move(Remaining)), "");
    ASSERT_TRUE(Caught);
    expectUnchanged(Before);
  }
};

TEST_P(X64StateTransition, RefreshesTLSAndPrivilegeAcrossSuccessAndExceptions) {
  for (unsigned Round = 0; Round < Rounds; ++Round) {
    SCOPED_TRACE(Round);
    for (bool UserMode : {false, true}) {
      SCOPED_TRACE(UserMode);
      State.UserMode = UserMode;
      const bool Swap = Round % 2;
      State.FSBase = Data + (Swap ? WordBytes : 0);
      State.GSBase = Data + (Swap ? 0 : WordBytes);
      State.reg(X64Register::PC) = Code;
      State.reg(X64Register::CX) = 0;
      ASSERT_EQ(llvm::toString(step()), "");
      EXPECT_EQ(State.reg(X64Register::AX), Swap ? Second : First);
      EXPECT_EQ(State.reg(X64Register::PC), Code + LoadGSOffset);
      ASSERT_EQ(llvm::toString(step()), "");
      EXPECT_EQ(State.reg(X64Register::DX), Swap ? First : Second);
      expectDivideFault();
    }
  }
}

TEST_P(X64StateTransition, RefreshesCR8AcrossSuccessAndExceptions) {
  llvm::cantFail(Memory->write(Code, ReadCR8));
  for (unsigned Round = 0; Round < Rounds; ++Round) {
    SCOPED_TRACE(Round);
    for (auto Level : Levels) {
      SCOPED_TRACE(Level);
      State.reg(X64Register::CR8) = Level;
      State.reg(X64Register::PC) = Code;
      ASSERT_EQ(llvm::toString(step()), "");
      EXPECT_EQ(State.reg(X64Register::AX), Level);
      expectDivideFault();
    }
  }
}

TEST_P(X64StateTransition,
       CancelledEntryPreservesStateAndAllowsNewTLSProjection) {
  for (unsigned Round = 0; Round < Rounds; ++Round) {
    SCOPED_TRACE(Round);
    State.UserMode = Round % 2;
    State.FSBase = Data;
    State.GSBase = Data + WordBytes;
    State.reg(X64Register::PC) = Code;
    State.reg(X64Register::CX) = 0;
    ASSERT_EQ(llvm::toString(step()), "");
    const auto Before = State;
    const std::atomic<bool> Stop{true};
    auto E = step(&Stop);
    ASSERT_TRUE(bool(E));
    EXPECT_FALSE(E.isA<X64ExceptionError>());
    llvm::consumeError(std::move(E));
    expectUnchanged(Before);
    State.GSBase = Data;
    ASSERT_EQ(llvm::toString(step()), "");
    EXPECT_EQ(State.reg(X64Register::DX), First);
  }
}

INSTANTIATE_TEST_SUITE_P(NativeTransports, X64StateTransition,
                         testing::Values(ExecutionBackendKind::KVM,
                                         ExecutionBackendKind::WHP));
} // namespace
} // namespace neverd::emulation
