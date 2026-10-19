//===- ResourcePauseTests.cpp - Execute repaired continuations ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Execute repaired continuations.
///

#include "gtest/gtest.h"

#include "neverd/emulation/AddressSpace.h"
#include "neverd/emulation/ExecutionSession.h"

#include <stdexcept>

using namespace neverd::emulation;
namespace {
constexpr uint64_t Page = 4096, Code = 0x10000, Data = 0x30000;
constexpr uint64_t Missing = 0x50000, Output = 0x60000, Stack = 0x80000;
constexpr uint64_t SP = Stack + Page - 128;
class ResourceCPU : public testing::TestWithParam<ExecutionContract> {
protected:
  std::shared_ptr<AddressSpace> Space;
  std::shared_ptr<ExecutionBudget> Budget;
  std::unique_ptr<ExecutionSession> Session;
  std::vector<ResourcePause> Observed;
  std::function<bool(const ResourcePause &)> Policy;
  std::function<void(uint64_t)> Observer;
  std::function<bool(const BackendFault &)> Exception;
  void SetUp() override {
    const auto Available = llvm::cantFail(queryExecutionBackendBuild(
        ExecutionBackendKind::Unicorn, GuestArchitecture::X64));
    if (Available.Availability != BackendAvailability::Available)
      GTEST_SKIP() << Available.Reason;
    Space = llvm::cantFail(AddressSpace::create(
        llvm::cantFail(PhysicalMemory::create(32 * Page)), 32 * Page));
    Budget = llvm::cantFail(ExecutionBudget::create({10000, 1000, 10000000}));
    Policy = [](const ResourcePause &) { return true; };
  }
  void start(llvm::ArrayRef<uint8_t> Bytes, uint64_t StopAt,
             bool ResourcePolicy = true) {
    ASSERT_EQ(llvm::toString(Space->map(Code, Page, Read | Write | Execute)),
              "");
    ASSERT_EQ(llvm::toString(Space->write(Code, Bytes)), "");
    ASSERT_EQ(llvm::toString(Space->map(Stack, Page, Read | Write)), "");
    auto Selected = createExecutionBackend(ExecutionBackendKind::Unicorn,
                                           GetParam(), Space);
    ASSERT_TRUE(bool(Selected)) << llvm::toString(Selected.takeError());
    ASSERT_EQ(llvm::toString(Selected->CPU->setReg(X64Register::SP, SP)), "");
    auto Created = ExecutionSession::create(
        std::move(Selected->CPU), Budget, Exception,
        [this](uint64_t PC, uint32_t) {
          if (Observer)
            Observer(PC);
        },
        ResourcePolicy ? std::function<bool(const ResourcePause &)>(
                             [this](const ResourcePause &P) {
                               Observed.push_back(P);
                               return Policy(P);
                             })
                       : std::function<bool(const ResourcePause &)>());
    ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
    Session = std::move(*Created);
    ASSERT_EQ(llvm::toString(Session->watchExecution({{StopAt, 1}})), "");
  }
  void map(uint64_t At, llvm::ArrayRef<uint8_t> Input,
           unsigned Permissions = Read | Write) {
    std::vector<uint8_t> Bytes(Page, 0);
    std::copy(Input.begin(), Input.end(), Bytes.begin());
    auto Transaction = Space->prepareMappings({{At, Permissions, Bytes}});
    ASSERT_TRUE(bool(Transaction)) << llvm::toString(Transaction.takeError());
    ASSERT_EQ(llvm::toString((*Transaction)->commit()), "");
  }
  void resumed(uint64_t PC) {
    ASSERT_EQ(llvm::toString(Session->acknowledgeResourcePause()), "");
    auto Exit = Session->run(PC, 10000);
    ASSERT_TRUE(bool(Exit)) << llvm::toString(Exit.takeError());
    EXPECT_EQ(Exit->Kind, SessionExitKind::ExecutionWatch);
    ASSERT_TRUE(Exit->CPU);
    EXPECT_FALSE(Session->cpu().fault());
  }
};
TEST_P(ResourceCPU, ScalarReadRetainsStateAndRequiresMappingAndExactPC) {
  const uint8_t Bytes[] = {0x48, 0x8B, 0x03, 0x90};
  start(Bytes, Code + 3);
  ASSERT_TRUE(Session);
  ASSERT_EQ(llvm::toString(Session->cpu().setReg(X64Register::BX, Missing)),
            "");
  ASSERT_EQ(llvm::toString(Session->cpu().setReg(X64Register::AX, 73)), "");
  auto Exit = Session->run(Code, 1000);
  ASSERT_TRUE(bool(Exit));
  ASSERT_TRUE(Exit->CPU);
  ASSERT_EQ(Exit->CPU->Kind, ExecutionExitKind::ResourcePause)
      << Exit->CPU->Diagnostic;
  ASSERT_TRUE(Exit->CPU->Resource);
  EXPECT_EQ(Exit->CPU->Resource->ResumePC, Code);
  EXPECT_EQ(Exit->CPU->Resource->RequiredPermissions, Read);
  EXPECT_EQ(*Session->cpu().reg(X64Register::AX), 73u);
  EXPECT_EQ(*Session->cpu().reg(X64Register::SP), SP);
  EXPECT_EQ(Budget->instructions(), 1u);
  EXPECT_FALSE(Session->cpu().fault());
  EXPECT_NE(llvm::toString(Session->acknowledgeResourcePause()), "");
  EXPECT_NE(llvm::toString(Session->cpu().setReg(X64Register::AX, 99)), "");
  auto Snapshot = Session->cpu().saveContext();
  ASSERT_FALSE(bool(Snapshot));
  llvm::consumeError(Snapshot.takeError());
  auto InvalidRun = Session->run(Code, 10);
  ASSERT_FALSE(bool(InvalidRun));
  llvm::consumeError(InvalidRun.takeError());
  uint8_t Value = 0;
  EXPECT_NE(llvm::toString(Session->cpu().read(Missing, Value)), "");
  EXPECT_FALSE(Session->cpu().fault());
  const uint8_t Input[] = {42};
  map(Missing, Input, Read);
  ASSERT_EQ(llvm::toString(Session->acknowledgeResourcePause()), "");
  EXPECT_NE(llvm::toString(Session->acknowledgeResourcePause()), "");
  EXPECT_NE(llvm::toString(Session->cpu().setReg(X64Register::AX, 99)), "");
  InvalidRun = Session->run(Code + 1, 10);
  ASSERT_FALSE(bool(InvalidRun));
  llvm::consumeError(InvalidRun.takeError());
  Exit = Session->run(Code, 1000);
  ASSERT_TRUE(bool(Exit));
  EXPECT_EQ(Exit->Kind, SessionExitKind::ExecutionWatch);
  EXPECT_EQ(*Session->cpu().reg(X64Register::AX), 42u);
  EXPECT_EQ(Budget->instructions(), 2u);
  EXPECT_EQ(*Session->cpu().reg(X64Register::SP), SP);
}
TEST_P(ResourceCPU, CrossPageReadPreservesExistingPrefix) {
  const uint8_t Bytes[] = {0x48, 0x8B, 0x03, 0x90};
  start(Bytes, Code + 3);
  ASSERT_TRUE(Session);
  map(Data, {});
  ASSERT_EQ(llvm::toString(Space->writeInteger(Data + Page - 4, 0x44332211, 4)),
            "");
  ASSERT_EQ(
      llvm::toString(Session->cpu().setReg(X64Register::BX, Data + Page - 4)),
      "");
  auto Exit = Session->run(Code, 1000);
  ASSERT_TRUE(bool(Exit));
  ASSERT_EQ(Exit->CPU->Kind, ExecutionExitKind::ResourcePause)
      << Exit->CPU->Diagnostic;
  EXPECT_EQ(*Space->readInteger(Data + Page - 4, 4), 0x44332211u);
  const uint8_t Suffix[] = {0x55, 0x66, 0x77, 0x88};
  map(Data + Page, Suffix, Read);
  resumed(Exit->CPU->Resource->ResumePC);
  EXPECT_EQ(*Session->cpu().reg(X64Register::AX), 0x8877665544332211ULL);
}
TEST_P(ResourceCPU, InitialFetchPausesBeforeAnyInstructionAdmission) {
  const uint8_t Bytes[] = {0x90};
  start(Bytes, Missing + 5);
  ASSERT_TRUE(Session);
  auto Exit = Session->run(Missing, 1000);
  ASSERT_TRUE(bool(Exit));
  ASSERT_EQ(Exit->CPU->Kind, ExecutionExitKind::ResourcePause)
      << Exit->CPU->Diagnostic;
  EXPECT_EQ(Exit->CPU->Resource->ResumePC, Missing);
  EXPECT_EQ(Budget->instructions(), 0u);
  const uint8_t NewCode[] = {0xB8, 42, 0, 0, 0, 0x90};
  map(Missing, NewCode, Read | Execute);
  resumed(Missing);
  EXPECT_EQ(*Session->cpu().reg(X64Register::AX), 42u);
}
TEST_P(ResourceCPU, CallTargetFetchResumesAfterTheReturnAddressWasPushed) {
  const uint8_t Bytes[] = {0xFF, 0xD0, 0x90};
  start(Bytes, Code + 2);
  ASSERT_TRUE(Session);
  ASSERT_EQ(llvm::toString(Session->cpu().setReg(X64Register::AX, Missing)),
            "");
  auto Exit = Session->run(Code, 1000);
  ASSERT_TRUE(bool(Exit));
  ASSERT_EQ(Exit->CPU->Kind, ExecutionExitKind::ResourcePause)
      << Exit->CPU->Diagnostic;
  ASSERT_TRUE(Exit->CPU->Resource);
  EXPECT_EQ(Exit->CPU->Resource->Restart, ResourceRestartKind::FetchBoundary);
  EXPECT_EQ(Exit->CPU->Resource->ResumePC, Missing);
  EXPECT_EQ(*Session->cpu().reg(X64Register::SP), SP - 8);
  EXPECT_EQ(*Space->readInteger(SP - 8, 8), Code + 2);
  const uint8_t Return[] = {0xC3};
  map(Missing, Return, Read | Execute);
  resumed(Missing);
  EXPECT_EQ(*Session->cpu().reg(X64Register::SP), SP);
  EXPECT_EQ(*Space->readInteger(SP - 8, 8), Code + 2);
}
TEST_P(ResourceCPU, InstructionFetchAcrossPageResumesAtTheInstructionStart) {
  const uint8_t Bytes[] = {0x90};
  const uint64_t Entry = Code + Page - 2;
  start(Bytes, Entry + 10);
  ASSERT_TRUE(Session);
  const uint8_t Prefix[] = {0x48, 0xB8};
  ASSERT_EQ(llvm::toString(Space->write(Entry, Prefix)), "");
  auto Exit = Session->run(Entry, 1000);
  ASSERT_TRUE(bool(Exit));
  ASSERT_EQ(Exit->CPU->Kind, ExecutionExitKind::ResourcePause)
      << Exit->CPU->Diagnostic;
  EXPECT_EQ(Exit->CPU->Resource->ResumePC, Entry);
  const uint8_t Suffix[] = {42, 0, 0, 0, 0, 0, 0, 0, 0x90};
  map(Code + Page, Suffix, Read | Execute);
  resumed(Entry);
  EXPECT_EQ(*Session->cpu().reg(X64Register::AX), 42u);
}
TEST_P(ResourceCPU, RepSourceFaultPreservesCompletedIterations) {
  const uint8_t Bytes[] = {0xF3, 0xA4, 0x90};
  start(Bytes, Code + 2);
  ASSERT_TRUE(Session);
  map(Data, {});
  map(Output, {});
  ASSERT_EQ(llvm::toString(Space->writeInteger(Data + Page - 2, 0xBBAA, 2)),
            "");
  ASSERT_EQ(
      llvm::toString(Session->cpu().setReg(X64Register::SI, Data + Page - 2)),
      "");
  ASSERT_EQ(llvm::toString(Session->cpu().setReg(X64Register::DI, Output)), "");
  ASSERT_EQ(llvm::toString(Session->cpu().setReg(X64Register::CX, 4)), "");
  auto Exit = Session->run(Code, 1000);
  ASSERT_TRUE(bool(Exit));
  ASSERT_EQ(Exit->CPU->Kind, ExecutionExitKind::ResourcePause)
      << Exit->CPU->Diagnostic;
  EXPECT_EQ(*Space->readInteger(Output, 4), 0xBBAAu);
  EXPECT_EQ(*Session->cpu().reg(X64Register::CX), 2u);
  EXPECT_EQ(*Session->cpu().reg(X64Register::SI), Data + Page);
  EXPECT_EQ(*Session->cpu().reg(X64Register::DI), Output + 2);
  // Changing completed input proves resume does not re-copy those iterations.
  ASSERT_EQ(llvm::toString(Space->writeInteger(Data + Page - 2, 0xEEEE, 2)),
            "");
  const uint8_t Rest[] = {0xCC, 0xDD};
  map(Data + Page, Rest);
  resumed(Code);
  EXPECT_EQ(*Space->readInteger(Output, 4), 0xDDCCBBAAu);
  EXPECT_EQ(*Session->cpu().reg(X64Register::CX), 0u);
  EXPECT_EQ(*Session->cpu().reg(X64Register::DI), Output + 4);
}
TEST_P(ResourceCPU, RepDestinationFaultPreservesCompletedStores) {
  const uint8_t Bytes[] = {0xF3, 0xA4, 0x90};
  start(Bytes, Code + 2);
  ASSERT_TRUE(Session);
  const uint8_t Source[] = {0xAA, 0xBB, 0xCC, 0xDD};
  map(Data, Source);
  map(Output, {});
  ASSERT_EQ(llvm::toString(Session->cpu().setReg(X64Register::SI, Data)), "");
  ASSERT_EQ(
      llvm::toString(Session->cpu().setReg(X64Register::DI, Output + Page - 2)),
      "");
  ASSERT_EQ(llvm::toString(Session->cpu().setReg(X64Register::CX, 4)), "");
  auto Exit = Session->run(Code, 1000);
  ASSERT_TRUE(bool(Exit));
  ASSERT_EQ(Exit->CPU->Kind, ExecutionExitKind::ResourcePause)
      << Exit->CPU->Diagnostic;
  EXPECT_EQ(*Space->readInteger(Output + Page - 2, 2), 0xBBAAu);
  EXPECT_EQ(*Session->cpu().reg(X64Register::CX), 2u);
  EXPECT_EQ(*Session->cpu().reg(X64Register::DI), Output + Page);
  ASSERT_EQ(llvm::toString(Space->writeInteger(Data, 0xEEEE, 2)), "");
  map(Output + Page, {});
  resumed(Code);
  EXPECT_EQ(*Space->readInteger(Output + Page - 2, 4), 0xDDCCBBAAu);
  EXPECT_EQ(*Session->cpu().reg(X64Register::CX), 0u);
}
TEST_P(ResourceCPU, UnverifiedSoftwareStoresRemainTerminal) {
  const uint8_t Bytes[] = {0x48, 0x89, 0x03, 0x90};
  start(Bytes, Code + 3);
  ASSERT_TRUE(Session);
  ASSERT_EQ(llvm::toString(Session->cpu().setReg(X64Register::BX, Missing)),
            "");
  ASSERT_EQ(llvm::toString(Session->cpu().setReg(X64Register::AX, 42)), "");
  auto Exit = Session->run(Code, 1000);
  ASSERT_TRUE(bool(Exit));
  if (GetParam() == ExecutionContract::Software) {
    EXPECT_EQ(Exit->CPU->Kind, ExecutionExitKind::GuestFault);
    EXPECT_FALSE(Session->cpu().pendingResourcePause());
    EXPECT_NE(llvm::toString(Session->acknowledgeResourcePause()), "");
    EXPECT_TRUE(Session->cpu().fault());
  } else {
    ASSERT_EQ(Exit->CPU->Kind, ExecutionExitKind::ResourcePause);
    map(Missing, {});
    resumed(Code);
    EXPECT_EQ(*Space->readInteger(Missing, 8), 42u);
  }
}
TEST_P(ResourceCPU, LegacyPolicyDoesNotReclassifyTerminalFaults) {
  const uint8_t Bytes[] = {0x48, 0x8B, 0x03, 0x90};
  start(Bytes, Code + 3, false);
  ASSERT_TRUE(Session);
  ASSERT_EQ(llvm::toString(Session->cpu().setReg(X64Register::BX, Missing)),
            "");
  auto Exit = Session->run(Code, 1000);
  ASSERT_TRUE(bool(Exit));
  EXPECT_EQ(Exit->CPU->Kind, ExecutionExitKind::GuestFault);
  EXPECT_FALSE(Session->cpu().pendingResourcePause());
  EXPECT_TRUE(Observed.empty());
  auto Again = Session->run(Code, 1000);
  EXPECT_FALSE(bool(Again));
  if (!Again)
    llvm::consumeError(Again.takeError());
}
TEST_P(ResourceCPU, ProtectionAndModelErrorsRemainTerminal) {
  const uint8_t Bytes[] = {0x48, 0x89, 0x03, 0x90};
  start(Bytes, Code + 3);
  ASSERT_TRUE(Session);
  map(Missing, {}, Read);
  ASSERT_EQ(llvm::toString(Session->cpu().setReg(X64Register::BX, Missing)),
            "");
  auto Exit = Session->run(Code, 1000);
  ASSERT_TRUE(bool(Exit));
  EXPECT_EQ(Exit->CPU->Kind, ExecutionExitKind::GuestFault);
  EXPECT_EQ(Session->cpu().fault()->Kind, BackendFaultKind::Protection);
  EXPECT_FALSE(Session->cpu().pendingResourcePause());
  EXPECT_TRUE(Observed.empty());
}
TEST_P(ResourceCPU, MissingMemoryFromAModelCallbackIsNotAResourcePause) {
  Observer = [&](uint64_t) {
    uint8_t Byte;
    llvm::consumeError(Session->cpu().read(Missing, Byte));
  };
  const uint8_t Bytes[] = {0x90, 0x90};
  start(Bytes, Code + 1);
  ASSERT_TRUE(Session);
  auto Exit = Session->run(Code, 1000);
  ASSERT_TRUE(bool(Exit));
  EXPECT_EQ(Exit->CPU->Kind, ExecutionExitKind::GuestFault);
  EXPECT_TRUE(Observed.empty());
  EXPECT_FALSE(Session->cpu().pendingResourcePause());
}
TEST_P(ResourceCPU, RejectedResourcePolicyPreservesOSExceptionTransfer) {
  Policy = [](const ResourcePause &) { return false; };
  Exception = [](const BackendFault &F) {
    return F.Kind == BackendFaultKind::UnmappedMemory;
  };
  const uint8_t Bytes[] = {0x48, 0x8B, 0x03, 0x90};
  start(Bytes, Code + 3);
  ASSERT_TRUE(Session);
  ASSERT_EQ(llvm::toString(Session->cpu().setReg(X64Register::BX, Missing)),
            "");
  auto Exit = Session->run(Code, 1000);
  ASSERT_TRUE(bool(Exit));
  EXPECT_EQ(Exit->CPU->Kind, ExecutionExitKind::RecoverableFault);
  EXPECT_FALSE(Session->cpu().pendingResourcePause());
  EXPECT_NE(llvm::toString(Session->acknowledgeResourcePause()), "");
  auto Fault = Session->takeRecoverableFault();
  ASSERT_TRUE(bool(Fault));
  EXPECT_EQ(Fault->Address, Missing);
}
TEST_P(ResourceCPU, ResourcePolicyExceptionCannotProduceAResumablePause) {
  Policy = [](const ResourcePause &) -> bool {
    throw std::runtime_error("policy failure");
  };
  const uint8_t Bytes[] = {0x48, 0x8B, 0x03, 0x90};
  start(Bytes, Code + 3);
  ASSERT_TRUE(Session);
  ASSERT_EQ(llvm::toString(Session->cpu().setReg(X64Register::BX, Missing)),
            "");
  auto Exit = Session->run(Code, 1000);
  ASSERT_TRUE(bool(Exit));
  EXPECT_NE(Exit->CPU->Kind, ExecutionExitKind::ResourcePause);
  EXPECT_TRUE(Session->cpu().fault());
  EXPECT_FALSE(Session->cpu().pendingResourcePause());
}
TEST_P(ResourceCPU, EnterRetainsItsCommittedStorePrefixAtAnArchitecturalRetry) {
  const uint8_t Bytes[] = {0xC8, 0, 0, 2, 0x90};
  start(Bytes, Code + 4);
  ASSERT_TRUE(Session);
  ASSERT_EQ(llvm::toString(Session->cpu().setReg(X64Register::BP, Missing + 8)),
            "");
  auto Exit = Session->run(Code, 1000);
  ASSERT_TRUE(bool(Exit));
  if (GetParam() == ExecutionContract::Software) {
    EXPECT_EQ(Exit->CPU->Kind, ExecutionExitKind::GuestFault);
    EXPECT_FALSE(Session->cpu().pendingResourcePause());
    return;
  }
  ASSERT_EQ(Exit->CPU->Kind, ExecutionExitKind::ResourcePause);
  EXPECT_TRUE(Exit->CPU->Resource->MemoryEffectsMayExist);
  EXPECT_EQ(Exit->CPU->Resource->Restart,
            ResourceRestartKind::ArchitecturalRetry);
  EXPECT_EQ(*Space->readInteger(SP - 8, 8), Missing + 8);
  EXPECT_EQ(*Session->cpu().reg(X64Register::SP), SP);
  EXPECT_EQ(*Session->cpu().reg(X64Register::BP), Missing + 8);
  const uint8_t Input[] = {0x22, 0x11};
  map(Missing, Input);
  resumed(Code);
  EXPECT_EQ(*Session->cpu().reg(X64Register::SP), SP - 24);
  EXPECT_EQ(*Session->cpu().reg(X64Register::BP), SP - 8);
  EXPECT_EQ(*Space->readInteger(SP - 16, 8), 0x1122u);
  EXPECT_EQ(*Space->readInteger(SP - 24, 8), SP - 8);
}
TEST_P(ResourceCPU, ReverseRepResumesWithItsDirectionAndCompletedProgress) {
  const uint8_t Bytes[] = {0xF3, 0xA4, 0x90};
  start(Bytes, Code + 2);
  ASSERT_TRUE(Session);
  const uint8_t Input[] = {0xAA, 0xBB};
  map(Data, Input);
  map(Output, {});
  ASSERT_EQ(llvm::toString(Session->cpu().setReg(X64Register::SI, Data + 1)),
            "");
  ASSERT_EQ(llvm::toString(Session->cpu().setReg(X64Register::DI, Output + 3)),
            "");
  ASSERT_EQ(llvm::toString(Session->cpu().setReg(X64Register::CX, 4)), "");
  ASSERT_EQ(llvm::toString(Session->cpu().setReg(X64Register::FLAGS, 0x402)),
            "");
  auto Exit = Session->run(Code, 1000);
  ASSERT_TRUE(bool(Exit));
  ASSERT_EQ(Exit->CPU->Kind, ExecutionExitKind::ResourcePause);
  EXPECT_EQ(*Session->cpu().reg(X64Register::CX), 2u);
  EXPECT_EQ(*Session->cpu().reg(X64Register::SI), Data - 1);
  EXPECT_EQ(*Session->cpu().reg(X64Register::DI), Output + 1);
  ASSERT_EQ(llvm::toString(Space->writeInteger(Data, 0xEEEE, 2)), "");
  std::vector<uint8_t> Rest(Page, 0);
  Rest[Page - 1] = 0xCC;
  Rest[Page - 2] = 0xDD;
  map(Data - Page, Rest);
  resumed(Code);
  EXPECT_EQ(*Space->readInteger(Output, 4), 0xBBAACCDDu);
  EXPECT_EQ(*Session->cpu().reg(X64Register::CX), 0u);
  EXPECT_EQ(*Session->cpu().reg(X64Register::FLAGS) & 0x400u, 0x400u);
}
TEST_P(ResourceCPU, ThreadBaseAndVectorStateSurviveTheResourceBoundary) {
  const uint8_t Bytes[] = {0x48, 0x8B, 0x03, 0x90};
  start(Bytes, Code + 3);
  ASSERT_TRUE(Session);
  ASSERT_EQ(llvm::toString(Session->cpu().setReg(X64Register::BX, Missing)),
            "");
  ASSERT_EQ(llvm::toString(Session->cpu().setGSBase(0x90000)), "");
  ExecutionBackend::XmmValue Vector{0x1122334455667788ULL,
                                    0x8877665544332211ULL};
  ASSERT_EQ(llvm::toString(Session->cpu().setXmm(0, Vector)), "");
  auto Exit = Session->run(Code, 1000);
  ASSERT_TRUE(bool(Exit));
  ASSERT_EQ(Exit->CPU->Kind, ExecutionExitKind::ResourcePause);
  EXPECT_EQ(*Session->cpu().reg(X64Register::GSBase), 0x90000u);
  EXPECT_EQ(*Session->cpu().xmm(0), Vector);
  const uint8_t Input[] = {42};
  map(Missing, Input);
  resumed(Code);
  EXPECT_EQ(*Session->cpu().reg(X64Register::BX), Missing);
  EXPECT_EQ(*Session->cpu().reg(X64Register::GSBase), 0x90000u);
  EXPECT_EQ(*Session->cpu().xmm(0), Vector);
}
TEST_P(ResourceCPU, DeviceModelFailureCannotBecomeAResourceRetry) {
  const uint8_t Bytes[] = {0x8B, 0x03, 0x90};
  start(Bytes, Code + 2);
  ASSERT_TRUE(Session);
  unsigned Reads = 0;
  GuestMMIOCallbacks Device;
  Device.Validate = [](uint64_t, uint64_t, bool) -> llvm::Error {
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "rejected model access");
  };
  Device.Read = [&](uint64_t, unsigned) -> llvm::Expected<uint64_t> {
    ++Reads;
    return 42;
  };
  Device.Write = [](uint64_t, unsigned, uint64_t) {
    return llvm::Error::success();
  };
  ASSERT_EQ(
      llvm::toString(Session->cpu().mapMMIO(Missing, Page, std::move(Device))),
      "");
  ASSERT_EQ(llvm::toString(Session->cpu().setReg(X64Register::BX, Missing)),
            "");
  auto Exit = Session->run(Code, 1000);
  ASSERT_TRUE(bool(Exit));
  EXPECT_EQ(Exit->CPU->Kind, ExecutionExitKind::DeviceFailure);
  EXPECT_FALSE(Session->cpu().pendingResourcePause());
  EXPECT_TRUE(Observed.empty());
  EXPECT_EQ(Reads, 0u);
  EXPECT_NE(llvm::toString(Session->acknowledgeResourcePause()), "");
}
TEST_P(ResourceCPU,
       CrossPageLoadWithAnAliasedDestinationKeepsItsAddressRegister) {
  const uint8_t Bytes[] = {0x48, 0x8B, 0x00, 0x90};
  start(Bytes, Code + 3);
  ASSERT_TRUE(Session);
  map(Data, {});
  ASSERT_EQ(llvm::toString(Space->writeInteger(Data + Page - 4, 0x44332211, 4)),
            "");
  ASSERT_EQ(
      llvm::toString(Session->cpu().setReg(X64Register::AX, Data + Page - 4)),
      "");
  auto Exit = Session->run(Code, 1000);
  ASSERT_TRUE(bool(Exit));
  ASSERT_EQ(Exit->CPU->Kind, ExecutionExitKind::ResourcePause);
  EXPECT_EQ(*Session->cpu().reg(X64Register::AX), Data + Page - 4);
  const uint8_t Rest[] = {0x55, 0x66, 0x77, 0x88};
  map(Data + Page, Rest);
  resumed(Code);
  EXPECT_EQ(*Session->cpu().reg(X64Register::AX), 0x8877665544332211ULL);
}
TEST_P(ResourceCPU, SessionChargesActiveWorkWithoutChargingStoppedInspection) {
  auto Active = ExecutionBudget::createWithTimePolicy(
      {10000, 1000, 10000000}, ExecutionTimePolicy::ActiveWork);
  ASSERT_TRUE(bool(Active));
  Budget = std::move(*Active);
  const uint8_t Bytes[] = {0x48, 0x8B, 0x03, 0x90};
  start(Bytes, Code + 3);
  ASSERT_TRUE(Session);
  ASSERT_EQ(llvm::toString(Session->cpu().setReg(X64Register::BX, Missing)),
            "");
  auto Exit = Session->run(Code, 1000);
  ASSERT_TRUE(bool(Exit));
  ASSERT_EQ(Exit->CPU->Kind, ExecutionExitKind::ResourcePause);
  const auto Remaining = Budget->remainingMicroseconds();
  EXPECT_LT(Remaining, Budget->limits().TimeoutMicroseconds);
  EXPECT_GT(Remaining, 0u);
  EXPECT_EQ(
      Budget->remainingMicroseconds(ExecutionBudget::Clock::time_point::max()),
      Remaining);
  const uint8_t Input[] = {42};
  map(Missing, Input);
  EXPECT_EQ(Budget->remainingMicroseconds(), Remaining);
  resumed(Code);
  EXPECT_LE(Budget->remainingMicroseconds(), Remaining);
  EXPECT_EQ(Budget->instructions(), 2u);
}
TEST_P(ResourceCPU, ScalarLoadWidthsAndExtensionsRetainTheirFaultBoundary) {
  struct LoadCase {
    std::vector<uint8_t> CodeBytes;
    uint64_t Expected;
  };
  const LoadCase Cases[] = {{{0x0F, 0xB6, 0x03}, 0x80},
                            {{0x48, 0x0F, 0xBE, 0x03}, 0xFFFFFFFFFFFFFF80ULL},
                            {{0x48, 0x63, 0x03}, 0xFFFFFFFFFFFFFF80ULL},
                            {{0x8B, 0x03}, 0xFFFFFF80ULL}};
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.CodeBytes.size());
    Session.reset();
    Space = llvm::cantFail(AddressSpace::create(
        llvm::cantFail(PhysicalMemory::create(32 * Page)), 32 * Page));
    auto Instructions = C.CodeBytes;
    Instructions.push_back(0x90);
    start(Instructions, Code + C.CodeBytes.size());
    ASSERT_TRUE(Session);
    ASSERT_EQ(llvm::toString(Session->cpu().setReg(X64Register::BX, Missing)),
              "");
    ASSERT_EQ(llvm::toString(Session->cpu().setReg(X64Register::AX,
                                                   0x1122334455667788ULL)),
              "");
    auto Exit = Session->run(Code, 1000);
    ASSERT_TRUE(bool(Exit));
    ASSERT_EQ(Exit->CPU->Kind, ExecutionExitKind::ResourcePause);
    EXPECT_EQ(*Session->cpu().reg(X64Register::AX), 0x1122334455667788ULL);
    const uint8_t Input[] = {0x80, 0xFF, 0xFF, 0xFF};
    map(Missing, Input);
    resumed(Code);
    EXPECT_EQ(*Session->cpu().reg(X64Register::AX), C.Expected);
  }
}
TEST_P(ResourceCPU, WatchStepOverSurvivesAResourcePause) {
  const uint8_t Bytes[] = {0x48, 0x8B, 0x03, 0x90};
  start(Bytes, Code);
  ASSERT_TRUE(Session);
  ASSERT_EQ(llvm::toString(Session->watchExecution({{Code, 1}, {Code + 3, 1}})),
            "");
  ASSERT_EQ(llvm::toString(Session->cpu().setReg(X64Register::BX, Missing)),
            "");
  auto Exit = Session->run(Code, 1000);
  ASSERT_TRUE(bool(Exit));
  ASSERT_EQ(Exit->Kind, SessionExitKind::ExecutionWatch);
  Exit = Session->run(Code, 1000);
  ASSERT_TRUE(bool(Exit));
  ASSERT_EQ(Exit->CPU->Kind, ExecutionExitKind::ResourcePause);
  const uint8_t Input[] = {42};
  map(Missing, Input);
  resumed(Code);
  EXPECT_EQ(*Session->cpu().reg(X64Register::PC), Code + 3);
  EXPECT_EQ(*Session->cpu().reg(X64Register::AX), 42u);
}
TEST_P(ResourceCPU, FetchRepairPreservesANewTargetWatch) {
  const uint8_t Bytes[] = {0x90};
  start(Bytes, Missing);
  ASSERT_TRUE(Session);
  ASSERT_EQ(llvm::toString(Session->cpu().setReg(X64Register::AX, 73)), "");
  auto Exit = Session->run(Missing, 1000);
  ASSERT_TRUE(bool(Exit));
  ASSERT_EQ(Exit->CPU->Kind, ExecutionExitKind::ResourcePause);
  const uint8_t Target[] = {0xB8, 42, 0, 0, 0, 0x90};
  map(Missing, Target, Read | Execute);
  resumed(Missing);
  EXPECT_EQ(*Session->cpu().reg(X64Register::PC), Missing);
  EXPECT_EQ(*Session->cpu().reg(X64Register::AX), 73u);
  EXPECT_EQ(Budget->instructions(), 0u);
}
TEST(ResourcePauseArchitecture, UntypedRunCannotReportAResourcePauseAsSuccess) {
  auto Available = queryExecutionBackendBuild(ExecutionBackendKind::Unicorn,
                                              GuestArchitecture::X64);
  ASSERT_TRUE(bool(Available));
  if (Available->Availability != BackendAvailability::Available)
    GTEST_SKIP() << Available->Reason;
  auto Selected = createExecutionBackend(
      ExecutionBackendKind::Unicorn, ExecutionContract::Software, 32 * Page);
  ASSERT_TRUE(bool(Selected));
  auto &CPU = *Selected->CPU;
  ASSERT_EQ(llvm::toString(CPU.map(Code, Page, Read | Write | Execute)), "");
  const uint8_t Bytes[] = {0x48, 0x8B, 0x03, 0x90};
  ASSERT_EQ(llvm::toString(CPU.write(Code, Bytes)), "");
  ASSERT_EQ(llvm::toString(CPU.setReg(X64Register::BX, Missing)), "");
  BackendHooks Hooks;
  Hooks.ResourcePause = [](const ResourcePause &) { return true; };
  Hooks.Instruction = [&](uint64_t PC, uint32_t) {
    if (PC == Code + 3)
      CPU.stop();
  };
  ASSERT_EQ(llvm::toString(CPU.installHooks(std::move(Hooks))), "");
  EXPECT_NE(llvm::toString(CPU.run(Code, 1000000)), "");
  EXPECT_TRUE(CPU.pendingResourcePause());
  EXPECT_FALSE(CPU.fault());
  EXPECT_NE(llvm::toString(CPU.run(Code, 1000000)), "");
  auto Space = CPU.addressSpace();
  ASSERT_EQ(llvm::toString(Space->map(Missing, Page, Read | Write)), "");
  ASSERT_EQ(llvm::toString(Space->writeInteger(Missing, 42, 8)), "");
  ASSERT_EQ(llvm::toString(CPU.acknowledgeResourcePause()), "");
  ASSERT_EQ(llvm::toString(CPU.run(Code, 1000000)), "");
  EXPECT_EQ(*CPU.reg(X64Register::AX), 42u);
  EXPECT_FALSE(CPU.pendingResourcePause());
}
TEST(ResourcePauseArchitecture, CheckedARM64UsesItsArchitecturalLoadBoundary) {
  auto Available = queryExecutionBackendBuild(ExecutionBackendKind::Unicorn,
                                              GuestArchitecture::AArch64);
  ASSERT_TRUE(bool(Available));
  if (Available->Availability != BackendAvailability::Available)
    GTEST_SKIP() << Available->Reason;
  auto Selected = createExecutionBackend(ExecutionBackendKind::Unicorn,
                                         ExecutionContract::CheckedAArch64,
                                         32 * Page, GuestArchitecture::AArch64);
  ASSERT_TRUE(bool(Selected)) << llvm::toString(Selected.takeError());
  auto Space = Selected->CPU->addressSpace();
  ASSERT_EQ(llvm::toString(Space->map(Code, Page, Read | Write | Execute)), "");
  ASSERT_EQ(llvm::toString(Space->writeInteger(Code, 0xF9400020, 4)), "");
  ASSERT_EQ(llvm::toString(Space->writeInteger(Code + 4, 0xD503201F, 4)), "");
  ASSERT_EQ(llvm::toString(Selected->CPU->setReg(AArch64Register::X1, Missing)),
            "");
  auto Budget = llvm::cantFail(ExecutionBudget::create({1000, 100, 1000000}));
  auto Session =
      ExecutionSession::create(std::move(Selected->CPU), std::move(Budget), {},
                               {}, [](const ResourcePause &) { return true; });
  ASSERT_TRUE(bool(Session));
  ASSERT_EQ(llvm::toString((*Session)->watchExecution({{Code + 4, 4}})), "");
  auto Exit = (*Session)->run(Code, 1000);
  ASSERT_TRUE(bool(Exit));
  ASSERT_EQ(Exit->CPU->Kind, ExecutionExitKind::ResourcePause);
  EXPECT_EQ(Exit->CPU->Resource->ResumePC, Code);
  EXPECT_EQ(Exit->CPU->Resource->RequiredPermissions, Read);
  ASSERT_EQ(llvm::toString(Space->map(Missing, Page, Read | Write)), "");
  ASSERT_EQ(llvm::toString(Space->writeInteger(Missing, 42, 8)), "");
  ASSERT_EQ(llvm::toString((*Session)->acknowledgeResourcePause()), "");
  Exit = (*Session)->run(Code, 1000);
  ASSERT_TRUE(bool(Exit));
  EXPECT_EQ(Exit->Kind, SessionExitKind::ExecutionWatch);
  EXPECT_EQ(*(*Session)->cpu().reg(AArch64Register::X0), 42u);
}
TEST(ResourcePauseArchitecture, SoftwareARM64RejectsUnverifiedPauseHooks) {
  auto Available = queryExecutionBackendBuild(ExecutionBackendKind::Unicorn,
                                              GuestArchitecture::AArch64);
  ASSERT_TRUE(bool(Available));
  if (Available->Availability != BackendAvailability::Available)
    GTEST_SKIP() << Available->Reason;
  auto Selected = createExecutionBackend(ExecutionBackendKind::Unicorn,
                                         ExecutionContract::Software, 32 * Page,
                                         GuestArchitecture::AArch64);
  ASSERT_TRUE(bool(Selected)) << llvm::toString(Selected.takeError());
  EXPECT_FALSE(Selected->CPU->supportsResourcePause());
  BackendHooks Hooks;
  Hooks.ResourcePause = [](const ResourcePause &) { return true; };
  EXPECT_NE(llvm::toString(Selected->CPU->installHooks(std::move(Hooks))), "");
}
INSTANTIATE_TEST_SUITE_P(
    SoftwareAndChecked, ResourceCPU,
    testing::Values(ExecutionContract::Software, ExecutionContract::CheckedX64),
    [](const testing::TestParamInfo<ExecutionContract> &Info) {
      return Info.param == ExecutionContract::Software ? "SoftwareX64"
                                                       : "CheckedX64";
    });
} // namespace
