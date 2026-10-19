//===- ExecutionSessionTests.cpp - Shared budgets and owned continuations -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionSession.h"

#include "llvm/Support/Endian.h"

#include <thread>

namespace neverd::emulation {
namespace {
#define NEVERD_USER_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_USER_X64(Name, ...) constexpr uint8_t Name[] = {__VA_ARGS__};
#define NEVERD_USER_ARM(Name, ...) constexpr uint32_t Name[] = {__VA_ARGS__};
#include "UserExecutionCases.def"
#undef NEVERD_USER_ARM
#undef NEVERD_USER_X64
#undef NEVERD_USER_VALUE
#define NEVERD_SERVICE_X64(Name, ...) constexpr uint8_t Name[] = {__VA_ARGS__};
#define NEVERD_SERVICE_ARM(Name, ...) constexpr uint32_t Name[] = {__VA_ARGS__};
#include "ServiceRequestCases.def"
#undef NEVERD_SERVICE_ARM
#undef NEVERD_SERVICE_X64
#define NEVERD_SESSION_TEST_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_SESSION_TEST_X64(Name, ...)                                     \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#define NEVERD_SESSION_TEST_ARM(Name, ...)                                     \
  constexpr uint32_t Name[] = {__VA_ARGS__};
#include "ExecutionSessionCases.def"
#undef NEVERD_SESSION_TEST_ARM
#undef NEVERD_SESSION_TEST_X64
#undef NEVERD_SESSION_TEST_VALUE

struct Profile {
  const char *Name;
  ExecutionBackendKind Backend;
  GuestArchitecture ISA;
  ExecutionContract Contract;
};
constexpr Profile Profiles[] = {
#define NEVERD_USER_PROFILE(Name, Backend, ISA, Contract)                      \
  {#Name, ExecutionBackendKind::Backend, GuestArchitecture::ISA,               \
   ExecutionContract::Contract},
#include "UserExecutionCases.def"
#undef NEVERD_USER_PROFILE
};
void PrintTo(const Profile &P, std::ostream *OS) { *OS << P.Name; }

class RuntimeSession : public testing::TestWithParam<Profile> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  std::shared_ptr<ExecutionBudget> Budget;
  CPURegister PC, Source, Result;
  void SetUp() override {
    auto B = createExecutionBackend(GetParam().Backend, GetParam().Contract,
                                    Limit, GetParam().ISA);
    if (!B) {
      auto E = B.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      auto Reason = llvm::toString(std::move(E));
      if (Unavailable && !requireHvf(GetParam().Backend, GetParam().ISA))
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    CPU = std::move(B->CPU);
    const bool X64 = GetParam().ISA == GuestArchitecture::X64;
    PC = X64 ? CPURegister::X64PC : CPURegister::AArch64PC;
    Source = X64 ? CPURegister::X64CX : CPURegister::AArch64X1;
    Result = X64 ? CPURegister::X64AX : CPURegister::AArch64X0;
    llvm::cantFail(
        CPU->map(Code, PageSize, Read | Write | Execute | UserAccessible));
    llvm::cantFail(CPU->map(Data, PageSize, Read | Write | UserAccessible));
    llvm::cantFail(CPU->writeRegister(Source, {Data, 0}));
  }
  void code(llvm::ArrayRef<uint8_t> X64, llvm::ArrayRef<uint32_t> ARM) {
    if (GetParam().ISA == GuestArchitecture::X64) {
      llvm::cantFail(CPU->write(Code, X64));
      return;
    }
    std::vector<uint8_t> Bytes(ARM.size() * sizeof(uint32_t));
    for (size_t I = 0; I < ARM.size(); ++I)
      llvm::support::endian::write32le(Bytes.data() + I * sizeof(uint32_t),
                                       ARM[I]);
    llvm::cantFail(CPU->write(Code, Bytes));
  }
  std::unique_ptr<ExecutionSession>
  start(std::function<bool(const BackendFault &)> Recovery = {}) {
    Budget = llvm::cantFail(
        ExecutionBudget::create({Instructions, Events, Timeout}));
    return llvm::cantFail(
        ExecutionSession::create(std::move(CPU), Budget, std::move(Recovery)));
  }
  uint64_t reg(ExecutionSession &S, CPURegister R) {
    return llvm::cantFail(S.cpu().readRegister(R))[0];
  }
};

TEST_P(RuntimeSession, ObserverSeesOnlyAdmittedAttemptsAcrossQuanta) {
  code(CounterX64, CounterARM);
  Budget =
      llvm::cantFail(ExecutionBudget::create({Instructions, Events, Timeout}));
  std::vector<uint64_t> Trace;
  auto Session = llvm::cantFail(ExecutionSession::create(
      std::move(CPU), Budget, {},
      [&](uint64_t Address, uint32_t) { Trace.push_back(Address); }));
  uint64_t Next = Code;
  for (;;) {
    auto Exit = llvm::cantFail(Session->run(Next, Quantum));
    EXPECT_EQ(Trace.size(), Budget->instructions());
    if (Exit.Kind == SessionExitKind::InstructionLimit)
      break;
    ASSERT_EQ(Exit.Kind, SessionExitKind::Quantum);
    Next = reg(*Session, PC);
  }
  EXPECT_EQ(Trace.size(), Instructions);
}

TEST_P(RuntimeSession, TwoCPUsShareRAMAndOneBudgetWithIndependentRegisters) {
  code(CounterX64, CounterARM);
  auto First = start();
  auto Other = llvm::cantFail(
      createExecutionBackend(GetParam().Backend, GetParam().Contract,
                             First->cpu().addressSpace(), GetParam().ISA));
  llvm::cantFail(Other.CPU->writeRegister(Source, {Data, 0}));
  auto Second =
      llvm::cantFail(ExecutionSession::create(std::move(Other.CPU), Budget));
  auto A = llvm::cantFail(First->run(Code, Quantum));
  EXPECT_EQ(A.Kind, SessionExitKind::Quantum);
  auto B = llvm::cantFail(Second->run(Code, Quantum));
  EXPECT_EQ(B.Kind, SessionExitKind::Quantum);
  EXPECT_EQ(reg(*First, Result), 1u);
  EXPECT_EQ(reg(*Second, Result), 2u);
  EXPECT_EQ(llvm::cantFail(Second->cpu().readInteger(Data, sizeof(uint64_t))),
            2u);
  EXPECT_EQ(Budget->instructions(), Quantum * 2);
  First.reset();
  auto C = llvm::cantFail(Second->run(reg(*Second, PC), Quantum));
  EXPECT_EQ(C.Kind, SessionExitKind::InstructionLimit);
  EXPECT_EQ(Budget->instructions(), Instructions);
  EXPECT_EQ(reg(*Second, Result), 3u);
  EXPECT_EQ(llvm::cantFail(Second->cpu().readInteger(Data, sizeof(uint64_t))),
            2u);
}
TEST_P(RuntimeSession, PendingServiceBlocksResumptionUntilConsumedExactlyOnce) {
  code(ServiceX64, ServiceARM);
  auto S = start();
  auto Exit = llvm::cantFail(S->run(Code, Quantum));
  ASSERT_TRUE(Exit.CPU);
  ASSERT_EQ(Exit.CPU->Kind, ExecutionExitKind::ServiceRequest);
  auto Repeated = S->run(Code, Quantum);
  EXPECT_FALSE(bool(Repeated));
  llvm::consumeError(Repeated.takeError());
  EXPECT_EQ(Budget->instructions(), 1u);
  auto Request = llvm::cantFail(S->takeServiceRequest());
  auto Again = S->takeServiceRequest();
  EXPECT_FALSE(bool(Again));
  llvm::consumeError(Again.takeError());
  llvm::cantFail(S->cpu().writeRegister(Result, {Updated, 0}));
  auto Resume = llvm::cantFail(S->run(Request.NextPC, 1));
  EXPECT_EQ(Resume.Kind, SessionExitKind::Quantum);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, sizeof(uint64_t))),
            Updated);
  EXPECT_EQ(Budget->instructions(), 2u);
}
TEST_P(RuntimeSession, RecoverableFaultRequiresAnExplicitTransferBoundary) {
  code(LoadX64, LoadARM);
  llvm::cantFail(CPU->writeRegister(Source, {Alias, 0}));
  auto S = start([](const BackendFault &F) {
    return F.Access == BackendAccessKind::Read;
  });
  auto Exit = llvm::cantFail(S->run(Code, Quantum));
  ASSERT_TRUE(Exit.CPU);
  ASSERT_EQ(Exit.CPU->Kind, ExecutionExitKind::RecoverableFault);
  auto Repeated = S->run(Code, Quantum);
  EXPECT_FALSE(bool(Repeated));
  llvm::consumeError(Repeated.takeError());
  auto Fault = llvm::cantFail(S->takeRecoverableFault());
  EXPECT_EQ(Fault.Address, Alias);
  llvm::cantFail(S->cpu().map(Alias, PageSize, Read | Write | UserAccessible));
  llvm::cantFail(S->cpu().writeInteger(Alias, Value, sizeof(uint64_t)));
  auto Resume = llvm::cantFail(S->run(Fault.PC, 1));
  EXPECT_EQ(Resume.Kind, SessionExitKind::Quantum);
  EXPECT_EQ(reg(*S, Result), Value);
  EXPECT_EQ(Budget->instructions(), 2u);
}
TEST_P(RuntimeSession, TerminalFaultNeverBecomesAQuantumOrSuccessfulResume) {
  code(LoadX64, LoadARM);
  llvm::cantFail(CPU->writeRegister(Source, {Alias, 0}));
  auto S = start();
  auto Exit = llvm::cantFail(S->run(Code, 1));
  EXPECT_EQ(Exit.Kind, SessionExitKind::CPU);
  ASSERT_TRUE(Exit.CPU);
  EXPECT_EQ(Exit.CPU->Kind, ExecutionExitKind::GuestFault);
  auto Repeated = S->run(Code, Quantum);
  EXPECT_FALSE(bool(Repeated));
  llvm::consumeError(Repeated.takeError());
  EXPECT_EQ(Budget->instructions(), 1u);
}
TEST_P(RuntimeSession, ModelFaultOutranksAnAlreadyExhaustedBudget) {
  code(LoadX64, LoadARM);
  auto S = start();
  EXPECT_TRUE(Budget->consumeInstructions(Instructions));
  auto Value = S->cpu().readInteger(Alias, sizeof(uint64_t));
  EXPECT_FALSE(bool(Value));
  llvm::consumeError(Value.takeError());
  auto Exit = S->run(Code, Quantum);
  EXPECT_FALSE(bool(Exit));
  llvm::consumeError(Exit.takeError());
  EXPECT_TRUE(S->cpu().fault());
}
TEST_P(RuntimeSession, ZeroQuantumDoesNotConsumeInstructionCredits) {
  code(CounterX64, CounterARM);
  auto S = start();
  auto Exit = S->run(Code, 0);
  EXPECT_FALSE(bool(Exit));
  llvm::consumeError(Exit.takeError());
  EXPECT_EQ(Budget->instructions(), 0u);
  EXPECT_EQ(llvm::cantFail(S->run(Code, Quantum)).Kind,
            SessionExitKind::Quantum);
}
TEST_P(RuntimeSession, ExecutionWatchStopsBeforeAdmissionAndResumesOnce) {
  code(CounterX64, CounterARM);
  const uint64_t Store =
      Code + (GetParam().ISA == GuestArchitecture::X64 ? StoreOffsetX64
                                                       : StoreOffsetARM);
  Budget =
      llvm::cantFail(ExecutionBudget::create({Instructions, Events, Timeout}));
  std::vector<uint64_t> Trace;
  auto S = llvm::cantFail(ExecutionSession::create(
      std::move(CPU), Budget, {},
      [&](uint64_t Address, uint32_t) { Trace.push_back(Address); }));
  llvm::cantFail(S->watchExecution({{Store, 1}}));
  auto Exit = llvm::cantFail(S->run(Code, Instructions));
  // The watched instruction was neither observed, charged nor executed.
  ASSERT_EQ(Exit.Kind, SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, PC), Store);
  EXPECT_EQ(Budget->instructions(), BeforeStore);
  EXPECT_EQ(Trace.size(), BeforeStore);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, sizeof(uint64_t))), 0u);
  // Resuming at the reported address executes it once; the watch, which is
  // still installed, fires again on the next iteration.
  Exit = llvm::cantFail(S->run(Store, Instructions));
  ASSERT_EQ(Exit.Kind, SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, PC), Store);
  EXPECT_EQ(Budget->instructions(), BeforeStore + LoopInstructions);
  EXPECT_EQ(Trace.size(), Budget->instructions());
  EXPECT_EQ(Trace[BeforeStore], Store);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, sizeof(uint64_t))), 1u);
}

