//===- UserExecutionTests.cpp - User CPU and address-space isolation -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "gtest/gtest.h"

#include "neverd/emulation/AddressSpace.h"
#include "neverd/emulation/CPU.h"

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
struct Profile {
  const char *Name;
  ExecutionBackendKind Backend;
  GuestArchitecture ISA;
  ExecutionContract Contract;
};
void PrintTo(const Profile &P, std::ostream *OS) { *OS << P.Name; }
constexpr Profile Profiles[] = {
#define NEVERD_USER_PROFILE(Name, Backend, ISA, Contract)                      \
  {#Name, ExecutionBackendKind::Backend, GuestArchitecture::ISA,               \
   ExecutionContract::Contract},
#include "UserExecutionCases.def"
#undef NEVERD_USER_PROFILE
};
class UserCPU : public testing::TestWithParam<Profile> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  CPURegister Result, Base, PC;
  unsigned Observations = 0, Instructions = 1;
  ExecutionConfiguration configuration() const {
    ExecutionConfiguration C;
    C.Backend = GetParam().Backend;
    C.Architecture = GetParam().ISA;
    C.Contract = GetParam().Contract;
    C.Privilege = ExecutionPrivilege::User;
    C.RequiredFeatures = ExecutionFeature::UserSupervisorIsolation;
    return C;
  }
  void SetUp() override {
    auto B = createExecutionBackend(configuration(), Limit);
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
    Result = X64 ? CPURegister::X64AX : CPURegister::AArch64X0;
    Base = X64 ? CPURegister::X64CX : CPURegister::AArch64X1;
    PC = X64 ? CPURegister::X64PC : CPURegister::AArch64PC;
    ASSERT_EQ(llvm::toString(CPU->map(Code, PageSize,
                                      Read | Write | Execute | UserAccessible)),
              "");
    ASSERT_EQ(llvm::toString(CPU->map(Data, PageSize, Read | Write)), "");
    ASSERT_EQ(llvm::toString(CPU->writeInteger(Data, Value, sizeof(uint64_t))),
              "");
    ASSERT_EQ(llvm::toString(CPU->writeRegister(Base, {Data, 0})), "");
    ASSERT_NO_FATAL_FAILURE(code(LoadX64, LoadARM));
    BackendHooks Hooks;
    Hooks.Instruction = [&](uint64_t, uint32_t) {
      if (Observations++ == Instructions)
        CPU->stop();
    };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
  }
  void code(llvm::ArrayRef<uint8_t> X64, llvm::ArrayRef<uint32_t> ARM) {
    if (GetParam().ISA == GuestArchitecture::X64) {
      ASSERT_EQ(llvm::toString(CPU->write(Code, X64)), "");
      return;
    }
    std::vector<uint8_t> Bytes(ARM.size() * sizeof(uint32_t));
    for (size_t I = 0; I < ARM.size(); ++I)
      llvm::support::endian::write32le(Bytes.data() + I * sizeof(uint32_t),
                                       ARM[I]);
    ASSERT_EQ(llvm::toString(CPU->write(Code, Bytes)), "");
  }
  ExecutionExit run() {
    Observations = 0;
    return llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  }
  void protection(const ExecutionExit &Exit, BackendAccessKind Access,
                  uint64_t Address) {
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::GuestFault) << Exit.Diagnostic;
    ASSERT_TRUE(Exit.Fault);
    EXPECT_EQ(Exit.Fault->Kind, BackendFaultKind::Protection);
    EXPECT_EQ(Exit.Fault->Access, Access);
    EXPECT_EQ(Exit.Fault->Address, Address);
    EXPECT_EQ(llvm::cantFail(CPU->readRegister(PC))[0], Code);
  }
};
TEST_P(UserCPU, ReadsAndWritesUserPagesThroughIndependentAliases) {
  llvm::cantFail(
      CPU->mapAlias(Alias, Data, PageSize, Read | Write | UserAccessible));
  llvm::cantFail(CPU->writeRegister(Base, {Alias, 0}));
  auto Exit = run();
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  EXPECT_EQ(llvm::cantFail(CPU->readRegister(Result))[0], Value);
  ASSERT_NO_FATAL_FAILURE(code(StoreX64, StoreARM));
  llvm::cantFail(CPU->writeRegister(Result, {Updated, 0}));
  Exit = run();
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, sizeof(uint64_t))), Updated);
}
TEST_P(UserCPU, UserStackUsesTheCorrectPrivilegeBank) {
  llvm::cantFail(CPU->map(Stack, PageSize, Read | Write | UserAccessible));
  const bool X64 = GetParam().ISA == GuestArchitecture::X64;
  const auto SP = X64 ? CPURegister::X64SP : CPURegister::AArch64SP;
  const auto Popped = X64 ? CPURegister::X64DX : CPURegister::AArch64X2;
  llvm::cantFail(CPU->writeRegister(SP, {Stack + PageSize, 0}));
  llvm::cantFail(CPU->writeRegister(Result, {Value, 0}));
  ASSERT_NO_FATAL_FAILURE(code(StackX64, StackARM));
  Instructions = 2;
  const auto Exit = run();
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  EXPECT_EQ(llvm::cantFail(CPU->readRegister(Popped))[0], Value);
  EXPECT_EQ(llvm::cantFail(CPU->readRegister(SP))[0], Stack + PageSize);
}
TEST_P(UserCPU, ReadOnlyUserMappingDeniesWrites) {
  llvm::cantFail(CPU->protect(Data, PageSize, Read | UserAccessible));
  ASSERT_NO_FATAL_FAILURE(code(StoreX64, StoreARM));
  llvm::cantFail(CPU->writeRegister(Result, {Updated, 0}));
  protection(run(), BackendAccessKind::Write, Data);
  EXPECT_EQ(
      llvm::cantFail(CPU->addressSpace()->readInteger(Data, sizeof(uint64_t))),
      Value);
}
TEST_P(UserCPU, UserDataMappingDoesNotGrantExecution) {
  llvm::cantFail(CPU->protect(Code, PageSize, Read | Write | UserAccessible));
  protection(run(), BackendAccessKind::Execute, Code);
  EXPECT_EQ(Observations, 0u);
}
TEST_P(UserCPU, SupervisorReadFaultsBeforeEffects) {
  protection(run(), BackendAccessKind::Read, Data);
  EXPECT_EQ(llvm::cantFail(CPU->readRegister(Result))[0], 0u);
}
TEST_P(UserCPU, SupervisorWriteFaultsBeforeEffects) {
  ASSERT_NO_FATAL_FAILURE(code(StoreX64, StoreARM));
  llvm::cantFail(CPU->writeRegister(Result, {Updated, 0}));
  protection(run(), BackendAccessKind::Write, Data);
  EXPECT_EQ(
      llvm::cantFail(CPU->addressSpace()->readInteger(Data, sizeof(uint64_t))),
      Value);
}
TEST_P(UserCPU, SupervisorFetchFaultsBeforeObservation) {
  llvm::cantFail(CPU->protect(Code, PageSize, Read | Write | Execute));
  EXPECT_FALSE(CPU->executable(Code));
  protection(run(), BackendAccessKind::Execute, Code);
  EXPECT_EQ(Observations, 0u);
}
TEST_P(UserCPU, UserMarkerAloneGrantsNoAccess) {
  llvm::cantFail(CPU->protect(Data, PageSize, UserAccessible));
  protection(run(), BackendAccessKind::Read, Data);
}
TEST_P(UserCPU, RevocationIsObservedAfterSuccessfulExecution) {
  llvm::cantFail(CPU->protect(Data, PageSize, Read | UserAccessible));
  auto Exit = run();
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  llvm::cantFail(CPU->protect(Data, PageSize, Read));
  llvm::cantFail(CPU->writeRegister(Result, {Updated, 0}));
  protection(run(), BackendAccessKind::Read, Data);
  EXPECT_EQ(llvm::cantFail(CPU->readRegister(Result))[0], Updated);
}
TEST_P(UserCPU, RecoverableProtectionCanBeResolvedByTheMemoryOwner) {
  BackendHooks Hooks;
  Hooks.RecoverableFault = [](const BackendFault &) { return true; };
  llvm::cantFail(CPU->installHooks(std::move(Hooks)));
  const auto Exit = run();
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault) << Exit.Diagnostic;
  ASSERT_TRUE(CPU->takeRecoverableFault());
  llvm::cantFail(
      CPU->addressSpace()->protect(Data, PageSize, Read | UserAccessible));
  Hooks = {};
  Hooks.Instruction = [&](uint64_t, uint32_t) {
    if (Observations++ == Instructions)
      CPU->stop();
  };
  llvm::cantFail(CPU->installHooks(std::move(Hooks)));
  auto Next = run();
  ASSERT_EQ(Next.Kind, ExecutionExitKind::Stopped) << Next.Diagnostic;
  EXPECT_EQ(llvm::cantFail(CPU->readRegister(Result))[0], Value);
}
TEST_P(UserCPU, AddressSpaceSwitchAndContextRestorePreserveUserExecution) {
  auto First = CPU->addressSpace();
  llvm::cantFail(First->protect(Data, PageSize, Read | UserAccessible));
  auto Context = llvm::cantFail(CPU->saveContext());
  auto Second =
      llvm::cantFail(AddressSpace::create(First->physicalMemory(), Limit));
  llvm::cantFail(
      Second->map(Code, PageSize, Read | Write | Execute | UserAccessible));
  llvm::cantFail(Second->map(Data, PageSize, Read | Write | UserAccessible));
  llvm::cantFail(CPU->bindAddressSpace(Second));
  ASSERT_NO_FATAL_FAILURE(code(LoadX64, LoadARM));
  llvm::cantFail(CPU->writeInteger(Data, Updated, sizeof(uint64_t)));
  auto Exit = run();
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  EXPECT_EQ(llvm::cantFail(CPU->readRegister(Result))[0], Updated);
  llvm::cantFail(CPU->bindAddressSpace(First));
  llvm::cantFail(CPU->restoreContext(*Context));
  Exit = run();
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  EXPECT_EQ(llvm::cantFail(CPU->readRegister(Result))[0], Value);
}
TEST_P(UserCPU, PrivilegedInstructionsStayOutsideTheUserContract) {
  ASSERT_NO_FATAL_FAILURE(code(PrivilegedX64, PrivilegedARM));
  EXPECT_EQ(run().Kind, ExecutionExitKind::UnsupportedOperation);
}
TEST_P(UserCPU, SelectorsCannotEscalatePrivilege) {
  if (GetParam().ISA != GuestArchitecture::X64)
    GTEST_SKIP() << "segment selectors are specific to the x64 guest ISA";
  auto Context = llvm::cantFail(CPU->saveContext());
  const auto CS = llvm::cantFail(CPU->reg(X64Register::CS));
  const auto SS = llvm::cantFail(CPU->reg(X64Register::SS));
  auto E = CPU->setReg(X64Register::CS, 0);
  ASSERT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  E = CPU->setReg(X64Register::SS, 0);
  ASSERT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  llvm::cantFail(CPU->restoreContext(*Context));
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::CS)), CS);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::SS)), SS);
}
TEST(UserIsolation, PortableCPUsShareBytesWithoutSharingPrivilege) {
  for (const auto ISA : {GuestArchitecture::X64, GuestArchitecture::AArch64}) {
    const auto Build = llvm::cantFail(
        queryExecutionBackendBuild(ExecutionBackendKind::Unicorn, ISA));
    if (Build.Availability == BackendAvailability::BuildDisabled)
      GTEST_SKIP() << Build.Reason;
    const bool X64 = ISA == GuestArchitecture::X64;
    const auto Supervisor =
        X64 ? ExecutionContract::CheckedX64 : ExecutionContract::CheckedAArch64;
    const auto User = X64 ? ExecutionContract::CheckedUserX64
                          : ExecutionContract::CheckedUserAArch64;
    auto RAM = llvm::cantFail(PhysicalMemory::create(Limit));
    auto Space = llvm::cantFail(AddressSpace::create(RAM, Limit));
    llvm::cantFail(
        Space->map(Code, PageSize, Read | Write | Execute | UserAccessible));
    llvm::cantFail(Space->map(Data, PageSize, Read | Write));
    llvm::cantFail(Space->writeInteger(Data, Value, sizeof(uint64_t)));
    llvm::cantFail(
        Space->mapAlias(Alias, Data, PageSize, Read | UserAccessible));
    if (X64)
      llvm::cantFail(Space->write(Code, LoadX64));
    else {
      std::vector<uint8_t> Bytes(sizeof(LoadARM));
      for (size_t N = 0; N < std::size(LoadARM); ++N)
        llvm::support::endian::write32le(Bytes.data() + N * sizeof(uint32_t),
                                         LoadARM[N]);
      llvm::cantFail(Space->write(Code, Bytes));
    }
    auto Kernel =
        llvm::cantFail(createExecutionBackend(ExecutionBackendKind::Unicorn,
                                              Supervisor, Space, ISA))
            .CPU;
    auto Application =
        llvm::cantFail(createExecutionBackend(ExecutionBackendKind::Unicorn,
                                              User, Space, ISA))
            .CPU;
    auto Run = [&](ExecutionBackend &CPU, uint64_t Address) {
      llvm::cantFail(CPU.writeRegister(
          X64 ? CPURegister::X64CX : CPURegister::AArch64X1, {Address, 0}));
      unsigned Seen = 0;
      BackendHooks Hooks;
      Hooks.Instruction = [&](uint64_t, uint32_t) {
        if (Seen++)
          CPU.stop();
      };
      llvm::cantFail(CPU.installHooks(std::move(Hooks)));
      return llvm::cantFail(CPU.runUntilExit(Code, Timeout));
    };
    auto Exit = Run(*Kernel, Data);
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    Exit = Run(*Application, Alias);
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    const auto Result = X64 ? CPURegister::X64AX : CPURegister::AArch64X0;
    EXPECT_EQ(llvm::cantFail(Kernel->readRegister(Result)),
              llvm::cantFail(Application->readRegister(Result)));
    EXPECT_EQ(llvm::cantFail(Application->readRegister(Result))[0], Value);
    Exit = Run(*Application, Data);
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::GuestFault) << Exit.Diagnostic;
    ASSERT_TRUE(Exit.Fault);
    EXPECT_EQ(Exit.Fault->Kind, BackendFaultKind::Protection);
    Exit = Run(*Kernel, Data);
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  }
}
INSTANTIATE_TEST_SUITE_P(Transports, UserCPU, testing::ValuesIn(Profiles),
                         [](const testing::TestParamInfo<Profile> &P) {
                           return P.param.Name;
                         });
} // namespace
} // namespace neverd::emulation
