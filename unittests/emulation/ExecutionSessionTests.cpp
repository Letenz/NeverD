//===- ExecutionSessionTests.cpp - Shared budgets and owned continuations -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionSession.h"

#include "llvm/Support/Endian.h"

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
INSTANTIATE_TEST_SUITE_P(Backends, RuntimeSession, testing::ValuesIn(Profiles),
                         [](const testing::TestParamInfo<Profile> &P) {
                           return P.param.Name;
                         });
} // namespace
} // namespace neverd::emulation