TEST_P(RuntimeSession, CommittedWriteWatchStopsBeforeTheNextAdmission) {
  code(CounterX64, CounterARM);
  auto S = start();
  llvm::cantFail(S->watchMemoryWrites({{Data, sizeof(uint64_t)}}));
  const uint64_t Next =
      Code + (GetParam().ISA == GuestArchitecture::X64 ? 9 : 12);
  auto Exit = llvm::cantFail(S->run(Code, Instructions));
  ASSERT_EQ(Exit.Kind, SessionExitKind::MemoryWriteWatch);
  EXPECT_EQ(reg(*S, PC), Next);
  EXPECT_EQ(Budget->instructions(), 3u);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, 8)), 1u);
  Exit = llvm::cantFail(S->run(Next, Instructions));
  ASSERT_EQ(Exit.Kind, SessionExitKind::MemoryWriteWatch);
  EXPECT_EQ(reg(*S, PC), Next);
  EXPECT_EQ(Budget->instructions(), 7u);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, 8)), 2u);
  llvm::cantFail(S->watchMemoryWrites({}));
  EXPECT_EQ(llvm::cantFail(S->run(Next, Instructions)).Kind,
            SessionExitKind::InstructionLimit);
  EXPECT_EQ(Budget->instructions(), Instructions);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, 8)), 2u);
}

