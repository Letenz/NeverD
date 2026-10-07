//===- DirectX64Tests.cpp - Bounded x64 execution and fetch watches -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionSession.h"

namespace neverd::emulation {
namespace {
constexpr uint64_t Code = 0x10000, Data = 0x20000, PageSize = 0x1000;
constexpr uint64_t Value = 0x123456789abcdef0;

class DirectX64 : public testing::TestWithParam<ExecutionBackendKind> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  std::shared_ptr<ExecutionBudget> Budget;
  void SetUp() override {
    auto Backend =
        createExecutionBackend(GetParam(), ExecutionContract::DirectUserX64,
                               0x100000, GuestArchitecture::X64);
    if (!Backend) {
      auto E = Backend.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      auto Reason = llvm::toString(std::move(E));
      if (Unavailable)
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    CPU = std::move(Backend->CPU);
    llvm::cantFail(
        CPU->map(Code, PageSize * 2, Read | Write | Execute | UserAccessible));
    llvm::cantFail(CPU->map(Data, PageSize, Read | Write | UserAccessible));
    llvm::cantFail(CPU->writeRegister(CPURegister::X64CX, {Data, 0}));
  }
  std::unique_ptr<ExecutionSession> start(uint64_t Timeout = 1000000) {
    Budget = llvm::cantFail(ExecutionBudget::create({1000000, 1000, Timeout}));
    return llvm::cantFail(ExecutionSession::create(std::move(CPU), Budget));
  }
  uint64_t reg(ExecutionSession &S, CPURegister R) {
    return llvm::cantFail(S.cpu().readRegister(R))[0];
  }
};

TEST_P(DirectX64, SplitInstructionRetiresBeforeTheWatchedPageBoundary) {
  // movabs rax, Value straddles the watch; mov [rcx], rax begins inside it.
  constexpr uint8_t Bytes[] = {0x48, 0xb8, 0xf0, 0xde, 0xbc, 0x9a, 0x78,
                               0x56, 0x34, 0x12, 0x48, 0x89, 0x01, 0x90};
  const uint64_t Start = Code + PageSize - 5, Store = Start + 10;
  llvm::cantFail(CPU->write(Start, Bytes));
  auto S = start();
  llvm::cantFail(S->watchExecution({{Code + PageSize, PageSize}}));
  auto Exit = llvm::cantFail(S->run(Start, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::ExecutionWatch)
      << (Exit.CPU ? Exit.CPU->Diagnostic : "no CPU entry")
      << " PC=" << reg(*S, CPURegister::X64PC) << " fault="
      << (Exit.CPU && Exit.CPU->Fault ? Exit.CPU->Fault->Address.value_or(0)
                                      : 0)
      << " vector="
      << (Exit.CPU && Exit.CPU->Fault ? Exit.CPU->Fault->Interrupt.value_or(0)
                                      : 0);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Store);
  EXPECT_EQ(reg(*S, CPURegister::X64AX), Value);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, 8)), 0u);
  Exit = llvm::cantFail(S->run(Store, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Store + 3);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, 8)), Value);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, ReadingAWatchedExecutablePageDoesNotTriggerAFetchWatch) {
  constexpr uint8_t Bytes[] = {0x48, 0x8b, 0x01, 0xeb, 0xfe};
  const uint64_t Address = Code + PageSize + 0x80;
  llvm::cantFail(CPU->write(Code, Bytes));
  llvm::cantFail(CPU->writeInteger(Address, Value, 8));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64CX, {Address, 0}));
  auto S = start(50000);
  llvm::cantFail(S->watchExecution({{Code + PageSize, PageSize}}));
  auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::Timeout);
  ASSERT_TRUE(Exit.CPU);
  EXPECT_EQ(Exit.CPU->Kind, ExecutionExitKind::Deadline);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 3);
  EXPECT_EQ(reg(*S, CPURegister::X64AX), Value);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, PartialPageWatchStopsOnlyAtTheSelectedInstruction) {
  constexpr uint8_t Bytes[] = {0x48, 0xff, 0xc0, 0x48, 0xff, 0xc0, 0xeb, 0xf8};
  llvm::cantFail(CPU->write(Code, Bytes));
  auto S = start();
  llvm::cantFail(S->watchExecution({{Code + 3, 1}}));
  ASSERT_EQ(llvm::cantFail(S->run(Code, 1)).Kind,
            SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 3);
  EXPECT_EQ(reg(*S, CPURegister::X64AX), 1u);
  ASSERT_EQ(llvm::cantFail(S->run(Code + 3, 1)).Kind,
            SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 3);
  EXPECT_EQ(reg(*S, CPURegister::X64AX), 3u);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, ResumedWatchCanRetireOneInstructionAcrossTwoWatchedPages) {
  constexpr uint8_t Bytes[] = {0x48, 0xb8, 0xf0, 0xde, 0xbc, 0x9a,
                               0x78, 0x56, 0x34, 0x12, 0x90};
  const uint64_t Start = Code + PageSize - 5;
  llvm::cantFail(CPU->write(Start, Bytes));
  auto S = start();
  llvm::cantFail(S->watchExecution({{Code, PageSize * 2}}));
  ASSERT_EQ(llvm::cantFail(S->run(Start, 1)).Kind,
            SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Start);
  EXPECT_EQ(reg(*S, CPURegister::X64AX), 0u);
  ASSERT_EQ(llvm::cantFail(S->run(Start, 1)).Kind,
            SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Start + 10);
  EXPECT_EQ(reg(*S, CPURegister::X64AX), Value);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, WatchEndingAtTheLastAddressCanResumeWithoutWrapping) {
  constexpr uint8_t Bytes[] = {0x48, 0xff, 0xc0, 0x48, 0xff, 0xc0, 0xeb, 0xf8};
  llvm::cantFail(CPU->write(Code, Bytes));
  auto S = start();
  llvm::cantFail(S->watchExecution({{Code + 3, UINT64_MAX - (Code + 3) + 1}}));
  ASSERT_EQ(llvm::cantFail(S->run(Code, 1)).Kind,
            SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64AX), 1u);
  ASSERT_EQ(llvm::cantFail(S->run(Code + 3, 1)).Kind,
            SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 6);
  EXPECT_EQ(reg(*S, CPURegister::X64AX), 2u);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, ServiceRequestPreservesTheOriginalPCAndRegisters) {
  constexpr uint8_t Bytes[] = {0x0f, 0x05, 0x90};
  llvm::cantFail(CPU->write(Code, Bytes));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64AX, {Value, 0}));
  auto S = start();
  auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_TRUE(Exit.CPU);
  ASSERT_EQ(Exit.CPU->Kind, ExecutionExitKind::ServiceRequest)
      << Exit.CPU->Diagnostic;
  auto Request = llvm::cantFail(S->takeServiceRequest());
  EXPECT_EQ(Request.PC, Code);
  EXPECT_EQ(Request.NextPC, Code + 2);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code);
  EXPECT_EQ(reg(*S, CPURegister::X64AX), Value);
  EXPECT_EQ(reg(*S, CPURegister::X64CX), Data);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, InvalidOpcodeIsAFaultAtItsOriginalPC) {
  constexpr uint8_t Bytes[] = {0x0f, 0x0b};
  llvm::cantFail(CPU->write(Code, Bytes));
  auto S = start();
  auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_TRUE(Exit.CPU);
  EXPECT_EQ(Exit.CPU->Kind, ExecutionExitKind::GuestTrap)
      << Exit.CPU->Diagnostic;
  ASSERT_TRUE(Exit.CPU->Fault);
  EXPECT_EQ(Exit.CPU->Fault->Interrupt, 6u);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, DeadlinePublishesCommittedRegistersAtAnInstructionBoundary) {
  // inc rax; jmp back. No checked instruction credits are charged.
  constexpr uint8_t Bytes[] = {0x48, 0xff, 0xc0, 0xeb, 0xfb};
  llvm::cantFail(CPU->write(Code, Bytes));
  auto S = start(50000);
  auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::Timeout);
  ASSERT_TRUE(Exit.CPU);
  EXPECT_EQ(Exit.CPU->Kind, ExecutionExitKind::Deadline);
  EXPECT_GT(reg(*S, CPURegister::X64AX), 0u);
  const auto PC = reg(*S, CPURegister::X64PC);
  EXPECT_TRUE(PC == Code || PC == Code + 3);
  EXPECT_EQ(Budget->instructions(), 0u);
}

INSTANTIATE_TEST_SUITE_P(Transports, DirectX64,
                         testing::Values(ExecutionBackendKind::Unicorn,
                                         ExecutionBackendKind::KVM,
                                         ExecutionBackendKind::WHP),
                         [](const auto &P) {
                           return executionBackendName(P.param);
                         });
} // namespace
} // namespace neverd::emulation
