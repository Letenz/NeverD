//===- DarwinProcessTests.cpp - Real macOS/iOS Mach-O workloads -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DarwinTestImage.h"
#include "HvfTestPolicy.h"
#include "gtest/gtest.h"
#include "os/darwin/DarwinProcess.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"

namespace neverd::emulation {
namespace {
struct Profile {
  const char *Name, *File;
  ProcessProfile OS;
  GuestArchitecture ISA;
  ExecutionBackendKind Backend;
};
std::vector<Profile> profiles() {
  std::vector<Profile> Result;
  for (auto Backend : {ExecutionBackendKind::Unicorn, ExecutionBackendKind::HVF,
                       ExecutionBackendKind::KVM, ExecutionBackendKind::WHP})
    for (auto P :
         {Profile{"MacOSX64", "macos-x86_64", ProcessProfile::MacOSMachO64,
                  GuestArchitecture::X64, Backend},
          Profile{"MacOSARM64", "macos-arm64", ProcessProfile::MacOSMachO64,
                  GuestArchitecture::AArch64, Backend},
          Profile{"IOSARM64", "ios-arm64", ProcessProfile::IOSMachO64,
                  GuestArchitecture::AArch64, Backend},
          Profile{"SimulatorX64", "ios-simulator-x86_64",
                  ProcessProfile::IOSSimulatorMachO64, GuestArchitecture::X64,
                  Backend},
          Profile{"SimulatorARM64", "ios-simulator-arm64",
                  ProcessProfile::IOSSimulatorMachO64,
                  GuestArchitecture::AArch64, Backend}})
      Result.push_back(P);
  return Result;
}
class DarwinProcess : public testing::TestWithParam<Profile> {
protected:
  ProcessOptions Options;
  std::filesystem::path Path;
  void SetUp() override {
    const auto &P = GetParam();
#ifndef NEVERD_DARWIN_FIXTURE_DIR
    if (requireHvf(P.Backend, P.ISA))
      FAIL() << "Clang and ld64.lld Darwin fixtures are required";
    GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures are unavailable";
#else
    Path = std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) / P.File;
    ExecutionConfiguration C;
    C.Backend = P.Backend;
    C.Architecture = P.ISA;
    C.Contract = P.ISA == GuestArchitecture::X64
                     ? ExecutionContract::CheckedUserX64
                     : ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(C);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available) {
      if (requireHvf(P.Backend, P.ISA))
        FAIL() << Probe->Reason;
      GTEST_SKIP() << Probe->Reason;
    }
    Options.Backend = P.Backend;
    Options.Arguments = {"guest", "normal", "argument"};
    Options.Environment = {"MODE=test"};
    Options.InstructionQuantum = 7;
#endif
  }
  llvm::Expected<ProcessResult> run(llvm::StringRef Mode) {
    Options.Arguments[1] = Mode.str();
    return emulateProcess(Path, GetParam().OS, Options);
  }
};
TEST_P(DarwinProcess, StartupDataBSSCarryAndBinaryOutput) {
  auto Result = run("normal");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, std::string("darwin\0\xff\n", 9));
  ASSERT_EQ(Result->Services.size(), 4u);
  EXPECT_EQ(Result->Services[0].Result, 9);
  EXPECT_EQ(Result->Services[0].Error, true);
  EXPECT_EQ(Result->Services[1].Result, 1000);
  EXPECT_EQ(Result->Services[1].Error, false);
  EXPECT_EQ(Result->Services[2].Result, 14);
  EXPECT_EQ(Result->Services[2].Error, true);
  EXPECT_EQ(Result->Services[3].Result, 9);
  EXPECT_EQ(Result->Services[3].Error, false);
  EXPECT_EQ(Result->SelectedBackend, GetParam().Backend);
  EXPECT_EQ(Result->Profile, GetParam().OS);
}
TEST_P(DarwinProcess, UnixThreadReceivesArgcAtTheInitialStackPointer) {
  using namespace llvm::MachO;
  const auto &P = GetParam();
  const auto Platform = P.OS == ProcessProfile::MacOSMachO64 ? PLATFORM_MACOS
                        : P.OS == ProcessProfile::IOSMachO64
                            ? PLATFORM_IOS
                            : PLATFORM_IOSSIMULATOR;
  darwin_test::Image I(P.ISA == GuestArchitecture::X64, Platform);
  darwin_test::TemporaryImage File(I);
  auto Result = emulateProcess(File.path(), P.OS, Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, Options.Arguments.size());
  EXPECT_EQ(Result->Services.size(), 1u);
  EXPECT_EQ(Result->SelectedBackend, P.Backend);
  EXPECT_EQ(Result->Profile, P.OS);
}
TEST_P(DarwinProcess, MainReturnAndBSDExitHaveRealStatus) {
  for (auto Mode : {"return", "exit", "identity"}) {
    SCOPED_TRACE(Mode);
    auto Result = run(Mode);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
    EXPECT_EQ(Result->ExitStatus, 37);
  }
}
TEST_P(DarwinProcess, PartialCopyRetainsEFAULTAndSubsequentWriteRecovers) {
  auto Result = run("partial");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, "pqp");
  ASSERT_EQ(Result->Services.size(), 3u);
  EXPECT_EQ(Result->Services[1].Result, 14);
  EXPECT_EQ(Result->Services[1].Error, true);
  EXPECT_EQ(Result->Services[2].Result, 1);
  EXPECT_EQ(Result->Services[2].Error, false);
}
TEST_P(DarwinProcess, OversizedWritePrecedesDescriptorPointerAndOutputChecks) {
  Options.OutputLimit = 1;
  auto Result = run("write-length");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, "w");
  EXPECT_TRUE(Result->StandardError.empty());
  ASSERT_EQ(Result->Services.size(), 7u);
  for (size_t I = 0; I < 4; ++I) {
    EXPECT_EQ(Result->Services[I].Result, 22u) << I;
    EXPECT_EQ(Result->Services[I].Error, true) << I;
  }
  EXPECT_EQ(Result->Services[4].Result, 9u);
  EXPECT_EQ(Result->Services[5].Result, 14u);
  EXPECT_EQ(Result->Services[6].Result, 1u);
  EXPECT_EQ(Result->Services[6].Error, false);
}
TEST_P(DarwinProcess, AnonymousMemoryAlignmentAtomicProtectionAndReuse) {
  auto Result = run("memory");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, "d");
}
TEST_P(DarwinProcess, InitialStackAndDataPartialUnmapReleasesPhysicalBudget) {
  const auto Profile =
      GetParam().OS == ProcessProfile::MacOSMachO64
          ? darwin_model::macOSProfile()
          : darwin_model::iOSProfile(GetParam().OS ==
                                     ProcessProfile::IOSSimulatorMachO64);
  auto Image = darwin_model::loadImage(Path, Profile, Options);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  Options.StackSize = Image->PageSize * 4;
  Options.MemoryLimit =
      Image->Plan.MappedBytes + Options.StackSize + Image->PageSize;
  Options.OutputLimit = Image->PageSize;
  auto Result = run("release");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
}
TEST_P(DarwinProcess, FaultAndReadOnlyStoreRemainCPUFailures) {
  for (auto Mode : {"fault", "permission"}) {
    SCOPED_TRACE(Mode);
    auto Result = run(Mode);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, ProcessStopReason::CPUFailure)
        << Result->Diagnostic;
    EXPECT_FALSE(Result->ExitStatus);
  }
}
TEST_P(DarwinProcess, UnknownBSDMachAndForeignTrapFailExplicitly) {
  for (auto Mode : {"unknown", "mach", "badtrap"}) {
    SCOPED_TRACE(Mode);
    auto Result = run(Mode);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, ProcessStopReason::UnsupportedService)
        << Result->Diagnostic;
    EXPECT_FALSE(Result->ExitStatus);
  }
}
TEST_P(DarwinProcess, InstructionBudgetSurvivesQuanta) {
  Options.Limits.Instructions = 200;
  auto Result = run("loop");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, ProcessStopReason::InstructionLimit)
      << Result->Diagnostic;
  EXPECT_EQ(Result->Instructions, 200u);
}
TEST_P(DarwinProcess, DeadlineTerminatesANonreturningGuestAcrossQuanta) {
  Options.Limits.Instructions = uint64_t(1) << 40;
  Options.Limits.TimeoutMicroseconds = 1000;
  auto Result = run("loop");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, ProcessStopReason::Timeout) << Result->Diagnostic;
  EXPECT_FALSE(Result->ExitStatus);
  EXPECT_LT(Result->Instructions, Options.Limits.Instructions);
}
TEST_P(DarwinProcess, EventAndOutputLimitsStopBeforeServiceEffects) {
  Options.Limits.Events = 1;
  auto Limited = run("normal");
  ASSERT_TRUE(bool(Limited)) << llvm::toString(Limited.takeError());
  EXPECT_EQ(Limited->Stop, ProcessStopReason::EventLimit);
  EXPECT_EQ(Limited->Events, 1u);
  EXPECT_TRUE(Limited->StandardOutput.empty());
  Options.Limits.Events = 100;
  Options.OutputLimit = 8;
  auto Output = run("normal");
  ASSERT_TRUE(bool(Output)) << llvm::toString(Output.takeError());
  EXPECT_EQ(Output->Stop, ProcessStopReason::OutputLimit) << Output->Diagnostic;
  EXPECT_TRUE(Output->StandardOutput.empty());
}
TEST_P(DarwinProcess, ExplicitProfileCannotBeReplacedByHostPlatform) {
  auto Wrong = emulateProcess(Path,
                              GetParam().OS == ProcessProfile::MacOSMachO64
                                  ? ProcessProfile::IOSMachO64
                                  : ProcessProfile::MacOSMachO64,
                              Options);
  ASSERT_FALSE(bool(Wrong));
  llvm::consumeError(Wrong.takeError());
}
INSTANTIATE_TEST_SUITE_P(Transports, DarwinProcess,
                         testing::ValuesIn(profiles()),
                         [](const testing::TestParamInfo<Profile> &P) {
                           return std::string(P.param.Name) + "_" +
                                  executionBackendName(P.param.Backend);
                         });
} // namespace
} // namespace neverd::emulation