TEST_P(RuntimeSession, WriteWatchesFollowPhysicalAliasesAndMappingChanges) {
  code(CounterX64, CounterARM);
  const uint64_t Other = Data + PageSize;
  llvm::cantFail(CPU->map(Other, PageSize, Read | Write | UserAccessible));
  llvm::cantFail(
      CPU->mapAlias(Alias, Data, PageSize, Read | Execute | UserAccessible));
  auto S = start();
  // The writer uses Data, while the watch names another virtual address.
  llvm::cantFail(S->watchMemoryWrites({{Alias + 3, 2}}));
  ASSERT_EQ(llvm::cantFail(S->run(Code, Instructions)).Kind,
            SessionExitKind::MemoryWriteWatch);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, 8)), 1u);
  llvm::cantFail(S->cpu().replaceAliases(
      {{Alias, PageSize}},
      {{Alias, Other, PageSize, Read | Execute | UserAccessible}}));
  // The same watch now names different backing. An old physical match must
  // disappear even though the watch set itself has not changed.
  ASSERT_EQ(llvm::cantFail(S->run(Code, Quantum)).Kind,
            SessionExitKind::Quantum);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, 8)), 2u);
  llvm::cantFail(S->cpu().writeRegister(Source, {Other, 0}));
  ASSERT_EQ(llvm::cantFail(S->run(Code, Instructions)).Kind,
            SessionExitKind::MemoryWriteWatch);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Other, 8)), 1u);
  EXPECT_EQ(Budget->instructions(), Instructions);
}

TEST_P(RuntimeSession, InvalidWriteWatchReplacementPreservesThePreviousSet) {
  code(CounterX64, CounterARM);
  auto S = start();
  llvm::cantFail(S->watchMemoryWrites({{Data + 4, 4}, {Data, 6}}));
  for (const auto W :
       {MemoryWriteWatch{Data, 0}, MemoryWriteWatch{UINT64_MAX, 2}}) {
    auto E = S->watchMemoryWrites({W});
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  }
  auto E = S->watchMemoryWrites({{0, UINT64_MAX}, {UINT64_MAX, 1}});
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  ASSERT_EQ(llvm::cantFail(S->run(Code, Instructions)).Kind,
            SessionExitKind::MemoryWriteWatch);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, 8)), 1u);
  EXPECT_EQ(Budget->instructions(), 3u);
}

TEST_P(RuntimeSession, StoppedInstructionInspectionHasNoExecutionEffects) {
  code(CounterX64, CounterARM);
  const auto Before = llvm::cantFail(CPU->readRegister(PC));
  if (GetParam().ISA == GuestArchitecture::X64) {
    constexpr uint8_t Nop[] = {0x90}, Prefix[] = {0x0f};
    llvm::cantFail(CPU->write(Code + PageSize - 1, Nop));
    EXPECT_EQ(llvm::cantFail(CPU->instructionSize(Code + PageSize - 1)), 1u);
    llvm::cantFail(CPU->write(Code + PageSize - 1, Prefix));
    auto Bad = CPU->instructionSize(Code + PageSize - 1);
    EXPECT_FALSE(bool(Bad));
    llvm::consumeError(Bad.takeError());
  } else
    EXPECT_EQ(llvm::cantFail(CPU->instructionSize(Code)), 4u);
  auto Unmapped = CPU->instructionSize(Alias);
  EXPECT_FALSE(bool(Unmapped));
  llvm::consumeError(Unmapped.takeError());
  EXPECT_FALSE(CPU->fault());
  EXPECT_EQ(llvm::cantFail(CPU->readRegister(PC)), Before);
  auto S = start();
  EXPECT_EQ(llvm::cantFail(S->run(Code, Quantum)).Kind,
            SessionExitKind::Quantum);
  EXPECT_EQ(Budget->instructions(), Quantum);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, 8)), 1u);
}

