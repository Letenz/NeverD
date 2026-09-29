//===- LinuxProcessTests.cpp - Executable ELF userspace contracts ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"
#include "neverd/loader/ELF/ELFLoader.h"

namespace neverd::emulation {
namespace {
#define NEVERD_LINUX_FIXTURE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_LINUX_FIXTURE_TEXT(Name, Text) constexpr char Name[] = Text;
#define NEVERD_LINUX_FIXTURE_BYTES(Name, ...)                                  \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#define NEVERD_LINUX_FIXTURE_MODE(Name, Character, Text)                       \
  constexpr char Name[] = Text;
#include "fixtures/LinuxProcessCases.def"
#undef NEVERD_LINUX_FIXTURE_MODE
#undef NEVERD_LINUX_FIXTURE_BYTES
#undef NEVERD_LINUX_FIXTURE_TEXT
#undef NEVERD_LINUX_FIXTURE_VALUE
namespace tls_fixture {
#define NEVERD_TLS_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_TLS_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/LinuxTLSCases.def"
#undef NEVERD_TLS_VALUE
#undef NEVERD_TLS_TEXT
} // namespace tls_fixture
namespace pie_fixture {
#define NEVERD_PIE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_PIE_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/LinuxPIECases.def"
#undef NEVERD_PIE_VALUE
#undef NEVERD_PIE_TEXT
} // namespace pie_fixture
namespace memory_fixture {
#define NEVERD_LINUX_MEMORY_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_LINUX_MEMORY_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/LinuxMemoryCases.def"
#undef NEVERD_LINUX_MEMORY_VALUE
#undef NEVERD_LINUX_MEMORY_TEXT
} // namespace memory_fixture
struct Profile {
  const char *Name;
  ExecutionBackendKind Backend;
  GuestArchitecture ISA;
  const char *Fixture;
};
constexpr Profile Profiles[] = {
#define NEVERD_USER_PROFILE(Name, Backend, ISA, Contract)                      \
  {#Name, ExecutionBackendKind::Backend, GuestArchitecture::ISA, #ISA ".elf"},
#include "UserExecutionCases.def"
#undef NEVERD_USER_PROFILE
};
void PrintTo(const Profile &P, std::ostream *OS) { *OS << P.Name; }
class LinuxProcess : public testing::TestWithParam<Profile> {
protected:
  ProcessOptions Options;
  std::filesystem::path Path;
  void SetUp() override {
#ifndef NEVERD_PROCESS_FIXTURE_DIR
    GTEST_SKIP() << MissingTools;
#else
    Path =
        std::filesystem::path(NEVERD_PROCESS_FIXTURE_DIR) / GetParam().Fixture;
    ExecutionConfiguration Configuration;
    Configuration.Backend = GetParam().Backend;
    Configuration.Architecture = GetParam().ISA;
    Configuration.Contract = GetParam().ISA == GuestArchitecture::X64
                                 ? ExecutionContract::CheckedUserX64
                                 : ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(Configuration);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available)
      GTEST_SKIP() << Probe->Reason;
    Options.Backend = GetParam().Backend;
    Options.Arguments = {ExecutableName, Normal};
    Options.Environment = {Environment};
#endif
  }
  ProcessResult run() {
    return llvm::cantFail(
        emulateProcess(Path, ProcessProfile::LinuxELF64, Options));
  }
};

TEST_P(LinuxProcess, LoadsDataBSSAndInitialStackThenHandlesErrorsAndExits) {
  // A tiny quantum forces many real CPU resumptions through the same process
  // image, registers, stack, service return state and resource account.
  Options.InstructionQuantum = 3;
  auto Result = run();
  EXPECT_EQ(Result.Stop, ProcessStopReason::Exited) << Result.Diagnostic;
  EXPECT_EQ(Result.ExitStatus, ExitStatus);
  EXPECT_EQ(Result.Architecture, GetParam().ISA);
  EXPECT_EQ(Result.SelectedBackend, GetParam().Backend);
  EXPECT_EQ(Result.StandardOutput, Message);
  EXPECT_EQ(Result.StandardError,
            std::string(reinterpret_cast<const char *>(BinaryOutput),
                        sizeof(BinaryOutput)));
  EXPECT_GT(Result.Instructions, Options.InstructionQuantum);
  EXPECT_EQ(Result.Events, Result.Services.size());
  ASSERT_FALSE(Result.Services.empty());
  EXPECT_FALSE(Result.Services.back().Result);
}
TEST_P(LinuxProcess, InstructionCreditsSurviveQuantumResumptions) {
  Options.Arguments[1] = Loop;
  Options.InstructionQuantum = 3;
  Options.Limits.Instructions = 1000;
  auto Result = run();
  EXPECT_EQ(Result.Stop, ProcessStopReason::InstructionLimit)
      << Result.Diagnostic;
  EXPECT_EQ(Result.Instructions, Options.Limits.Instructions);
  EXPECT_FALSE(Result.ExitStatus);
}
TEST_P(LinuxProcess,
       AnonymousMappingsHeapAndCodeSurviveRealServiceContinuations) {
  Path.replace_filename(Path.stem().string() + memory_fixture::Suffix);
  Options.Arguments = {memory_fixture::ExecutableName, memory_fixture::Normal};
  Options.InstructionQuantum = 3;
  const auto Result = run();
  EXPECT_EQ(Result.Stop, ProcessStopReason::Exited) << Result.Diagnostic;
  EXPECT_EQ(Result.ExitStatus, memory_fixture::ExitStatus);
  EXPECT_EQ(Result.StandardOutput, memory_fixture::Message);
  EXPECT_TRUE(Result.StandardError.empty());
  EXPECT_EQ(Result.Events, Result.Services.size());
}
TEST_P(LinuxProcess,
       GuestStoresObserveBothExplicitAndPartialProtectionChanges) {
  Path.replace_filename(Path.stem().string() + memory_fixture::Suffix);
  for (const char *Mode :
       {memory_fixture::ProtectionFault, memory_fixture::HoleFault}) {
    SCOPED_TRACE(Mode);
    Options.Arguments = {memory_fixture::ExecutableName, Mode};
    const auto Result = run();
    EXPECT_EQ(Result.Stop, ProcessStopReason::CPUFailure) << Result.Diagnostic;
    EXPECT_FALSE(Result.ExitStatus);
    ASSERT_TRUE(Result.LastCPUExit);
    ASSERT_TRUE(Result.LastCPUExit->Fault);
    EXPECT_EQ(Result.LastCPUExit->Fault->Kind, BackendFaultKind::Protection);
    EXPECT_TRUE(Result.StandardOutput.empty());
  }
}
TEST_P(LinuxProcess, FixedReplacementIsRejectedWithoutInventingAMappingResult) {
  Path.replace_filename(Path.stem().string() + memory_fixture::Suffix);
  Options.Arguments = {memory_fixture::ExecutableName,
                       memory_fixture::Unsupported};
  const auto Result = run();
  EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_FALSE(Result.ExitStatus);
  ASSERT_EQ(Result.Services.size(), 1u);
  EXPECT_FALSE(Result.Services.front().Result);
}
TEST_P(LinuxProcess, CompilerTLSBlocksRemainIndependentAcrossProcessQuanta) {
  Path.replace_filename(Path.stem().string() + tls_fixture::Suffix);
  Options.Arguments = {tls_fixture::ExecutableName, tls_fixture::Normal};
  Options.InstructionQuantum = 3;
  const auto Result = run();
  EXPECT_EQ(Result.Stop, ProcessStopReason::Exited) << Result.Diagnostic;
  EXPECT_EQ(Result.ExitStatus, tls_fixture::ExitStatus);
  EXPECT_EQ(Result.StandardOutput, tls_fixture::Message);
  EXPECT_EQ(Result.SelectedBackend, GetParam().Backend);
  EXPECT_TRUE(Result.StandardError.empty());
  EXPECT_GT(Result.Instructions, Options.InstructionQuantum);
}
TEST_P(LinuxProcess, UnimplementedArchPrctlDoesNotInventAServiceReturn) {
  if (GetParam().ISA != GuestArchitecture::X64)
    GTEST_SKIP() << tls_fixture::RequiresX64;
  Path.replace_filename(Path.stem().string() + tls_fixture::Suffix);
  Options.Arguments = {tls_fixture::ExecutableName, tls_fixture::Unknown};
  const auto Result = run();
  EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService)
      << Result.Diagnostic;
  EXPECT_FALSE(Result.ExitStatus);
  ASSERT_FALSE(Result.Services.empty());
  EXPECT_EQ(Result.Services.back().Number, tls_fixture::ArchPrctl);
  EXPECT_EQ(Result.Services.back().Arguments[0],
            tls_fixture::UnsupportedOperation);
  EXPECT_FALSE(Result.Services.back().Result);
  EXPECT_TRUE(Result.StandardOutput.empty());
}
TEST_P(LinuxProcess, StaticPIEReceivesRawSlotsAndRelocatesAtItsActualBias) {
  Path.replace_filename(Path.stem().string() + pie_fixture::Suffix);
  Options.Arguments = {pie_fixture::ExecutableName};
  Options.InstructionQuantum = 3;
  const auto Result = run();
  EXPECT_EQ(Result.Stop, ProcessStopReason::Exited) << Result.Diagnostic;
  EXPECT_EQ(Result.ExitStatus, pie_fixture::ExitStatus);
  EXPECT_EQ(Result.StandardOutput, pie_fixture::Message);
  EXPECT_EQ(Result.SelectedBackend, GetParam().Backend);
  EXPECT_TRUE(Result.StandardError.empty());
  EXPECT_GT(Result.Instructions, Options.InstructionQuantum);
}
TEST_P(LinuxProcess, ServiceEventLimitStopsBeforeAnotherGuestReturn) {
  Options.Limits.Events = 1;
  auto Result = run();
  EXPECT_EQ(Result.Stop, ProcessStopReason::EventLimit) << Result.Diagnostic;
  EXPECT_EQ(Result.Events, Options.Limits.Events);
  EXPECT_EQ(Result.Services.size(), Options.Limits.Events);
  EXPECT_TRUE(Result.StandardOutput.empty());
  EXPECT_FALSE(Result.ExitStatus);
}
TEST_P(LinuxProcess, OutputLimitDoesNotPublishAnOverBudgetWrite) {
  Options.OutputLimit = sizeof(Message) - 2;
  auto Result = run();
  EXPECT_EQ(Result.Stop, ProcessStopReason::OutputLimit) << Result.Diagnostic;
  EXPECT_TRUE(Result.StandardOutput.empty());
  EXPECT_TRUE(Result.StandardError.empty());
  EXPECT_FALSE(Result.ExitStatus);
}
TEST_P(LinuxProcess, UnknownServiceDoesNotBecomeSuccessOrHostExecution) {
  Options.Arguments[1] = Unknown;
  auto Result = run();
  EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService)
      << Result.Diagnostic;
  EXPECT_FALSE(Result.ExitStatus);
  ASSERT_EQ(Result.Services.size(), 1u);
  EXPECT_EQ(Result.Services[0].Number, UnknownService);
  EXPECT_FALSE(Result.Services[0].Result);
}
TEST_P(LinuxProcess, UnmappedAndReadOnlyStoresRemainGuestCPUFailures) {
  for (const auto *Mode : {Fault, ReadOnly}) {
    Options.Arguments[1] = Mode;
    auto Result = run();
    EXPECT_EQ(Result.Stop, ProcessStopReason::CPUFailure) << Result.Diagnostic;
    EXPECT_FALSE(Result.ExitStatus);
    ASSERT_TRUE(Result.LastCPUExit);
    EXPECT_EQ(Result.LastCPUExit->Kind, ExecutionExitKind::GuestFault);
    EXPECT_TRUE(Result.StandardOutput.empty());
  }
}
TEST_P(LinuxProcess, AWriteCanReturnItsReadablePrefixAndContinue) {
  Options.Arguments[1] = Partial;
  auto Result = run();
  EXPECT_EQ(Result.Stop, ProcessStopReason::Exited) << Result.Diagnostic;
  EXPECT_EQ(Result.ExitStatus, ExitStatus);
  EXPECT_EQ(Result.StandardOutput, Tail);
  ASSERT_FALSE(Result.Services.empty());
  EXPECT_EQ(Result.Services[0].Result, sizeof(Tail) - 1);
}
TEST_P(LinuxProcess, InvalidRequestAndStackStringsFailBeforeExecution) {
  Options.Arguments.push_back(std::string(sizeof(Message), '\0'));
  auto Result = emulateProcess(Path, ProcessProfile::LinuxELF64, Options);
  EXPECT_FALSE(bool(Result));
  llvm::consumeError(Result.takeError());
  Options.Arguments = {ExecutableName, Normal};
  Options.StackSize = PageSize - 1;
  Result = emulateProcess(Path, ProcessProfile::LinuxELF64, Options);
  EXPECT_FALSE(bool(Result));
  llvm::consumeError(Result.takeError());
}

INSTANTIATE_TEST_SUITE_P(Backends, LinuxProcess, testing::ValuesIn(Profiles),
                         [](const testing::TestParamInfo<Profile> &P) {
                           return P.param.Name;
                         });
} // namespace
} // namespace neverd::emulation
