//===- ServiceRequestTests.cpp - Explicit service ownership and admission ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "arch/aarch64/CheckedAArch64Backend.h"
#include "arch/x86_64/CheckedX64Backend.h"
#include "core/ExecutionDiagnostics.h"
#include "core/ExecutionExitBuilder.h"
#include "gtest/gtest.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Endian.h"

#include <stdexcept>

namespace neverd::emulation {
namespace {
#define NEVERD_USER_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "UserExecutionCases.def"
#undef NEVERD_USER_VALUE
#define NEVERD_SERVICE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_SERVICE_TEXT(Name, Value) constexpr char Name[] = Value;
#define NEVERD_SERVICE_X64(Name, ...) constexpr uint8_t Name[] = {__VA_ARGS__};
#define NEVERD_SERVICE_ARM(Name, ...) constexpr uint32_t Name[] = {__VA_ARGS__};
#include "ServiceRequestCases.def"
#undef NEVERD_SERVICE_ARM
#undef NEVERD_SERVICE_X64
#undef NEVERD_SERVICE_TEXT
#undef NEVERD_SERVICE_VALUE

// Any transport entry is an error in the injection profiles. This proves that
// a service handoff does not depend on native or Unicorn exception behavior.
class RejectedX64Machine final : public X64Machine {
public:
  explicit RejectedX64Machine(unsigned &Entries) : Entries(Entries) {}
  llvm::Error step(X64MachineState &, uint64_t, MachineRunControl) override {
    ++Entries;
    return diagnostic::error(diagnostic::KvmRun);
  }

private:
  unsigned &Entries;
};
class RejectedARMMachine final : public AArch64Machine {
public:
  explicit RejectedARMMachine(unsigned &Entries) : Entries(Entries) {}
  llvm::Error step(AArch64MachineState &, MachineRunControl) override {
    ++Entries;
    return diagnostic::error(diagnostic::KvmRun);
  }

private:
  unsigned &Entries;
};
struct Profile {
  const char *Name;
  GuestArchitecture ISA;
  ExecutionContract Contract;
  std::optional<ExecutionBackendKind> Backend;
};
void PrintTo(const Profile &P, std::ostream *OS) { *OS << P.Name; }
constexpr Profile Profiles[] = {
    {CheckedX64Name, GuestArchitecture::X64, ExecutionContract::CheckedUserX64,
     std::nullopt},
    {CheckedARMName, GuestArchitecture::AArch64,
     ExecutionContract::CheckedUserAArch64, std::nullopt},
#define NEVERD_USER_PROFILE(Name, Backend, ISA, Contract)                      \
  {#Name, GuestArchitecture::ISA, ExecutionContract::Contract,                 \
   ExecutionBackendKind::Backend},
#include "UserExecutionCases.def"
#undef NEVERD_USER_PROFILE
};

void code(ExecutionBackend &CPU, llvm::ArrayRef<uint8_t> X64,
          llvm::ArrayRef<uint32_t> ARM) {
  if (CPU.architecture() == GuestArchitecture::X64) {
    llvm::cantFail(CPU.write(Code, X64));
    return;
  }
  std::vector<uint8_t> Bytes(ARM.size() * sizeof(uint32_t));
  for (size_t I = 0; I < ARM.size(); ++I)
    llvm::support::endian::write32le(Bytes.data() + I * sizeof(uint32_t),
                                     ARM[I]);
  llvm::cantFail(CPU.write(Code, Bytes));
}

class ServiceBoundary : public testing::TestWithParam<Profile> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  CPURegister PC, Result;
  unsigned Entries = 0;