TEST_P(RuntimeSession, CancelledStoreCannotPublishAWriteWatch) {
  code(CounterX64, CounterARM);
  const uint64_t Store =
      Code + (GetParam().ISA == GuestArchitecture::X64 ? StoreOffsetX64
                                                       : StoreOffsetARM);
  auto *Raw = CPU.get();
  bool Cancel = true;
  Budget =
      llvm::cantFail(ExecutionBudget::create({Instructions, Events, Timeout}));
  auto S = llvm::cantFail(ExecutionSession::create(
      std::move(CPU), Budget, {}, [&](uint64_t Address, uint32_t) {
        if (Address == Store && Cancel) {
          Cancel = false;
          Raw->stop();
        }
      }));
  llvm::cantFail(S->watchMemoryWrites({{Data, 8}}));
  auto Exit = llvm::cantFail(S->run(Code, Instructions));
  ASSERT_EQ(Exit.Kind, SessionExitKind::CPU);
  ASSERT_TRUE(Exit.CPU);
  EXPECT_EQ(Exit.CPU->Kind, ExecutionExitKind::Stopped);
  EXPECT_EQ(reg(*S, PC), Store);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, 8)), 0u);
  EXPECT_EQ(Budget->instructions(), 3u);
  ASSERT_EQ(llvm::cantFail(S->run(Store, Instructions)).Kind,
            SessionExitKind::MemoryWriteWatch);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, 8)), 1u);
  EXPECT_EQ(Budget->instructions(), 4u);
}
TEST_P(RuntimeSession, OnlyAContinuationAtTheWatchedAddressStepsOverIt) {
  code(CounterX64, CounterARM);
  const uint64_t Store =
      Code + (GetParam().ISA == GuestArchitecture::X64 ? StoreOffsetX64
                                                       : StoreOffsetARM);
  auto S = start();
  llvm::cantFail(S->watchExecution({{Store, 1}}));
  ASSERT_EQ(llvm::cantFail(S->run(Code, Instructions)).Kind,
            SessionExitKind::ExecutionWatch);
  // Restarting the loop is a different continuation: the watch still holds.
  auto Exit = llvm::cantFail(S->run(Code, Instructions));
  ASSERT_EQ(Exit.Kind, SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, PC), Store);
  EXPECT_EQ(Budget->instructions(), 2 * BeforeStore);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, sizeof(uint64_t))), 0u);
  // A quantum that ends first does not consume the watch either.
  llvm::cantFail(S->watchExecution({}));
  EXPECT_EQ(llvm::cantFail(S->run(Store, 1)).Kind, SessionExitKind::Quantum);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, sizeof(uint64_t))), 1u);
}
TEST_P(RuntimeSession, ExecutionWatchesMergeAndRejectInvalidRanges) {
  code(CounterX64, CounterARM);
  auto S = start();
  for (const ExecutionWatch Invalid :
       {ExecutionWatch{Code, 0}, ExecutionWatch{UINT64_MAX, 2},
        ExecutionWatch{0, 0}}) {
    auto E = S->watchExecution({Invalid});
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  }
  // The whole address space cannot be stated as one size.
  auto Everything = S->watchExecution({{0, UINT64_MAX}, {UINT64_MAX, 1}});
  EXPECT_TRUE(bool(Everything));
  llvm::consumeError(std::move(Everything));
  // A rejected set leaves the session unwatched.
  EXPECT_EQ(llvm::cantFail(S->run(Code, Quantum)).Kind,
            SessionExitKind::Quantum);
  // Overlapping, adjacent and unordered ranges are one watch.
  llvm::cantFail(S->watchExecution({{Code + LoopBytes / 2, LoopBytes / 2},
                                    {Code, LoopBytes / 2},
                                    {Code, 1}}));
  const uint64_t At = reg(*S, PC);
  auto Exit = llvm::cantFail(S->run(At, Quantum));
  ASSERT_EQ(Exit.Kind, SessionExitKind::ExecutionWatch);
  EXPECT_EQ(reg(*S, PC), At);
  EXPECT_EQ(Budget->instructions(), Quantum);
}
TEST_P(RuntimeSession, CheckedQuantumEndsBeforeTheNextExecutionWatch) {
  code(CounterX64, CounterARM);
  const uint64_t Store =
      Code + (GetParam().ISA == GuestArchitecture::X64 ? StoreOffsetX64
                                                       : StoreOffsetARM);
  auto S = start();
  llvm::cantFail(S->watchExecution({{Store, 1}}));
  EXPECT_EQ(llvm::cantFail(S->run(Code, BeforeStore)).Kind,
            SessionExitKind::Quantum);
  EXPECT_EQ(reg(*S, PC), Store);
  EXPECT_EQ(Budget->instructions(), BeforeStore);
  EXPECT_EQ(llvm::cantFail(S->run(Store, 1)).Kind,
            SessionExitKind::ExecutionWatch);
  EXPECT_EQ(Budget->instructions(), BeforeStore);
  EXPECT_EQ(llvm::cantFail(S->run(Store, 1)).Kind, SessionExitKind::Quantum);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, 8)), 1u);
}
TEST_P(RuntimeSession, WatchesCannotChangeUnderAPendingContinuation) {
  code(ServiceX64, ServiceARM);
  auto S = start();
  auto Exit = llvm::cantFail(S->run(Code, Quantum));
  ASSERT_TRUE(Exit.CPU);
  ASSERT_EQ(Exit.CPU->Kind, ExecutionExitKind::ServiceRequest);
  auto E = S->watchExecution({{Code, 1}});
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  llvm::cantFail(S->takeServiceRequest());
  llvm::cantFail(S->watchExecution({{Code, 1}}));
}
TEST_P(RuntimeSession,
       ContextRestoresWatchStepOverAndWriteWatchesWithoutRAMRollback) {
  code(CounterX64, CounterARM);
  const auto Store =
      Code + (GetParam().ISA == GuestArchitecture::X64 ? StoreOffsetX64
                                                       : StoreOffsetARM);
  auto S = start();
  llvm::cantFail(S->watchExecution({{Store, 1}}));
  llvm::cantFail(S->watchMemoryWrites({{Data, 8}}));
  ASSERT_EQ(llvm::cantFail(S->run(Code, Instructions)).Kind,
            SessionExitKind::ExecutionWatch);
  auto Saved = S->saveContext();
  ASSERT_TRUE(bool(Saved)) << llvm::toString(Saved.takeError());
  llvm::cantFail(S->watchReturn(Code + ImageOffset));
  llvm::cantFail(S->watchExecution({}));
  llvm::cantFail(S->watchMemoryWrites({}));
  llvm::cantFail(S->cpu().writeRegister(Result, {9, 0}));
  ASSERT_EQ(llvm::cantFail(S->run(Store, 1)).Kind, SessionExitKind::Quantum);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, 8)), 9u);
  ASSERT_EQ(llvm::toString(S->restoreContext(**Saved)), "");
  EXPECT_EQ(reg(*S, Result), 1u);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, 8)), 9u);
  auto Exit = S->run(Store, Instructions);
  ASSERT_TRUE(bool(Exit)) << llvm::toString(Exit.takeError());
  EXPECT_EQ(Exit->Kind, SessionExitKind::MemoryWriteWatch);
  EXPECT_EQ(llvm::cantFail(S->cpu().readInteger(Data, 8)), 1u);
  EXPECT_EQ(Budget->instructions(), BeforeStore + 2);
}

