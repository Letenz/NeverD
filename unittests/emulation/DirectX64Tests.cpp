//===- DirectX64Tests.cpp - Bounded x64 execution and fetch watches -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/AddressSpace.h"
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

TEST_P(DirectX64, CommittedWriteStopsBeforeTheFollowingInstruction) {
  constexpr uint8_t Bytes[] = {0x48, 0x89, 0x01, 0x48, 0xff, 0x01, 0x0f, 0x05};
  llvm::cantFail(CPU->write(Code, Bytes));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64AX, {Value, 0}));
  auto S = start();
  llvm::cantFail(S->watchMemoryWrites({{Data, 8}}));
  auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::MemoryWriteWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 3);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, 8)), Value);
  Exit = llvm::cantFail(S->run(Code + 3, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::MemoryWriteWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 6);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, 8)), Value + 1);
  llvm::cantFail(S->watchMemoryWrites({}));
  Exit = llvm::cantFail(S->run(Code + 6, 1));
  ASSERT_TRUE(Exit.CPU);
  EXPECT_EQ(Exit.CPU->Kind, ExecutionExitKind::ServiceRequest);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, ReadingWriteWatchedRAMDoesNotPublishAWrite) {
  constexpr uint8_t Bytes[] = {0x48, 0x8b, 0x01, 0x0f, 0x05};
  llvm::cantFail(CPU->write(Code, Bytes));
  llvm::cantFail(CPU->writeInteger(Data, Value, 8));
  auto S = start();
  llvm::cantFail(S->watchMemoryWrites({{Data, 8}}));
  auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_TRUE(Exit.CPU);
  EXPECT_EQ(Exit.CPU->Kind, ExecutionExitKind::ServiceRequest);
  EXPECT_EQ(reg(*S, CPURegister::X64AX), Value);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, ResumedSelfBranchReachesTheSameWatchAgain) {
  constexpr uint8_t Bytes[] = {0xeb, 0xfe};
  llvm::cantFail(CPU->write(Code, Bytes));
  auto S = start();
  llvm::cantFail(S->watchExecution({{Code, 2}}));
  ASSERT_EQ(llvm::cantFail(S->run(Code, 1)).Kind,
            SessionExitKind::ExecutionWatch);
  ASSERT_EQ(llvm::cantFail(S->run(Code, 1)).Kind,
            SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, PartialWatchCanRunALongLoopInTheSamePage) {
  // mov ecx, 10000000; dec ecx; jnz -4; jmp to the watched instruction.
  constexpr uint8_t Bytes[] = {0xb9, 0x80, 0x96, 0x98, 0x00, 0xff,
                               0xc9, 0x75, 0xfc, 0xeb, 0x75};
  llvm::cantFail(CPU->write(Code, Bytes));
  llvm::cantFail(CPU->writeInteger(Code + 128, 0x90, 1));
  auto S = start(5000000);
  llvm::cantFail(S->watchExecution({{Code + 128, 1}}));
  const auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 128);
  EXPECT_EQ(reg(*S, CPURegister::X64CX), 0u);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, FragmentedWatchesAllowRepeatedPageCrossings) {
  constexpr uint8_t Start[] = {0xb9, 0xe8, 0x03, 0x00, 0x00,
                               0xe9, 0xf6, 0x0f, 0x00, 0x00};
  constexpr uint8_t Loop[] = {0xff, 0xc9, 0x0f, 0x85, 0xfd, 0xef, 0xff,
                              0xff, 0xe9, 0x73, 0xf0, 0xff, 0xff};
  llvm::cantFail(CPU->write(Code, Start));
  llvm::cantFail(CPU->write(Code + PageSize, Loop));
  llvm::cantFail(CPU->writeInteger(Code + 128, 0x90, 1));
  std::vector<ExecutionWatch> Watches{{Code + 128, 1}};
  for (uint64_t I = 0; I < 100000; ++I)
    Watches.push_back({0x1000000 + I * 32, 1});
  auto S = start(10000000);
  llvm::cantFail(S->watchExecution(Watches));
  const auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 128);
  EXPECT_EQ(reg(*S, CPURegister::X64CX), 0u);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, WatchedCrossPageInstructionStopsBeforeItsEffects) {
  constexpr uint8_t Jump[] = {0xe9, 0xf9, 0x0f, 0, 0};
  constexpr uint8_t Tail[] = {0x48, 0xff, 0xc0, 0x0f, 0x0b};
  llvm::cantFail(CPU->write(Code, Jump));
  llvm::cantFail(CPU->write(Code + PageSize - 2, Tail));
  auto S = start();
  llvm::cantFail(S->watchExecution({{Code + PageSize - 2, 1}}));
  const auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + PageSize - 2);
  EXPECT_EQ(reg(*S, CPURegister::X64AX), 0u);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, FullNamespacePageCoverDoesNotWrap) {
  constexpr uint8_t Bytes[] = {0x48, 0xff, 0xc0, 0x90};
  llvm::cantFail(CPU->write(Code, Bytes));
  auto S = start();
  llvm::cantFail(S->watchExecution({{0, UINT64_MAX}}));
  ASSERT_EQ(llvm::cantFail(S->run(Code, 1)).Kind,
            SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64AX), 0u);
  ASSERT_EQ(llvm::cantFail(S->run(Code, 1)).Kind,
            SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 3);
  EXPECT_EQ(reg(*S, CPURegister::X64AX), 1u);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, AliasedCodeWriteInvalidatesAPlannedBranch) {
  // Change a forward jump from the service instruction to the watch, through
  // another mapping of the code's physical page. The private code guard must
  // replan before that branch without publishing a public memory-write stop.
  constexpr uint64_t Alias = 0x30000;
  constexpr uint8_t Bytes[] = {0xc6, 0x01, 0x7b, 0xeb, 0x00, 0x0f, 0x05};
  llvm::cantFail(CPU->write(Code, Bytes));
  llvm::cantFail(CPU->writeInteger(Code + 128, 0x90, 1));
  llvm::cantFail(
      CPU->mapAlias(Alias, Code, PageSize, Read | Write | UserAccessible));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64CX, {Alias + 4, 0}));
  auto S = start();
  llvm::cantFail(S->watchExecution({{Code + 128, 1}}));
  const auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 128);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Code + 4, 1)), 0x7bu);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, InactivePagePlansRevalidateWritesThroughPhysicalAliases) {
  constexpr uint64_t Alias = 0x30000;
  constexpr uint8_t Jump[] = {0xe9, 0xfb, 0x0f, 0, 0};
  constexpr uint8_t Watch[] = {0x48, 0xff, 0xc0, 0x0f, 0x0b};
  // Rewrite the first page's jump through an alias while the second page
  // executes. The old plan had no stop inside its page; the new one must
  // stop at the watched increment before its effects.
  constexpr uint8_t Rewrite[] = {0xc7, 0x05, 0xf7, 0xef, 0x01, 0,    0x7b, 0,
                                 0,    0,    0xe9, 0xf1, 0xef, 0xff, 0xff};
  llvm::cantFail(CPU->write(Code, Jump));
  llvm::cantFail(CPU->write(Code + 128, Watch));
  llvm::cantFail(CPU->write(Code + PageSize, Rewrite));
  llvm::cantFail(
      CPU->mapAlias(Alias, Code, PageSize, Read | Write | UserAccessible));
  auto S = start();
  llvm::cantFail(S->watchExecution({{Code + 128, 1}}));
  const auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 128);
  EXPECT_EQ(reg(*S, CPURegister::X64AX), 0u);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Code + 1, 4)), 0x7bu);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, UnwatchedPageCanReturnToAPartialPageWatch) {
  // A direct branch leaves the partially watched page; an indirect branch on
  // an unwatched page returns to the watch before the increment executes.
  constexpr uint8_t Branch[] = {0xe9, 0xfb, 0x0f, 0x00, 0x00};
  constexpr uint8_t Return[] = {0xff, 0xe1};
  constexpr uint8_t Increment[] = {0x48, 0xff, 0xc0};
  llvm::cantFail(CPU->write(Code, Branch));
  llvm::cantFail(CPU->write(Code + PageSize, Return));
  llvm::cantFail(CPU->write(Code + 128, Increment));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64CX, {Code + 128, 0}));
  auto S = start();
  llvm::cantFail(S->watchExecution({{Code + 128, 1}}));
  const auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 128);
  EXPECT_EQ(reg(*S, CPURegister::X64AX), 0u);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, GuestTrapFlagOutranksAPartialPageWatch) {
  // push 0x302; popfq; nop; inc rax. TF takes effect after the NOP.
  constexpr uint8_t Bytes[] = {0x68, 0x02, 0x03, 0x00, 0x00,
                               0x9d, 0x90, 0x48, 0xff, 0xc0};
  llvm::cantFail(CPU->write(Code, Bytes));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64SP, {Data + 0x800, 0}));
  auto S = start();
  llvm::cantFail(S->watchExecution({{Code + 128, 1}}));
  const auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::CPU);
  ASSERT_TRUE(Exit.CPU);
  ASSERT_EQ(Exit.CPU->Kind, ExecutionExitKind::GuestTrap);
  ASSERT_TRUE(Exit.CPU->Fault);
  EXPECT_EQ(Exit.CPU->Fault->Interrupt, 1u);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 7);
  EXPECT_EQ(reg(*S, CPURegister::X64AX), 0u);
  EXPECT_EQ(reg(*S, CPURegister::X64FLAGS) & 0x100, 0x100u);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, IndirectFrontierDoesNotLeakASingleStepFlag) {
  constexpr uint8_t Indirect[] = {0xff, 0xe0};          // jmp rax
  constexpr uint8_t Flags[] = {0x9c, 0x5a, 0xeb, 0x6c}; // pushfq; pop rdx; jmp
  llvm::cantFail(CPU->write(Code, Indirect));
  llvm::cantFail(CPU->write(Code + 16, Flags));
  llvm::cantFail(CPU->writeInteger(Code + 128, 0x90, 1));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64AX, {Code + 16, 0}));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64SP, {Data + 0x800, 0}));
  auto S = start();
  llvm::cantFail(S->watchExecution({{Code + 128, 1}}));
  const auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 128);
  EXPECT_EQ(reg(*S, CPURegister::X64DX) & 0x100, 0u);
  EXPECT_EQ(reg(*S, CPURegister::X64FLAGS) & 0x100, 0u);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, ManyControlFlowFrontiersPreservePushedFlags) {
  // Five untaken conditional branches lead to independent indirect branches.
  // A bounded execution-stop plan must retain PUSHFQ's original guest flags.
  constexpr uint8_t Bytes[] = {0x39, 0xc9, 0x9c, 0x5a, 0x75, 0x3a, 0x75, 0x40,
                               0x75, 0x46, 0x75, 0x4c, 0x75, 0x52, 0xeb, 0x70};
  constexpr uint8_t Indirect[] = {0xff, 0xe0};
  llvm::cantFail(CPU->write(Code, Bytes));
  for (uint64_t Offset : {64, 72, 80, 88, 96})
    llvm::cantFail(CPU->write(Code + Offset, Indirect));
  llvm::cantFail(CPU->writeInteger(Code + 128, 0x90, 1));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64AX, {Code + 128, 0}));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64SP, {Data + 0x800, 0}));
  auto S = start();
  llvm::cantFail(S->watchExecution({{Code + 128, 1}}));
  const auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 128);
  EXPECT_EQ(reg(*S, CPURegister::X64DX) & 0x100, 0u);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, ResumedPushFlagsWritesOnlyTheGuestFlags) {
  constexpr uint8_t Bytes[] = {0x9c, 0x5a, 0x0f, 0x05};
  llvm::cantFail(CPU->write(Code, Bytes));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64SP, {Data + 0x800, 0}));
  auto S = start();
  llvm::cantFail(S->watchExecution({{Code, 1}}));
  llvm::cantFail(S->watchMemoryWrites({{Data + 0x7f8, 8}}));
  ASSERT_EQ(llvm::cantFail(S->run(Code, 1)).Kind,
            SessionExitKind::ExecutionWatch);
  ASSERT_EQ(llvm::cantFail(S->run(Code, 1)).Kind,
            SessionExitKind::MemoryWriteWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 1);
  EXPECT_EQ(reg(*S, CPURegister::X64SP), Data + 0x7f8);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data + 0x7f8, 8)), 0x202u);
  llvm::cantFail(S->watchMemoryWrites({}));
  const auto Exit = llvm::cantFail(S->run(Code + 1, 1));
  ASSERT_TRUE(Exit.CPU);
  EXPECT_EQ(Exit.CPU->Kind, ExecutionExitKind::ServiceRequest);
  EXPECT_EQ(reg(*S, CPURegister::X64DX), 0x202u);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, ResumedPopFlagsPreservesTheNextGuestDebugTrap) {
  constexpr uint8_t Bytes[] = {0x9d, 0x90, 0x48, 0xff, 0xc0};
  llvm::cantFail(CPU->write(Code, Bytes));
  llvm::cantFail(CPU->writeInteger(Data + 0x800, 0x302, 8));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64SP, {Data + 0x800, 0}));
  auto S = start();
  llvm::cantFail(S->watchExecution({{Code, 2}}));
  ASSERT_EQ(llvm::cantFail(S->run(Code, 1)).Kind,
            SessionExitKind::ExecutionWatch);
  ASSERT_EQ(llvm::cantFail(S->run(Code, 1)).Kind,
            SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 1);
  EXPECT_EQ(reg(*S, CPURegister::X64SP), Data + 0x808);
  EXPECT_EQ(reg(*S, CPURegister::X64FLAGS) & 0x100, 0x100u);
  const auto Exit = llvm::cantFail(S->run(Code + 1, 1));
  ASSERT_TRUE(Exit.CPU);
  ASSERT_EQ(Exit.CPU->Kind, ExecutionExitKind::GuestTrap);
  ASSERT_TRUE(Exit.CPU->Fault);
  EXPECT_EQ(Exit.CPU->Fault->Interrupt, 1u);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 2);
  EXPECT_EQ(reg(*S, CPURegister::X64AX), 0u);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, WriteInAResumedFetchWatchIsCommittedExactlyOnce) {
  constexpr uint8_t Bytes[] = {0x48, 0xff, 0x01, 0x0f, 0x05};
  llvm::cantFail(CPU->write(Code, Bytes));
  auto S = start();
  llvm::cantFail(S->watchMemoryWrites({{Data, 8}}));
  llvm::cantFail(S->watchExecution({{Code, 3}}));
  ASSERT_EQ(llvm::cantFail(S->run(Code, 1)).Kind,
            SessionExitKind::ExecutionWatch);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, 8)), 0u);
  auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::MemoryWriteWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code + 3);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, 8)), 1u);
  llvm::cantFail(S->watchExecution({}));
  Exit = llvm::cantFail(S->run(Code + 3, 1));
  ASSERT_TRUE(Exit.CPU);
  EXPECT_EQ(Exit.CPU->Kind, ExecutionExitKind::ServiceRequest);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, 8)), 1u);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, WriteWatchesFollowPhysicalAliasesAndMappingChanges) {
  constexpr uint8_t Bytes[] = {0x48, 0x89, 0x01, 0x0f, 0x05};
  constexpr uint64_t Alias = 0x30000, Other = Data + PageSize;
  llvm::cantFail(CPU->write(Code, Bytes));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64AX, {Value, 0}));
  llvm::cantFail(CPU->map(Other, PageSize, Read | Write | UserAccessible));
  llvm::cantFail(
      CPU->mapAlias(Alias, Data, PageSize, Read | Execute | UserAccessible));
  auto S = start();
  llvm::cantFail(S->watchMemoryWrites({{Alias + 3, 2}}));
  ASSERT_EQ(llvm::cantFail(S->run(Code, 1)).Kind,
            SessionExitKind::MemoryWriteWatch);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, 8)), Value);
  llvm::cantFail(S->cpu().replaceAliases(
      {{Alias, PageSize}},
      {{Alias, Other, PageSize, Read | Execute | UserAccessible}}));
  // Old backing no longer matches. A normal service boundary ends this run.
  llvm::cantFail(S->cpu().writeRegister(CPURegister::X64AX, {Value + 1, 0}));
  auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_TRUE(Exit.CPU);
  ASSERT_EQ(Exit.CPU->Kind, ExecutionExitKind::ServiceRequest);
  llvm::cantFail(S->takeServiceRequest());
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, 8)), Value + 1);
  llvm::cantFail(S->cpu().writeRegister(CPURegister::X64CX, {Other, 0}));
  ASSERT_EQ(llvm::cantFail(S->run(Code, 1)).Kind,
            SessionExitKind::MemoryWriteWatch);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Other, 8)), Value + 1);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(DirectX64, RealWriteProtectionFaultOutranksAWriteWatch) {
  constexpr uint8_t Bytes[] = {0x48, 0x89, 0x01};
  llvm::cantFail(CPU->write(Code, Bytes));
  llvm::cantFail(CPU->protect(Data, PageSize, Read | UserAccessible));
  auto S = start();
  llvm::cantFail(S->watchMemoryWrites({{Data, 8}}));
  auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::CPU);
  ASSERT_TRUE(Exit.CPU);
  ASSERT_EQ(Exit.CPU->Kind, ExecutionExitKind::GuestTrap);
  ASSERT_TRUE(Exit.CPU->Fault);
  EXPECT_EQ(Exit.CPU->Fault->PC, Code);
  EXPECT_EQ(Exit.CPU->Fault->Address, Data);
  EXPECT_EQ(Exit.CPU->Fault->Interrupt, 14u);
  EXPECT_EQ(llvm::cantFail(S->cpu().addressSpace()->readInteger(Data, 8)), 0u);
}