  llvm::Expected<std::unique_ptr<ExecutionBackend>> create(bool User) {
    if (GetParam().Backend) {
      const auto Contract = User ? GetParam().Contract
                            : GetParam().ISA == GuestArchitecture::X64
                                ? ExecutionContract::CheckedX64
                                : ExecutionContract::CheckedAArch64;
      auto B = createExecutionBackend(*GetParam().Backend, Contract, Limit,
                                      GetParam().ISA);
      if (!B)
        return B.takeError();
      return std::move(B->CPU);
    }
    auto Memory = llvm::cantFail(MemoryProjection::create(Limit));
    if (GetParam().ISA == GuestArchitecture::X64)
      return CheckedX64Backend::create(
          std::move(Memory), std::make_unique<RejectedX64Machine>(Entries),
          User);
    return CheckedAArch64Backend::create(
        std::move(Memory), std::make_unique<RejectedARMMachine>(Entries), User);
  }
  void SetUp() override {
    auto B = create(true);
    if (!B) {
      auto E = B.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      auto Reason = llvm::toString(std::move(E));
      if (Unavailable)
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    CPU = std::move(*B);
    const bool X64 = GetParam().ISA == GuestArchitecture::X64;
    PC = X64 ? CPURegister::X64PC : CPURegister::AArch64PC;
    Result = X64 ? CPURegister::X64AX : CPURegister::AArch64X0;
    llvm::cantFail(
        CPU->map(Code, PageSize, Read | Write | Execute | UserAccessible));
    llvm::cantFail(CPU->map(Data, PageSize, Read | Write | UserAccessible));
    llvm::cantFail(CPU->writeInteger(Data, Value, sizeof(uint64_t)));
    llvm::cantFail(CPU->writeRegister(PC, {Code, 0}));
    llvm::cantFail(CPU->writeRegister(Result, {Updated, 0}));
    llvm::cantFail(CPU->writeRegister(
        X64 ? CPURegister::X64CX : CPURegister::AArch64X1, {Data, 0}));
    llvm::cantFail(CPU->writeRegister(
        X64 ? CPURegister::X64R11 : CPURegister::AArch64X8, {Value, 0}));
    code(*CPU, ServiceX64, ServiceARM);
  }
  void TearDown() override { EXPECT_EQ(Entries, 0u); }
  uint64_t nextPC() const {
    return Code + (GetParam().ISA == GuestArchitecture::X64 ? X64ServiceSize
                                                            : ARMServiceSize);
  }
  std::vector<RegisterValue> registers() const {
    std::vector<RegisterValue> Values;
    const CPURegister Registers[] = {
#define NEVERD_SCALAR_REGISTER(Arch, Name, Width, Backend)                     \
  CPURegister::Arch##Name,
#define NEVERD_VECTOR_REGISTER(Arch, Index, Backend)                           \
  CPURegister::Arch##V##Index,
#define NEVERD_EXTENDED_REGISTER NEVERD_SCALAR_REGISTER
#include "neverd/emulation/Registers.def"
#undef NEVERD_EXTENDED_REGISTER
#undef NEVERD_VECTOR_REGISTER
#undef NEVERD_SCALAR_REGISTER
    };
    for (auto R : Registers) {
      if (registerMatches(R, CPU->architecture()))
        Values.push_back(llvm::cantFail(CPU->readRegister(R)));
    }
    return Values;
  }
  ExecutionExit request() {
    return llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  }
  void expectRequest(const ExecutionExit &Exit) {
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::ServiceRequest) << Exit.Diagnostic;
    ASSERT_TRUE(Exit.Service);
    const bool X64 = GetParam().ISA == GuestArchitecture::X64;
    EXPECT_EQ(Exit.Service->Kind, X64 ? ServiceRequestKind::X64Syscall
                                      : ServiceRequestKind::AArch64SVC);
    EXPECT_EQ(Exit.Service->PC, Code);
    EXPECT_EQ(Exit.Service->NextPC, nextPC());
    EXPECT_EQ(Exit.Service->Immediate, X64 ? 0 : Immediate);
    EXPECT_EQ(CPU->pendingServiceRequest(), Exit.Service);
    EXPECT_FALSE(Exit.Fault);
    EXPECT_FALSE(Exit.StopRequested);
    EXPECT_FALSE(Exit.DeadlineReached);
    EXPECT_TRUE(Exit.Diagnostic.empty());
  }
};

TEST_P(ServiceBoundary, CapturesBeforePrivilegeEntryAndBeforeFollowingStore) {
  const auto Before = registers();
  unsigned Observed = 0;
  BackendHooks H;
  H.Instruction = [&](uint64_t Address, uint32_t Size) {
    EXPECT_EQ(Address, Code);
    EXPECT_EQ(Address + Size, nextPC());
    EXPECT_FALSE(CPU->pendingServiceRequest());
    EXPECT_FALSE(CPU->takeServiceRequest());
    ++Observed;
  };
  llvm::cantFail(CPU->installHooks(std::move(H)));
  expectRequest(request());
  EXPECT_EQ(Observed, 1u);
  EXPECT_EQ(registers(), Before);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, sizeof(uint64_t))), Value);
}