TEST_P(RuntimeSession,
       ReservedReturnCannotStepOverOrDisappearWhenWatchesChange) {
  code(CounterX64, CounterARM);
  auto S = start();
  ASSERT_EQ(llvm::toString(S->watchReturn(Code)), "");
  ASSERT_EQ(llvm::toString(S->watchExecution({})), "");
  for (unsigned I = 0; I < 2; ++I) {
    auto Exit = S->run(Code, Instructions);
    ASSERT_TRUE(bool(Exit));
    EXPECT_EQ(Exit->Kind, SessionExitKind::InvocationReturn);
    EXPECT_TRUE(S->hasInvocationReturn());
    EXPECT_EQ(Budget->instructions(), 0u);
  }
  EXPECT_NE(llvm::toString(S->watchReturn(Code + ImageOffset)), "");
}

TEST_P(RuntimeSession, ForeignSessionContextIsRejectedBeforeCPUOrWatchEffects) {
  auto First = start();
  auto Saved = First->saveContext();
  ASSERT_TRUE(bool(Saved));
  auto Other =
      createExecutionBackend(GetParam().Backend, GetParam().Contract,
                             First->cpu().addressSpace(), GetParam().ISA);
  ASSERT_TRUE(bool(Other));
  auto Second = ExecutionSession::create(std::move(Other->CPU), Budget);
  ASSERT_TRUE(bool(Second));
  ASSERT_EQ(llvm::toString((*Second)->cpu().writeRegister(Result, {99, 0})),
            "");
  EXPECT_NE(llvm::toString((*Second)->restoreContext(**Saved)), "");
  EXPECT_EQ(reg(**Second, Result), 99u);
  EXPECT_TRUE((*Second)->readyToRun());
}

TEST_P(RuntimeSession,
       PendingServicePreventsSessionContextCaptureAndRestoration) {
  code(ServiceX64, ServiceARM);
  auto S = start();
  auto Saved = S->saveContext();
  ASSERT_TRUE(bool(Saved));
  auto Exit = S->run(Code, Quantum);
  ASSERT_TRUE(bool(Exit));
  ASSERT_TRUE(Exit->CPU);
  ASSERT_EQ(Exit->CPU->Kind, ExecutionExitKind::ServiceRequest);
  auto Invalid = S->saveContext();
  ASSERT_FALSE(bool(Invalid));
  llvm::consumeError(Invalid.takeError());
  EXPECT_NE(llvm::toString(S->restoreContext(**Saved)), "");
  EXPECT_NE(llvm::toString(S->watchReturn(Code)), "");
  auto Request = S->takeServiceRequest();
  ASSERT_TRUE(bool(Request));
  EXPECT_EQ(llvm::toString(S->restoreContext(**Saved)), "");
  EXPECT_EQ(Budget->instructions(), 1u);
}

TEST_P(RuntimeSession, LocalTimeCapDoesNotReplenishOrReplaceTheSharedDeadline) {
  code(CounterX64, CounterARM);
  auto S = start();
  auto Exit = S->run(Code, Instructions, 0);
  ASSERT_TRUE(bool(Exit));
  EXPECT_EQ(Exit->Kind, SessionExitKind::Timeout);
  EXPECT_FALSE(Exit->CPU);
  EXPECT_EQ(Budget->instructions(), 0u);
  EXPECT_EQ(llvm::cantFail(S->run(Code, Quantum)).Kind,
            SessionExitKind::Quantum);
}

TEST_P(RuntimeSession,
       LiveObserverCannotCaptureRestoreMutateOrReenterTheSession) {
  code(CounterX64, CounterARM);
  Budget =
      llvm::cantFail(ExecutionBudget::create({Instructions, Events, Timeout}));
  std::unique_ptr<ExecutionSession> S;
  std::unique_ptr<SessionContext> Saved;
  bool Checked = false;
  auto Created = ExecutionSession::create(
      std::move(CPU), Budget, {}, [&](uint64_t, uint32_t) {
        if (Checked)
          return;
        Checked = true;
        EXPECT_FALSE(S->readyToRun());
        auto Capture = S->saveContext();
        EXPECT_FALSE(bool(Capture));
        if (!Capture)
          llvm::consumeError(Capture.takeError());
        EXPECT_NE(llvm::toString(S->restoreContext(*Saved)), "");
        EXPECT_NE(llvm::toString(S->watchExecution({{Code, 1}})), "");
        EXPECT_NE(llvm::toString(S->watchMemoryWrites({{Data, 8}})), "");
        EXPECT_NE(llvm::toString(S->watchReturn(Code + ImageOffset)), "");
        auto Nested = S->run(Code, Quantum);
        EXPECT_FALSE(bool(Nested));
        if (!Nested)
          llvm::consumeError(Nested.takeError());
        S->stop();
      });
  ASSERT_TRUE(bool(Created));
  S = std::move(*Created);
  auto Context = S->saveContext();
  ASSERT_TRUE(bool(Context));
  Saved = std::move(*Context);
  ASSERT_EQ(llvm::cantFail(S->run(Code, Quantum)).Kind, SessionExitKind::CPU);
  EXPECT_TRUE(Checked);
  EXPECT_TRUE(S->readyToRun());
  EXPECT_FALSE(S->cpu().fault());
  EXPECT_EQ(llvm::cantFail(S->run(Code, Quantum)).Kind,
            SessionExitKind::Quantum);
  EXPECT_EQ(Budget->instructions(), 1 + Quantum);
}

TEST_P(RuntimeSession,
       ModelBoundaryRemainsUnchargedAtTheExactInstructionLimit) {
  code(CounterX64, CounterARM);
  Budget =
      llvm::cantFail(ExecutionBudget::create({Instructions, Events, Timeout}));
  SessionModelHooks Hooks;
  bool Boundary = true;
  Hooks.BeforeInstruction = [&](uint64_t, uint32_t) {
    return Boundary && Budget->instructions() == Instructions
               ? InstructionAdmission::Stop
               : InstructionAdmission::Execute;
  };
  uint64_t Observed = 0;
  auto S = llvm::cantFail(ExecutionSession::create(
      std::move(CPU), Budget, {}, [&](uint64_t, uint32_t) { ++Observed; }, {},
      std::move(Hooks)));
  auto Exit = llvm::cantFail(S->run(Code, Instructions));
  EXPECT_EQ(Exit.Kind, SessionExitKind::ModelBoundary);
  EXPECT_EQ(Observed, Instructions);
  EXPECT_EQ(Budget->instructions(), Instructions);
  const auto BoundaryPC = reg(*S, PC);
  EXPECT_EQ(llvm::cantFail(S->run(BoundaryPC, Quantum)).Kind,
            SessionExitKind::ModelBoundary);
  EXPECT_EQ(Observed, Instructions);
  Boundary = false;
  EXPECT_EQ(llvm::cantFail(S->run(BoundaryPC, Quantum)).Kind,
            SessionExitKind::InstructionLimit);
  EXPECT_EQ(Observed, Instructions);
  EXPECT_EQ(reg(*S, PC), BoundaryPC);
}

