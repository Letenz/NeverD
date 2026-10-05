//===- DarwinProcessTests.cpp - Real macOS/iOS Mach-O workloads -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DarwinFileTestData.h"
#include "DarwinTestImage.h"
#include "DarwinTimeTestData.h"
#include "HvfTestPolicy.h"
#include "gtest/gtest.h"
#include "os/darwin/process/DarwinProcess.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/ADT/StringExtras.h"

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
TEST_P(DarwinProcess,
       FilesShareOffsetsAcrossDupAndKeepPositionedReadsIndependent) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/data"] = {'0', '1', '2', '3', '4',
                                         '5', '6', '7', '8', '9'};
  Options.Arguments[2] = "/data";
  for (auto Mode : {"files", "files-nocancel"}) {
    SCOPED_TRACE(Mode);
    auto Result = run(Mode);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
    EXPECT_EQ(Result->ExitStatus, 37);
    EXPECT_EQ(Result->StandardOutput, "f");
    ASSERT_FALSE(Result->Services.empty());
    EXPECT_EQ(Result->Services.front().Number,
              (GetParam().ISA == GuestArchitecture::X64 ? 0x2000000u : 0u) +
                  (Mode == llvm::StringRef("files") ? 5u : 398u));
    EXPECT_EQ(Result->Services.front().Error, false);
  }
}
TEST_P(DarwinProcess,
       WritableFilesPreserveSharedContentsOffsetsAndNativeErrorOrder) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/data"] = {'0', '1', '2', '3', '4',
                                         '5', '6', '7', '8', '9'};
  Options.DarwinFiles->WritableFiles.insert("/data");
  Options.Arguments[2] = "/data";
  for (auto Mode : {"writable-files", "writable-files-nocancel"}) {
    SCOPED_TRACE(Mode);
    auto Result = run(Mode);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
    EXPECT_EQ(Result->ExitStatus, 37);
    EXPECT_EQ(Result->StandardOutput, "00006e");
    EXPECT_TRUE(Result->StandardError.empty());
    const uint64_t Class =
        GetParam().ISA == GuestArchitecture::X64 ? 0x2000000 : 0;
    const uint64_t Pwrite =
        Class + (llvm::StringRef(Mode).ends_with("nocancel") ? 415 : 154);
    unsigned Positioned = 0;
    for (const auto &Event : Result->Services)
      if (Event.Number == Pwrite)
        ++Positioned;
    EXPECT_EQ(Positioned, 5u);
  }
}
TEST_P(DarwinProcess,
       PrivateFileMappingsRetainCopiesAfterCloseAndPreserveOffsets) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/data"] = {'0', '1', '2', '3', '4',
                                         '5', '6', '7', '8', '9'};
  Options.Arguments[2] = "/data";
  auto Result = run("file-mapping");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, "m");
  EXPECT_TRUE(Result->StandardError.empty());
  EXPECT_EQ(Result->SelectedBackend, GetParam().Backend);
}
TEST_P(DarwinProcess, DirectoryRelativePathsAndCWDMatchNativeLifetime) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/data"] = {'0', '1', '2', '3', '4',
                                         '5', '6', '7', '8', '9'};
  Options.DarwinFiles->Metadata["/data"] = darwin_test::metadata();
  Options.DarwinFiles->Directories.insert("/empty");
  Options.DarwinFiles->WorkingDirectory = "/empty";
  Options.Arguments[2] = "/data";
  auto Result = run("directories");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, "d");
  EXPECT_TRUE(Result->StandardError.empty());
  EXPECT_EQ(Result->SelectedBackend, GetParam().Backend);
}
TEST_P(DarwinProcess, DirectoryEnumerationPreservesRecordsCookiesAndCopyOrder) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/data"] = {'0', '1', '2', '3', '4',
                                         '5', '6', '7', '8', '9'};
  Options.DarwinFiles->Metadata["/data"] = darwin_test::metadata();
  Options.DarwinFiles->Directories.insert("/empty");
  Options.DarwinFiles->DirectoryContents["/"] =
      darwin_test::directoryContents();
  Options.Arguments[2] = "/data";
  auto Result = run("directory-entries");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, "e");
  EXPECT_TRUE(Result->StandardError.empty());
  EXPECT_EQ(Result->SelectedBackend, GetParam().Backend);
}
TEST_P(DarwinProcess, ExplicitTimeObservationsPreserveBytesErrorsAndCopyOrder) {
  auto Null = run("time-null");
  ASSERT_TRUE(bool(Null)) << llvm::toString(Null.takeError());
  EXPECT_EQ(Null->Stop, ProcessStopReason::Exited) << Null->Diagnostic;
  EXPECT_EQ(Null->ExitStatus, 37);
  auto Missing = run("time");
  ASSERT_TRUE(bool(Missing)) << llvm::toString(Missing.takeError());
  EXPECT_EQ(Missing->Stop, ProcessStopReason::UnsupportedService);
  EXPECT_TRUE(Missing->StandardOutput.empty());
  Options.DarwinTime = darwin_test::timeOptions();
  for (auto Mode : {"time", "time-values"}) {
    SCOPED_TRACE(Mode);
    auto Result = run(Mode);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
    EXPECT_EQ(Result->ExitStatus, 37);
    EXPECT_EQ(Result->StandardOutput,
              Mode == llvm::StringRef("time")
                  ? "t"
                  : llvm::fromHex(darwin_test::TimeHex));
    EXPECT_TRUE(Result->StandardError.empty());
    EXPECT_EQ(Result->SelectedBackend, GetParam().Backend);
    ASSERT_FALSE(Result->Services.empty());
    EXPECT_EQ(Result->Services.front().Number,
              (GetParam().ISA == GuestArchitecture::X64 ? 0x2000000u : 0u) +
                  116u);
    EXPECT_EQ(Result->Services.front().Result, 0u);
    EXPECT_EQ(Result->Services.front().Error, false);
  }
}
TEST_P(DarwinProcess, MachTimePreservesReturnStateAndExplicitObservations) {
  auto Missing = run("mach-time");
  ASSERT_TRUE(bool(Missing)) << llvm::toString(Missing.takeError());
  EXPECT_EQ(Missing->Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(Missing->Diagnostic, "Darwin timebase is not configured");
  const bool X64 = GetParam().ISA == GuestArchitecture::X64;
  const uint64_t Timebase = X64 ? 0x01000059ULL : uint64_t(-89);
  Options.DarwinTime = darwin_test::timeOptions();
  for (auto Mode : {"mach-time", "mach-timebase-values"}) {
    SCOPED_TRACE(Mode);
    auto Result = run(Mode);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
    EXPECT_EQ(Result->ExitStatus, 37);
    EXPECT_EQ(Result->StandardOutput,
              Mode == llvm::StringRef("mach-time")
                  ? "h"
                  : llvm::fromHex(darwin_test::TimebaseHex));
    ASSERT_GE(Result->Services.size(), 13u);
    for (size_t I = 0; I != 12; ++I) {
      EXPECT_EQ(Result->Services[I].Result, 0u);
      EXPECT_FALSE(Result->Services[I].Error.has_value());
    }
    EXPECT_EQ(Result->Services[0].Number, Timebase);
    EXPECT_EQ(Result->Services[4].Number, uint32_t(Timebase));
    EXPECT_EQ(Result->Services[5].Number,
              0x1234567800000000ULL | uint32_t(Timebase));
    EXPECT_EQ(Result->Services.back().Error, false); // BSD write
    if (Mode == llvm::StringRef("mach-time")) {
      EXPECT_EQ(Result->Services[12].Result, 1000u);
      EXPECT_EQ(Result->Services[13].Number,
                X64 ? 0x1234567802000014ULL : 0x1234567800000014ULL);
      EXPECT_EQ(Result->Services[13].Error, false);
      EXPECT_EQ(Result->Services[14].Error, true); // BSD read(999)
      EXPECT_EQ(Result->Services[15].Error, true); // BSD write(999)
    }
  }
  auto Clocks = run("mach-clock-values");
  ASSERT_TRUE(bool(Clocks)) << llvm::toString(Clocks.takeError());
  if (X64) {
    EXPECT_EQ(Clocks->Stop, ProcessStopReason::UnsupportedService);
  } else {
    ASSERT_EQ(Clocks->Stop, ProcessStopReason::Exited) << Clocks->Diagnostic;
    EXPECT_EQ(Clocks->ExitStatus, 37);
    EXPECT_EQ(Clocks->StandardOutput, llvm::fromHex(darwin_test::MachClockHex));
    ASSERT_EQ(Clocks->Services.size(), 3u);
    EXPECT_EQ(Clocks->Services[0].Result, 0xfedcba9876543210ULL);
    EXPECT_EQ(Clocks->Services[1].Result, UINT64_MAX);
    EXPECT_FALSE(Clocks->Services[0].Error);
    EXPECT_FALSE(Clocks->Services[1].Error);
  }
  for (auto Mode : {"mach-absolute", "mach-continuous"}) {
    SCOPED_TRACE(Mode);
    Options.DarwinTime.emplace();
    for (bool Configured : {false, true}) {
      if (Configured) {
        if (Mode == llvm::StringRef("mach-absolute"))
          Options.DarwinTime->MachAbsoluteTime = 0;
        else
          Options.DarwinTime->MachContinuousTime = 0;
      }
      auto Result = run(Mode);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      if (X64 || !Configured) {
        EXPECT_EQ(Result->Stop, ProcessStopReason::UnsupportedService);
        EXPECT_TRUE(Result->StandardOutput.empty());
      } else {
        EXPECT_EQ(Result->Stop, ProcessStopReason::Exited)
            << Result->Diagnostic;
        EXPECT_EQ(Result->ExitStatus, 37);
        EXPECT_EQ(Result->StandardOutput, std::string(8, '\0'));
      }
    }
  }
}
TEST_P(DarwinProcess, FiniteStandardInputRetainsBinaryBytesAndSharedCursor) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->StandardInput = {0, 0xff, 'x'};
  auto Result = run("stdin");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, std::string("\0\xffx", 3));
  EXPECT_TRUE(Result->StandardError.empty());
}
TEST_P(DarwinProcess,
       Stat64ObservationsPreserveLayoutErrorsAndDescriptorState) {
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/data"] = {'0', '1', '2', '3', '4',
                                         '5', '6', '7', '8', '9'};
  Options.Arguments[2] = "/data";
  auto Missing = run("file-status");
  ASSERT_TRUE(bool(Missing)) << llvm::toString(Missing.takeError());
  EXPECT_EQ(Missing->Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(Missing->Services.size(), 1u);
  EXPECT_TRUE(Missing->StandardOutput.empty());
  Options.DarwinFiles->Metadata["/data"] = darwin_test::metadata();
  auto Result = run("file-status");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, "s");
  EXPECT_TRUE(Result->StandardError.empty());
  ASSERT_FALSE(Result->Services.empty());
  EXPECT_EQ(Result->Services.front().Number,
            (GetParam().ISA == GuestArchitecture::X64 ? 0x2000000u : 0u) +
                338u);
  EXPECT_EQ(Result->Services.front().Result, 0u);
  EXPECT_EQ(Result->Services.front().Error, false);
}
TEST_P(DarwinProcess, OutputDescriptorsCanBeClosedReusedAndRedirected) {
  auto Result = run("output-descriptors");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 37);
  EXPECT_EQ(Result->StandardOutput, "ok");
  EXPECT_TRUE(Result->StandardError.empty());
  Options.OutputLimit = 1;
  Result = run("output-descriptors");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, ProcessStopReason::OutputLimit);
  EXPECT_EQ(Result->StandardOutput, "o");
}
TEST_P(DarwinProcess, MissingFileAndInputConfigurationStopWithoutHostAccess) {
  for (auto Mode : {"files", "stdin"}) {
    SCOPED_TRACE(Mode);
    auto Result = run(Mode);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, ProcessStopReason::UnsupportedService);
    ASSERT_FALSE(Result->Services.empty());
    EXPECT_FALSE(Result->Services.back().Result);
    EXPECT_FALSE(Result->Services.back().Error);
    EXPECT_TRUE(Result->StandardOutput.empty());
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
  Options.StackSize = Image->Memory.PageSize * 4;
  Options.MemoryLimit =
      Image->Plan.MappedBytes + Options.StackSize + Image->Memory.PageSize;
  Options.OutputLimit = Image->Memory.PageSize;
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
  Options.DarwinTime = darwin_test::timeOptions();
  for (auto Mode : {"unknown", "mach", "badtrap", "mach-int32-min",
                    "mach-wrong-class", "mach-foreign-class"}) {
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
