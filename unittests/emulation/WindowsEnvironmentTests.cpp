//===- WindowsEnvironmentTests.cpp - Win32 environment observations ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <filesystem>

namespace neverd::emulation {
namespace {
#define NEVERD_ENV_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_ENV_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/WindowsEnvironmentCases.def"
#undef NEVERD_ENV_TEXT
#undef NEVERD_ENV_VALUE
struct Case {
  const char *Name, *Argument;
};
constexpr Case Cases[] = {
#define NEVERD_ENV_CASE(Name, Argument) {#Name, Argument},
#include "fixtures/WindowsEnvironmentCases.def"
#undef NEVERD_ENV_CASE
};
llvm::StringRef expected(llvm::StringRef Argument) {
#define NEVERD_ENV_EXPECTED(Mode, Hex)                                         \
  if (Argument == Mode)                                                        \
    return Hex;
#include "fixtures/WindowsEnvironmentCases.def"
#undef NEVERD_ENV_EXPECTED
  return {};
}
struct Profile {
  const char *Name;
  ExecutionBackendKind Backend;
  GuestArchitecture ISA;
  const char *Directory;
};
constexpr Profile Profiles[] = {
#define NEVERD_USER_PROFILE(Name, Backend, ISA, Contract)                      \
  {#Name, ExecutionBackendKind::Backend, GuestArchitecture::ISA, ISA##Dir},
#include "UserExecutionCases.def"
#undef NEVERD_USER_PROFILE
};
void PrintTo(const Profile &P, std::ostream *OS) { *OS << P.Name; }
class WindowsEnvironment : public testing::TestWithParam<Profile> {
protected:
  std::filesystem::path Path;
  ProcessOptions Options;
  void SetUp() override {
#ifndef NEVERD_WINDOWS_ENVIRONMENT_FIXTURE_DIR
    if (requireHvf(GetParam().Backend, GetParam().ISA))
      FAIL() << MissingTools;
    GTEST_SKIP() << MissingTools;
#else
    const auto &P = GetParam();
    ExecutionConfiguration Config;
    Config.Backend = P.Backend;
    Config.Architecture = P.ISA;
    Config.Contract = P.ISA == GuestArchitecture::X64
                          ? ExecutionContract::CheckedUserX64
                          : ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(Config);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available) {
      if (requireHvf(P.Backend, P.ISA))
        FAIL() << Probe->Reason;
      GTEST_SKIP() << Probe->Reason;
    }
    Path = std::filesystem::path(NEVERD_WINDOWS_ENVIRONMENT_FIXTURE_DIR) /
           P.Directory / ProgramFile;
    Options.Backend = P.Backend;
    Options.Environment = {InitialVariable};
    Options.Limits.Instructions = InstructionLimit;
#endif
  }
};
TEST_P(WindowsEnvironment, ExecutesOriginalEnvironmentScenarios) {
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Name);
    ASSERT_FALSE(expected(C.Argument).empty());
    Options.Arguments = {ProgramFile, C.Argument};
    auto R = emulateProcess(Path, ProcessProfile::WindowsPE64, Options);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
    EXPECT_EQ(R->ExitStatus, ExitStatus) << llvm::toHex(R->StandardError);
    EXPECT_TRUE(R->StandardError.empty());
    EXPECT_EQ(llvm::toHex(R->StandardOutput), expected(C.Argument));
  }
}
TEST_P(WindowsEnvironment, RejectsInvalidStatePointersAndUnsupportedSemantics) {
  for (const char *Argument :
       {BadPointer, BadBlock, BadOutput, DoubleFree, NonASCII, Overlap}) {
    SCOPED_TRACE(Argument);
    Options.Arguments = {ProgramFile, Argument};
    auto R = emulateProcess(Path, ProcessProfile::WindowsPE64, Options);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_TRUE(R->Stop == ProcessStopReason::RuntimeFailure ||
                R->Stop == ProcessStopReason::UnsupportedService)
        << R->Diagnostic;
    EXPECT_FALSE(R->ExitStatus);
    EXPECT_TRUE(R->StandardOutput.empty());
    ASSERT_FALSE(R->NativeCalls.empty());
    EXPECT_FALSE(R->NativeCalls.back().Result);
  }
}
TEST_P(WindowsEnvironment, SnapshotReleaseReclaimsLimitedGuestMemory) {
  Options.MemoryLimit = LimitedMemory;
  Options.Arguments = {ProgramFile, ReclaimArgument};
  auto R = emulateProcess(Path, ProcessProfile::WindowsPE64, Options);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
  EXPECT_EQ(R->ExitStatus, ExitStatus) << llvm::toHex(R->StandardError);
  EXPECT_EQ(llvm::toHex(R->StandardOutput), expected(ReclaimArgument));
}
INSTANTIATE_TEST_SUITE_P(Backends, WindowsEnvironment,
                         testing::ValuesIn(Profiles),
                         [](const auto &P) { return P.param.Name; });

TEST(WindowsEnvironmentNative, RunsOriginalEnvironmentExecutable) {
#if !defined(_WIN32) || !defined(_M_X64)
  GTEST_SKIP() << NativeOnly;
#elif !defined(NEVERD_WINDOWS_ENVIRONMENT_FIXTURE_DIR)
  FAIL() << MissingTools;
#else
  llvm::SmallString<128> Temporary;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(TempPrefix, Temporary));
  const std::filesystem::path Root(Temporary.c_str());
  auto Cleanup = llvm::scope_exit(
      [&] { llvm::sys::fs::remove_directories(Root.string()); });
  const auto Program =
      (std::filesystem::path(NEVERD_WINDOWS_ENVIRONMENT_FIXTURE_DIR) / X64Dir /
       ProgramFile)
          .string();
  const auto Output = (Root / StdoutFile).string();
  const auto Error = (Root / StderrFile).string();
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Output,
                                                      Error};
  const llvm::StringRef Environment[] = {InitialVariable};
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Name);
    ASSERT_FALSE(expected(C.Argument).empty());
    std::string Diagnostic;
    bool Failed = false;
    const int Status = llvm::sys::ExecuteAndWait(
        Program, {Program, C.Argument}, llvm::ArrayRef(Environment), Redirects,
        NativeTimeoutSeconds, 0, &Diagnostic, &Failed);
    ASSERT_FALSE(Failed) << Diagnostic;
    auto Out = llvm::MemoryBuffer::getFile(Output);
    auto Err = llvm::MemoryBuffer::getFile(Error);
    ASSERT_TRUE(bool(Out));
    ASSERT_TRUE(bool(Err));
    llvm::outs() << ObservationLabel << C.Argument << ' ' << Status << ' '
                 << llvm::toHex((*Out)->getBuffer()) << '\n';
    EXPECT_EQ(Status, ExitStatus) << llvm::toHex((*Err)->getBuffer());
    EXPECT_TRUE((*Err)->getBuffer().empty());
    EXPECT_EQ(llvm::toHex((*Out)->getBuffer()), expected(C.Argument));
  }
#endif
}
} // namespace
} // namespace neverd::emulation