TEST_P(RuntimeSession,
       EmulatedAttemptChargesOnceWithoutMachineOrMemoryEffects) {
  code(CounterX64, CounterARM);
  llvm::cantFail(CPU->writeRegister(Result, {99, 0}));
  Budget =
      llvm::cantFail(ExecutionBudget::create({Instructions, Events, Timeout}));
  SessionModelHooks Hooks;
  Hooks.BeforeInstruction = [](uint64_t, uint32_t) {
    return InstructionAdmission::Emulate;
  };
  uint64_t Reads = 0, Observed = 0;
  Hooks.Read = [&](uint64_t, uint32_t) { ++Reads; };
  auto S = llvm::cantFail(ExecutionSession::create(
      std::move(CPU), Budget, {},
      [&](uint64_t Address, uint32_t Size) {
        ++Observed;
        EXPECT_EQ(Address, Code);
        EXPECT_NE(Size, 0u);
      },
      {}, std::move(Hooks)));
  EXPECT_EQ(llvm::cantFail(S->run(Code, Quantum)).Kind,
            SessionExitKind::ModelBoundary);
  EXPECT_EQ(Budget->instructions(), 1u);
  EXPECT_EQ(Observed, 1u);
  EXPECT_EQ(Reads, 0u);
  EXPECT_EQ(reg(*S, Result), 99u);
  EXPECT_EQ(reg(*S, PC), Code);
  EXPECT_TRUE(S->readyToRun());
}

TEST_P(RuntimeSession, DeferredAttemptOwnsTheBoundaryUntilStoppedAdmission) {
  code(CounterX64, CounterARM);
  llvm::cantFail(CPU->writeRegister(Result, {99, 0}));
  Budget =
      llvm::cantFail(ExecutionBudget::create({Instructions, Events, Timeout}));
  SessionModelHooks Hooks;
  Hooks.BeforeInstruction = [](uint64_t, uint32_t) {
    return InstructionAdmission::Defer;
  };
  std::unique_ptr<ExecutionSession> S;
  uint64_t Observed = 0;
  auto Created = ExecutionSession::create(
      std::move(CPU), Budget, {},
      [&](uint64_t Address, uint32_t) {
        ++Observed;
        EXPECT_EQ(Address, Code);
        EXPECT_FALSE(S->readyToRun());
        auto Reentered = S->admitDeferredInstruction();
        EXPECT_FALSE(bool(Reentered));
        if (!Reentered)
          llvm::consumeError(Reentered.takeError());
      },
      {}, std::move(Hooks));
  ASSERT_TRUE(bool(Created));
  S = std::move(*Created);
  auto Saved = llvm::cantFail(S->saveContext());
  auto Exit = llvm::cantFail(S->run(Code, Quantum));
  EXPECT_EQ(Exit.Kind, SessionExitKind::DeferredInstruction);
  EXPECT_TRUE(S->hasDeferredInstruction());
  EXPECT_FALSE(S->readyToRun());
  EXPECT_EQ(Budget->instructions(), 0u);
  EXPECT_EQ(Observed, 0u);
  EXPECT_EQ(reg(*S, Result), 99u);
  auto RunAgain = S->run(Code, Quantum);
  EXPECT_FALSE(bool(RunAgain));
  if (!RunAgain)
    llvm::consumeError(RunAgain.takeError());
  auto Capture = S->saveContext();
  EXPECT_FALSE(bool(Capture));
  if (!Capture)
    llvm::consumeError(Capture.takeError());
  EXPECT_NE(llvm::toString(S->restoreContext(*Saved)), "");
  EXPECT_NE(llvm::toString(S->watchExecution({{Code, 1}})), "");
  EXPECT_NE(llvm::toString(S->watchMemoryWrites({{Data, 8}})), "");
  EXPECT_NE(llvm::toString(S->watchReturn(Code)), "");
  auto Accepted = S->admitDeferredInstruction();
  ASSERT_TRUE(bool(Accepted));
  EXPECT_FALSE(*Accepted);
  EXPECT_TRUE(S->readyToRun());
  EXPECT_FALSE(S->hasDeferredInstruction());
  EXPECT_EQ(Budget->instructions(), 1u);
  EXPECT_EQ(Observed, 1u);
  EXPECT_EQ(reg(*S, PC), Code);
  EXPECT_EQ(reg(*S, Result), 99u);
  auto Twice = S->admitDeferredInstruction();
  EXPECT_FALSE(bool(Twice));
  if (!Twice)
    llvm::consumeError(Twice.takeError());
  EXPECT_EQ(Budget->instructions(), 1u);
}

TEST_P(RuntimeSession, DeferredAttemptRejectsAChangedPcBeforeCharging) {
  code(CounterX64, CounterARM);
  Budget =
      llvm::cantFail(ExecutionBudget::create({Instructions, Events, Timeout}));
  SessionModelHooks Hooks;
  Hooks.BeforeInstruction = [](uint64_t, uint32_t) {
    return InstructionAdmission::Defer;
  };
  auto S = llvm::cantFail(ExecutionSession::create(std::move(CPU), Budget, {},
                                                   {}, {}, std::move(Hooks)));
  ASSERT_EQ(llvm::cantFail(S->run(Code, Quantum)).Kind,
            SessionExitKind::DeferredInstruction);
  llvm::cantFail(S->cpu().writeRegister(PC, {Code + ImageOffset, 0}));
  auto Invalid = S->admitDeferredInstruction();
  EXPECT_FALSE(bool(Invalid));
  if (!Invalid)
    EXPECT_NE(llvm::toString(Invalid.takeError()).find("PC"),
              std::string::npos);
  EXPECT_TRUE(S->hasDeferredInstruction());
  EXPECT_EQ(Budget->instructions(), 0u);
  llvm::cantFail(S->cpu().writeRegister(PC, {Code, 0}));
  EXPECT_FALSE(llvm::cantFail(S->admitDeferredInstruction()));
  EXPECT_EQ(Budget->instructions(), 1u);
}