TEST_P(DirectX64, RepWriteWatchPreservesPartialProgressBeforeARealFault) {
  constexpr uint8_t Bytes[] = {0xf3, 0xaa, 0x0f, 0x05}; // rep stosb; syscall
  llvm::cantFail(CPU->write(Code, Bytes));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64AX, {0x7f, 0}));
  llvm::cantFail(CPU->writeRegister(CPURegister::X64CX, {2, 0}));
  llvm::cantFail(
      CPU->writeRegister(CPURegister::X64DI, {Data + PageSize - 1, 0}));
  auto S = start();
  llvm::cantFail(S->watchMemoryWrites({{Data, PageSize}}));
  auto Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::MemoryWriteWatch);
  EXPECT_EQ(reg(*S, CPURegister::X64PC), Code);
  EXPECT_EQ(reg(*S, CPURegister::X64CX), 1u);
  EXPECT_EQ(reg(*S, CPURegister::X64DI), Data + PageSize);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data + PageSize - 1, 1)),
            0x7fu);
  Exit = llvm::cantFail(S->run(Code, 1));
  ASSERT_EQ(Exit.Kind, SessionExitKind::CPU);
  ASSERT_TRUE(Exit.CPU);
  ASSERT_EQ(Exit.CPU->Kind, ExecutionExitKind::GuestTrap);
  ASSERT_TRUE(Exit.CPU->Fault);
  EXPECT_EQ(Exit.CPU->Fault->PC, Code);
  EXPECT_EQ(Exit.CPU->Fault->Address, Data + PageSize);
  EXPECT_EQ(Exit.CPU->Fault->Interrupt, 14u);
  EXPECT_EQ(reg(*S, CPURegister::X64CX), 1u);
  EXPECT_EQ(reg(*S, CPURegister::X64DI), Data + PageSize);
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