TEST_P(ServiceBoundary, PendingRequestBlocksStateChangesAndContextRollback) {
  auto Context = llvm::cantFail(CPU->saveContext());
  auto Space = CPU->addressSpace();
  const auto Before = registers();
  auto Exit = request();
  ASSERT_TRUE(Exit.Service);
  auto Again = CPU->runUntilExit(Code, Timeout);
  ASSERT_FALSE(bool(Again));
  EXPECT_EQ(llvm::toString(Again.takeError()), diagnostic::PendingService);
  EXPECT_EQ(llvm::toString(CPU->writeRegister(Result, {Value, 0})),
            diagnostic::PendingService);
  EXPECT_EQ(llvm::toString(CPU->writeInteger(Data, Updated, sizeof(uint64_t))),
            diagnostic::PendingService);
  EXPECT_EQ(llvm::toString(CPU->protect(Code, PageSize, Read)),
            diagnostic::PendingService);
  EXPECT_EQ(llvm::toString(CPU->bindAddressSpace(Space)),
            diagnostic::PendingService);
  EXPECT_EQ(llvm::toString(CPU->restoreContext(*Context)),
            diagnostic::PendingService);
  EXPECT_EQ(llvm::toString(CPU->saveContext(*Context)),
            diagnostic::PendingService);
  EXPECT_EQ(llvm::toString(CPU->installHooks({})), diagnostic::PendingService);
  auto Saved = CPU->saveContext();
  ASSERT_FALSE(bool(Saved));
  EXPECT_EQ(llvm::toString(Saved.takeError()), diagnostic::PendingService);
  EXPECT_EQ(CPU->pendingServiceRequest(), Exit.Service);
  EXPECT_EQ(registers(), Before);
  EXPECT_FALSE(CPU->fault());
}

TEST_P(ServiceBoundary, ConsumptionIsOnceAndDoesNotApplyAnImplicitReturn) {
  auto Exit = request();
  ASSERT_TRUE(Exit.Service);
  const auto Before = registers();
  EXPECT_EQ(CPU->takeServiceRequest(), Exit.Service);
  EXPECT_FALSE(CPU->takeServiceRequest());
  EXPECT_FALSE(CPU->pendingServiceRequest());
  EXPECT_EQ(registers(), Before);
  // Explicit retry at the same PC produces the same request, not a hidden NOP.
  EXPECT_EQ(request().Service, Exit.Service);
  EXPECT_EQ(CPU->takeServiceRequest(), Exit.Service);
  llvm::cantFail(CPU->writeRegister(Result, {Value, 0}));
  BackendHooks H;
  H.Instruction = [&](uint64_t Address, uint32_t) {
    EXPECT_EQ(Address, Exit.Service->NextPC);
    CPU->stop();
  };
  llvm::cantFail(CPU->installHooks(std::move(H)));
  auto Next = llvm::cantFail(CPU->runUntilExit(Exit.Service->NextPC, Timeout));
  EXPECT_EQ(Next.Kind, ExecutionExitKind::Stopped);
  EXPECT_FALSE(Next.Service);
  EXPECT_EQ(llvm::cantFail(CPU->readRegister(PC))[0], nextPC());
}

TEST_P(ServiceBoundary, LegacyRunReportsPendingServiceAsAnError) {
  EXPECT_EQ(llvm::toString(CPU->run(Code, Timeout)),
            diagnostic::PendingService);
  EXPECT_TRUE(CPU->pendingServiceRequest());
  EXPECT_FALSE(CPU->fault());
}

