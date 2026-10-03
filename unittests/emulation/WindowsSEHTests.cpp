//===- WindowsSEHTests.cpp - User C SEH execution and native oracle -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "gtest/gtest.h"
#include "os/windows/process/WindowsProcess.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd::emulation {
namespace {
#define NEVERD_USER_SEH_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_USER_SEH_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/WindowsSEHCases.def"
#undef NEVERD_USER_SEH_TEXT
#undef NEVERD_USER_SEH_VALUE
struct Case {
  const char *Name;
  uint64_t Mode;
  const char *Argument;
  uint64_t Trace;
};
constexpr Case Cases[] = {
#define NEVERD_USER_SEH_CASE(Name, Mode, Argument, Trace)                      \
  {#Name, Mode, Argument, Trace},
#include "fixtures/WindowsSEHCases.def"
#undef NEVERD_USER_SEH_CASE
};
std::string expected(const Case &C) {
  std::array<uint8_t, 2 * sizeof(uint64_t)> Bytes;
  llvm::support::endian::write64le(Bytes.data(), C.Mode);
  llvm::support::endian::write64le(Bytes.data() + sizeof(uint64_t), C.Trace);
  return llvm::toHex(Bytes);
}
struct Profile {
  const char *Name;
  ExecutionBackendKind Backend;
  GuestArchitecture ISA;
};
const auto Profiles = [] {
  std::vector<Profile> Profiles;
#define NEVERD_USER_PROFILE(Name, Backend, ISA, Contract)                      \
  if (GuestArchitecture::ISA == GuestArchitecture::X64)                        \
    Profiles.push_back(                                                        \
        {#Name, ExecutionBackendKind::Backend, GuestArchitecture::ISA});
#include "UserExecutionCases.def"
#undef NEVERD_USER_PROFILE
  return Profiles;
}();
void PrintTo(const Profile &P, std::ostream *OS) { *OS << P.Name; }
class WindowsSEH : public testing::TestWithParam<Profile> {
protected:
  ProcessOptions Options;
  std::filesystem::path Path;
  void SetUp() override {
#ifndef NEVERD_WINDOWS_SEH_FIXTURE_DIR
    if (requireHvf(GetParam().Backend, GetParam().ISA))
      FAIL() << MissingTools;
    GTEST_SKIP() << MissingTools;
#else
    const auto &P = GetParam();
    ExecutionConfiguration Config;
    Config.Backend = P.Backend;
    Config.Architecture = P.ISA;
    Config.Contract = ExecutionContract::CheckedUserX64;
    auto Probe = probeExecutionBackend(Config);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available) {
      if (requireHvf(P.Backend, P.ISA))
        FAIL() << Probe->Reason;
      GTEST_SKIP() << Probe->Reason;
    }
    Path = std::filesystem::path(NEVERD_WINDOWS_SEH_FIXTURE_DIR) / ProgramFile;
    Options.Backend = P.Backend;
    Options.Windows.emplace();
    Options.Windows->Modules.push_back(
        {LibraryFile, Path.parent_path() / LibraryFile});
    Options.Limits.Instructions = InstructionLimit;
    Options.Limits.TimeoutMicroseconds = TimeoutMicroseconds;
#endif
  }
};
TEST_P(WindowsSEH, ExecutesOriginalSearchUnwindAndCrossImageScenarios) {
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Name);
    Options.Arguments = {ProgramFile, C.Argument};
    auto R = emulateProcess(Path, ProcessProfile::WindowsPE64, Options);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
    EXPECT_EQ(R->ExitStatus, CompletionStatus) << llvm::toHex(R->StandardError);
    EXPECT_TRUE(R->StandardError.empty()) << llvm::toHex(R->StandardError);
    EXPECT_EQ(llvm::toHex(R->StandardOutput), expected(C));
  }
}
TEST_P(WindowsSEH, RejectsChangedMetadataAndInvalidContinuation) {
  struct Negative {
    const char *Argument, *Diagnostic;
  };
  constexpr Negative NegativeCases[] = {
#define NEVERD_USER_SEH_NEGATIVE(Name, Mode, Argument, Diagnostic)             \
  {Argument, windows_process::text::Diagnostic},
#include "fixtures/WindowsSEHCases.def"
#undef NEVERD_USER_SEH_NEGATIVE
  };
  for (const auto &C : NegativeCases) {
    SCOPED_TRACE(C.Argument);
    Options.Arguments = {ProgramFile, C.Argument};
    auto R = emulateProcess(Path, ProcessProfile::WindowsPE64, Options);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::RuntimeFailure) << R->Diagnostic;
    EXPECT_NE(R->Diagnostic.find(C.Diagnostic), std::string::npos)
        << R->Diagnostic;
    EXPECT_FALSE(R->ExitStatus);
    EXPECT_TRUE(R->StandardOutput.empty());
    EXPECT_TRUE(R->StandardError.empty()) << llvm::toHex(R->StandardError);
  }
}
TEST_P(WindowsSEH, CallbacksShareTheProcessBudget) {
  Options.Arguments = {ProgramFile, Cases[0].Argument};
  Options.Limits.Events = SmallEventLimit;
  auto R = emulateProcess(Path, ProcessProfile::WindowsPE64, Options);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->Stop, ProcessStopReason::EventLimit) << R->Diagnostic;
  EXPECT_EQ(R->Events, SmallEventLimit);
  EXPECT_FALSE(R->ExitStatus);
}
INSTANTIATE_TEST_SUITE_P(Backends, WindowsSEH, testing::ValuesIn(Profiles),
                         [](const auto &P) { return P.param.Name; });