TEST_P(RuntimeSession, DeferredAttemptCannotReplenishASharedInstructionBudget) {
  code(CounterX64, CounterARM);
  Budget =
      llvm::cantFail(ExecutionBudget::create({Instructions, Events, Timeout}));
  SessionModelHooks Hooks;
  Hooks.BeforeInstruction = [](uint64_t, uint32_t) {
    return InstructionAdmission::Defer;
  };
  auto S = llvm::cantFail(ExecutionSession::create(std::move(CPU), Budget, {},
                                                   {}, {}, std::move(Hooks)));
  ASSERT_EQ(llvm::cantFail(S->run(Code, Quantum)).Kind,
            SessionExitKind::DeferredInstruction);
  ASSERT_TRUE(Budget->consumeInstructions(Instructions));
  EXPECT_EQ(llvm::cantFail(S->admitDeferredInstruction()),
            SessionExitKind::InstructionLimit);
  EXPECT_EQ(Budget->instructions(), Instructions);
  EXPECT_TRUE(S->hasDeferredInstruction());
}

TEST_P(RuntimeSession,
       DeferredAttemptCannotPublishAdmissionAfterActiveTimeExhaustion) {
  code(CounterX64, CounterARM);
  Budget = llvm::cantFail(ExecutionBudget::createWithTimePolicy(
      {Instructions, Events, Timeout}, ExecutionTimePolicy::ActiveWork));
  SessionModelHooks Hooks;
  Hooks.BeforeInstruction = [](uint64_t, uint32_t) {
    return InstructionAdmission::Defer;
  };
  auto S = llvm::cantFail(ExecutionSession::create(std::move(CPU), Budget, {},
                                                   {}, {}, std::move(Hooks)));
  ASSERT_EQ(llvm::cantFail(S->run(Code, Quantum)).Kind,
            SessionExitKind::DeferredInstruction);
  auto Now = ExecutionBudget::Clock::now();
  auto ModelWork = llvm::cantFail(Budget->enterWork(Now));
  llvm::cantFail(ModelWork->close(Now + std::chrono::microseconds(Timeout)));
  EXPECT_EQ(llvm::cantFail(S->admitDeferredInstruction()),
            SessionExitKind::Timeout);
  EXPECT_EQ(Budget->instructions(), 0u);
  EXPECT_TRUE(S->hasDeferredInstruction());
}

TEST_P(RuntimeSession, WatchesAndReservedReturnsPrecedeModelAdmission) {
  code(CounterX64, CounterARM);
  Budget =
      llvm::cantFail(ExecutionBudget::create({Instructions, Events, Timeout}));
  SessionModelHooks Hooks;
  uint64_t Decisions = 0;
  Hooks.BeforeInstruction = [&](uint64_t, uint32_t) {
    ++Decisions;
    return InstructionAdmission::Stop;
  };
  auto S = llvm::cantFail(ExecutionSession::create(std::move(CPU), Budget, {},
                                                   {}, {}, std::move(Hooks)));
  auto Saved = llvm::cantFail(S->saveContext());
  llvm::cantFail(S->watchExecution({{Code, 1}}));
  EXPECT_EQ(llvm::cantFail(S->run(Code, Quantum)).Kind,
            SessionExitKind::ExecutionWatch);
  EXPECT_EQ(Decisions, 0u);
  llvm::cantFail(S->watchReturn(Code));
  EXPECT_EQ(llvm::cantFail(S->run(Code, Quantum)).Kind,
            SessionExitKind::InvocationReturn);
  EXPECT_EQ(Decisions, 0u);
  EXPECT_EQ(Budget->instructions(), 0u);
  llvm::cantFail(S->restoreContext(*Saved));
  EXPECT_EQ(llvm::cantFail(S->run(Code, Quantum)).Kind,
            SessionExitKind::ModelBoundary);
  EXPECT_EQ(Decisions, 1u);
  EXPECT_EQ(Budget->instructions(), 0u);
}

TEST_P(RuntimeSession,
       DeferredAttemptRetainsItsLocalCapWhileActiveBudgetWaits) {
  code(CounterX64, CounterARM);
  Budget = llvm::cantFail(ExecutionBudget::createWithTimePolicy(
      {Instructions, Events, Timeout}, ExecutionTimePolicy::ActiveWork));
  SessionModelHooks Hooks;
  Hooks.BeforeInstruction = [](uint64_t, uint32_t) {
    return InstructionAdmission::Defer;
  };
  auto S = llvm::cantFail(ExecutionSession::create(std::move(CPU), Budget, {},
                                                   {}, {}, std::move(Hooks)));
  const auto Start = ExecutionBudget::Clock::now();
  ASSERT_EQ(llvm::cantFail(S->run(Code, Quantum, 100000)).Kind,
            SessionExitKind::DeferredInstruction);
  std::this_thread::sleep_until(Start + std::chrono::milliseconds(150));
  ASSERT_GT(Budget->remainingMicroseconds(), Timeout / 2);
  EXPECT_EQ(llvm::cantFail(S->admitDeferredInstruction()),
            SessionExitKind::Timeout);
  EXPECT_EQ(Budget->instructions(), 0u);
  EXPECT_TRUE(S->hasDeferredInstruction());
}

TEST_P(RuntimeSession, AQuantumCannotCreateOrDoubleChargeADeferredAttempt) {
  code(CounterX64, CounterARM);
  Budget =
      llvm::cantFail(ExecutionBudget::create({Instructions, Events, Timeout}));
  SessionModelHooks Hooks;
  Hooks.BeforeInstruction = [](uint64_t Address, uint32_t) {
    return Address == Code ? InstructionAdmission::Execute
                           : InstructionAdmission::Defer;
  };
  uint64_t Observed = 0;
  auto S = llvm::cantFail(ExecutionSession::create(
      std::move(CPU), Budget, {}, [&](uint64_t, uint32_t) { ++Observed; }, {},
      std::move(Hooks)));
  EXPECT_EQ(llvm::cantFail(S->run(Code, 1)).Kind, SessionExitKind::Quantum);
  EXPECT_FALSE(S->hasDeferredInstruction());
  EXPECT_EQ(Observed, 1u);
  const auto Next = reg(*S, PC);
  EXPECT_NE(Next, Code);
  EXPECT_EQ(llvm::cantFail(S->run(Next, 1)).Kind,
            SessionExitKind::DeferredInstruction);
  EXPECT_EQ(Observed, 1u);
  EXPECT_FALSE(llvm::cantFail(S->admitDeferredInstruction()));
  EXPECT_EQ(Observed, 2u);
  EXPECT_EQ(Budget->instructions(), 2u);
  EXPECT_EQ(reg(*S, PC), Next);
  EXPECT_EQ(reg(*S, Result), 0u);
}