TEST_P(ServiceBoundary, ExplicitReturnRunsFollowingStoreOnRealCPU) {
  if (!GetParam().Backend)
    GTEST_SKIP();
  auto Exit = request();
  ASSERT_TRUE(Exit.Service);
  ASSERT_EQ(CPU->takeServiceRequest(), Exit.Service);
  llvm::cantFail(CPU->writeRegister(Result, {ServiceResult, 0}));
  const auto End =
      nextPC() +
      (GetParam().ISA == GuestArchitecture::X64 ? X64StoreSize : ARMStoreSize);
  BackendHooks H;
  H.Instruction = [&](uint64_t Address, uint32_t) {
    if (Address == End)
      CPU->stop();
  };
  llvm::cantFail(CPU->installHooks(std::move(H)));
  auto Next = llvm::cantFail(CPU->runUntilExit(Exit.Service->NextPC, Timeout));
  ASSERT_EQ(Next.Kind, ExecutionExitKind::Stopped) << Next.Diagnostic;
  EXPECT_FALSE(Next.Service);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, sizeof(uint64_t))),
            ServiceResult);
}

TEST_P(ServiceBoundary, ObserverStopPreventsRequestCapture) {
  BackendHooks H;
  H.Instruction = [&](uint64_t, uint32_t) { CPU->stop(); };
  llvm::cantFail(CPU->installHooks(std::move(H)));
  const auto Exit = request();
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::Stopped);
  EXPECT_FALSE(Exit.Service);
  EXPECT_FALSE(CPU->pendingServiceRequest());
}

TEST_P(ServiceBoundary, ObserverFailureIsTerminalWithoutServiceCapture) {
  BackendHooks H;
  H.Instruction = [](uint64_t, uint32_t) {
    throw std::runtime_error(ObserverFailure);
  };
  llvm::cantFail(CPU->installHooks(std::move(H)));
  const auto Exit = request();
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::BackendFailure);
  EXPECT_FALSE(Exit.Service);
  EXPECT_FALSE(CPU->pendingServiceRequest());
}

TEST_P(ServiceBoundary, FetchProtectionPrecedesServiceAdmission) {
  llvm::cantFail(CPU->protect(Code, PageSize, Read | Execute));
  const auto Exit = request();
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::GuestFault);
  ASSERT_TRUE(Exit.Fault);
  EXPECT_EQ(Exit.Fault->Kind, BackendFaultKind::Protection);
  EXPECT_EQ(Exit.Fault->Access, BackendAccessKind::Execute);
  EXPECT_FALSE(Exit.Service);
  EXPECT_FALSE(CPU->pendingServiceRequest());
}

TEST_P(ServiceBoundary, SupervisorContractsStillRejectServiceInstructions) {
  CPU = llvm::cantFail(create(false));
  llvm::cantFail(CPU->map(Code, PageSize, Read | Write | Execute));
  code(*CPU, ServiceX64, ServiceARM);
  const auto Exit = request();
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation);
  EXPECT_FALSE(Exit.Service);
  EXPECT_FALSE(CPU->pendingServiceRequest());
}

TEST_P(ServiceBoundary, OtherEntryMechanismsRemainUnsupported) {
  code(*CPU, OtherEntryX64, OtherEntryARM);
  const auto Exit = request();
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation);
  EXPECT_FALSE(Exit.Service);
}

TEST_P(ServiceBoundary, SoftwareInterruptsAreNotAliasedToServiceRequests) {
  code(*CPU, SoftwareInterruptX64, SoftwareInterruptARM);
  const auto Exit = request();
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation);
  EXPECT_FALSE(Exit.Service);
}

TEST_P(ServiceBoundary, ISAEncodingBoundariesRemainExplicit) {
  if (GetParam().ISA == GuestArchitecture::X64) {
    code(*CPU, PrefixedX64, {});
    const auto Exit = request();
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation);
    EXPECT_FALSE(Exit.Service);
    return;
  }
#define NEVERD_SERVICE_IMMEDIATE(Value, Word)                                  \
  {                                                                            \
    const uint32_t Instruction[] = {Word};                                     \
    code(*CPU, {}, Instruction);                                               \
    const auto Exit = request();                                               \
    ASSERT_TRUE(Exit.Service);                                                 \
    EXPECT_EQ(Exit.Service->Immediate, Value);                                 \
    EXPECT_EQ(CPU->takeServiceRequest(), Exit.Service);                        \
  }