TEST(WindowsSEHNative, RunsOriginalSEHExecutable) {
#if !defined(_WIN32) || !defined(_M_X64)
  GTEST_SKIP() << NativeOnly;
#elif !defined(NEVERD_WINDOWS_SEH_FIXTURE_DIR)
  FAIL() << MissingTools;
#else
  llvm::SmallString<128> Temporary;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(TempPrefix, Temporary));
  const std::filesystem::path Root(Temporary.c_str());
  auto Cleanup = llvm::scope_exit(
      [&] { llvm::sys::fs::remove_directories(Root.string()); });
  const auto Program =
      (std::filesystem::path(NEVERD_WINDOWS_SEH_FIXTURE_DIR) / ProgramFile)
          .string();
  const auto Output = (Root / StdoutFile).string(),
             Error = (Root / StderrFile).string();
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Output,
                                                      Error};
  const Case Probes[] = {
#define NEVERD_USER_SEH_PROBE(Name, Mode, Argument) {#Name, Mode, Argument, 0},
#include "fixtures/WindowsSEHCases.def"
#undef NEVERD_USER_SEH_PROBE
  };
  auto Run = [&](const Case &C, bool Probe) {
    SCOPED_TRACE(C.Name);
    std::string Diagnostic;
    bool Failed = false;
    int Status = llvm::sys::ExecuteAndWait(
        Program, {Program, C.Argument}, std::nullopt, Redirects,
        NativeTimeoutSeconds, 0, &Diagnostic, &Failed);
    ASSERT_FALSE(Failed) << Diagnostic;
    auto Out = llvm::MemoryBuffer::getFile(Output),
         Err = llvm::MemoryBuffer::getFile(Error);
    ASSERT_TRUE(bool(Out));
    ASSERT_TRUE(bool(Err));
    llvm::outs() << ObservationLabel << C.Argument << ' ' << Status << ' '
                 << llvm::toHex((*Out)->getBuffer()) << '\n';
    EXPECT_EQ(Status, CompletionStatus) << llvm::toHex((*Err)->getBuffer());
    if (Probe) {
      llvm::outs() << RecordLabel << C.Argument << ' '
                   << llvm::toHex((*Err)->getBuffer()) << '\n';
      return;
    }
    EXPECT_TRUE((*Err)->getBuffer().empty())
        << llvm::toHex((*Err)->getBuffer());
    EXPECT_EQ(llvm::toHex((*Out)->getBuffer()), expected(C));
  };
  for (const auto &C : Cases)
    Run(C, false);
  for (const auto &C : Probes)
    Run(C, true);
#endif
}
} // namespace
} // namespace neverd::emulation