TEST_P(RuntimeSession, ModelMemoryObserversRetainTypedFaultPrecedence) {
  code(CounterX64, CounterARM);
  Budget =
      llvm::cantFail(ExecutionBudget::create({Instructions, Events, Timeout}));
  SessionModelHooks Hooks;
  bool Boundary = true;
  uint64_t Reads = 0, Writes = 0, Faults = 0;
  std::unique_ptr<ExecutionSession> S;
  Hooks.BeforeInstruction = [&](uint64_t, uint32_t) {
    return Boundary && Budget->instructions() == LoopInstructions
               ? InstructionAdmission::Stop
               : InstructionAdmission::Execute;
  };
  Hooks.Read = [&](uint64_t Address, uint32_t Size) {
    ++Reads;
    EXPECT_EQ(Address, Boundary ? Data : Data + PageSize);
    EXPECT_EQ(Size, 8u);
  };
  Hooks.Write = [&](uint64_t Address, uint32_t Size, uint64_t Value) {
    ++Writes;
    EXPECT_EQ(Address, Data);
    EXPECT_EQ(Size, 8u);
    EXPECT_EQ(Value, 1u);
  };
  Hooks.Fault = [&](uint64_t Address, uint32_t, const char *) {
    ++Faults;
    EXPECT_EQ(Address, Data + PageSize);
    S->stop();
  };
  auto Created = ExecutionSession::create(std::move(CPU), Budget, {}, {}, {},
                                          std::move(Hooks));
  ASSERT_TRUE(bool(Created));
  S = std::move(*Created);
  EXPECT_EQ(llvm::cantFail(S->run(Code, Instructions)).Kind,
            SessionExitKind::ModelBoundary);
  EXPECT_EQ(Reads, 1u);
  EXPECT_EQ(Writes, 1u);
  EXPECT_EQ(Faults, 0u);
  EXPECT_EQ(*S->cpu().readInteger(Data, 8), 1u);
  Boundary = false;
  llvm::cantFail(S->cpu().writeRegister(Source, {Data + PageSize, 0}));
  auto Exit = llvm::cantFail(S->run(Code, Quantum));
  ASSERT_TRUE(Exit.CPU);
  EXPECT_EQ(Exit.CPU->Kind, ExecutionExitKind::GuestFault);
  EXPECT_TRUE(Exit.CPU->Fault);
  EXPECT_EQ(Faults, 1u);
  EXPECT_EQ(Reads, 2u);
  EXPECT_FALSE(S->readyToRun());
  EXPECT_EQ(Budget->instructions(), LoopInstructions + 1);
}

TEST_P(RuntimeSession, DeferredObserverExceptionIsTerminalAfterOneCharge) {
  code(CounterX64, CounterARM);
  Budget =
      llvm::cantFail(ExecutionBudget::create({Instructions, Events, Timeout}));
  SessionModelHooks Hooks;
  Hooks.BeforeInstruction = [](uint64_t, uint32_t) {
    return InstructionAdmission::Defer;
  };
  auto S = llvm::cantFail(ExecutionSession::create(
      std::move(CPU), Budget, {}, [](uint64_t, uint32_t) { throw 7; }, {},
      std::move(Hooks)));
  ASSERT_EQ(llvm::cantFail(S->run(Code, Quantum)).Kind,
            SessionExitKind::DeferredInstruction);
  auto Accepted = S->admitDeferredInstruction();
  EXPECT_FALSE(bool(Accepted));
  if (!Accepted)
    EXPECT_NE(llvm::toString(Accepted.takeError()).find("observer"),
              std::string::npos);
  EXPECT_EQ(Budget->instructions(), 1u);
  EXPECT_FALSE(S->readyToRun());
}

TEST_P(RuntimeSession, ReservedReturnRemainsObservableWithZeroLocalQuantum) {
  code(CounterX64, CounterARM);
  auto S = start();
  llvm::cantFail(S->watchReturn(Code));
  EXPECT_EQ(llvm::cantFail(S->run(Code, 0)).Kind,
            SessionExitKind::InvocationReturn);
  EXPECT_EQ(Budget->instructions(), 0u);
  EXPECT_EQ(reg(*S, Result), 0u);
}

TEST_P(RuntimeSession, ZeroLocalQuantumCannotExecutePastReservedReturn) {
  code(CounterX64, CounterARM);
  auto S = start();
  llvm::cantFail(S->watchReturn(Code + 128));
  EXPECT_EQ(llvm::cantFail(S->run(Code, 0)).Kind, SessionExitKind::Quantum);
  EXPECT_EQ(Budget->instructions(), 0u);
  EXPECT_EQ(reg(*S, PC), Code);
}

TEST_P(RuntimeSession, ModelBoundaryRemainsObservableWithZeroLocalQuantum) {
  code(CounterX64, CounterARM);
  Budget =
      llvm::cantFail(ExecutionBudget::create({Instructions, Events, Timeout}));
  SessionModelHooks Hooks;
  bool Boundary = true;
  Hooks.BeforeInstruction = [&](uint64_t, uint32_t) {
    return Boundary ? InstructionAdmission::Stop
                    : InstructionAdmission::Execute;
  };
  auto S = llvm::cantFail(ExecutionSession::create(std::move(CPU), Budget, {},
                                                   {}, {}, std::move(Hooks)));
  EXPECT_EQ(llvm::cantFail(S->run(Code, 0)).Kind,
            SessionExitKind::ModelBoundary);
  Boundary = false;
  EXPECT_EQ(llvm::cantFail(S->run(Code, 0)).Kind, SessionExitKind::Quantum);
  EXPECT_EQ(Budget->instructions(), 0u);
}

INSTANTIATE_TEST_SUITE_P(Backends, RuntimeSession, testing::ValuesIn(Profiles),
                         [](const testing::TestParamInfo<Profile> &P) {
                           return P.param.Name;
                         });
} // namespace
} // namespace neverd::emulation