#include "ServiceRequestCases.def"
#undef NEVERD_SERVICE_IMMEDIATE
}

INSTANTIATE_TEST_SUITE_P(Transports, ServiceBoundary,
                         testing::ValuesIn(Profiles),
                         [](const testing::TestParamInfo<Profile> &P) {
                           return P.param.Name;
                         });

TEST(ServiceRequest, CapabilityInventoryRestrictsServicesToUserProfiles) {
  for (auto ISA : {GuestArchitecture::X64, GuestArchitecture::AArch64}) {
    const bool X64 = ISA == GuestArchitecture::X64;
    const auto User = llvm::cantFail(
        executionCapabilities(X64 ? ExecutionContract::CheckedUserX64
                                  : ExecutionContract::CheckedUserAArch64,
                              ISA));
    const auto Supervisor = llvm::cantFail(executionCapabilities(
        X64 ? ExecutionContract::CheckedX64 : ExecutionContract::CheckedAArch64,
        ISA));
    const auto HasService = [&](const ExecutionCapabilities &C) {
      return llvm::any_of(C.InstructionFamilies, [&](llvm::StringRef Name) {
        return Name == (X64 ? SyscallName : SVCName);
      });
    };
    EXPECT_TRUE(User.supports(ExecutionFeature::ServiceTraps));
    EXPECT_TRUE(HasService(User));
    EXPECT_FALSE(Supervisor.supports(ExecutionFeature::ServiceTraps));
    EXPECT_FALSE(HasService(Supervisor));
  }
}

TEST(ServiceRequest, EventOutranksStopAndDeadlineButNotFailure) {
  ExecutionExitFacts Facts;
  Facts.Service = ServiceRequest{ServiceRequestKind::X64Syscall, Code,
                                 Code + X64ServiceSize};
  Facts.StopRequested = Facts.DeadlineReached = true;
  auto Exit = makeExecutionExit(llvm::Error::success(), Facts);
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::ServiceRequest);
  EXPECT_EQ(Exit.Service, Facts.Service);
  EXPECT_TRUE(Exit.StopRequested);
  EXPECT_TRUE(Exit.DeadlineReached);
  Facts.BackendFailed = true;
  Exit = makeExecutionExit(diagnostic::error(diagnostic::Callback), Facts);
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::BackendFailure);
  EXPECT_FALSE(Exit.Service);
}

TEST(ServiceRequest, BothArchitecturesRetainRequestsWithoutTransportEntry) {
  unsigned Entries = 0;
  for (auto ISA : {GuestArchitecture::X64, GuestArchitecture::AArch64}) {
    auto Memory = llvm::cantFail(MemoryProjection::create(Limit));
    auto CPU = llvm::cantFail(
        ISA == GuestArchitecture::X64
            ? CheckedX64Backend::create(
                  std::move(Memory),
                  std::make_unique<RejectedX64Machine>(Entries), true)
            : CheckedAArch64Backend::create(
                  std::move(Memory),
                  std::make_unique<RejectedARMMachine>(Entries), true));
    llvm::cantFail(
        CPU->map(Code, PageSize, Read | Write | Execute | UserAccessible));
    code(*CPU, ServiceX64, ServiceARM);
    auto Exit = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::ServiceRequest);
    ASSERT_TRUE(Exit.Service);
    EXPECT_EQ(CPU->pendingServiceRequest(), Exit.Service);
    auto Again = CPU->runUntilExit(Code, Timeout);
    ASSERT_FALSE(bool(Again));
    EXPECT_EQ(llvm::toString(Again.takeError()), diagnostic::PendingService);
    EXPECT_EQ(CPU->takeServiceRequest(), Exit.Service);
    EXPECT_EQ(llvm::cantFail(CPU->runUntilExit(Code, Timeout)).Service,
              Exit.Service);
  }
  EXPECT_EQ(Entries, 0u);
}
} // namespace
} // namespace neverd::emulation
