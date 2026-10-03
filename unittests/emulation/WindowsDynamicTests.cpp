//===- WindowsDynamicTests.cpp - Original runtime loader observations ----===//
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
#define NEVERD_DYNAMIC_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_DYNAMIC_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/WindowsDynamicCases.def"
#undef NEVERD_DYNAMIC_TEXT
#undef NEVERD_DYNAMIC_VALUE
struct Case {
  const char *Name, *Argument;
};
constexpr Case Cases[] = {
#define NEVERD_DYNAMIC_CASE(Name, Argument) {#Name, Argument},
#include "fixtures/WindowsDynamicCases.def"
#undef NEVERD_DYNAMIC_CASE
};
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
class WindowsDynamic : public testing::TestWithParam<Profile> {
protected:
  std::filesystem::path Directory;
  ProcessOptions Options;
  void SetUp() override {
#ifndef NEVERD_WINDOWS_DYNAMIC_FIXTURE_DIR
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
    Directory =
        std::filesystem::path(NEVERD_WINDOWS_DYNAMIC_FIXTURE_DIR) / P.Directory;
    Options.Backend = P.Backend;
#endif
  }
};
TEST_P(WindowsDynamic, ExecutesOriginalRuntimeLoaderScenarios) {
  for (bool NoEntry : {false, true}) {
    const auto Inputs = NoEntry ? Directory / NoEntryDirectory : Directory;
    Options.Windows = WindowsProcessOptions{{{LeafFile, Inputs / LeafFile},
                                             {MiddleFile, Inputs / MiddleFile},
                                             {TopFile, Directory / TopFile}}};
    for (const auto &C : Cases) {
      if (NoEntry &&
          (C.Argument[1] == FailedMiddleMode || C.Argument[1] == LeafRole ||
           C.Argument[1] == NestedFailureMode))
        continue;
      SCOPED_TRACE(C.Name);
      Options.Arguments = {ProgramFile, C.Argument};
      auto R = emulateProcess(Directory / ProgramFile,
                              ProcessProfile::WindowsPE64, Options);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      llvm::outs() << ObservationLabel << NoEntry << ' ' << ProgramFile << ' '
                   << C.Argument << ' ' << R->ExitStatus.value_or(0) << ' '
                   << llvm::toHex(R->StandardOutput) << '\n';
      EXPECT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
      EXPECT_EQ(R->ExitStatus, ExitStatus) << llvm::toHex(R->StandardError);
      EXPECT_TRUE(R->StandardError.empty());
    }
  }
}
INSTANTIATE_TEST_SUITE_P(ExplicitBackends, WindowsDynamic,
                         testing::ValuesIn(Profiles),
                         [](const testing::TestParamInfo<Profile> &P) {
                           return P.param.Name;
                         });
TEST(WindowsDynamicOracle, NativeWindowsLoadsAndUnloadsOriginalImages) {
#if !defined(NEVERD_WINDOWS_DYNAMIC_FIXTURE_DIR) || !defined(_WIN32) ||        \
    !defined(_M_X64)
  GTEST_SKIP() << NativeOnly;
#else
  const auto Directory =
      std::filesystem::path(NEVERD_WINDOWS_DYNAMIC_FIXTURE_DIR) / X64Dir;
  llvm::SmallString<128> Temporary;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(TempPrefix, Temporary));
  const std::filesystem::path Root(Temporary.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  for (bool NoEntry : {false, true}) {
    for (const char *File :
         {ProgramFile, StaticProgramFile, LeafFile, MiddleFile, TopFile})
      ASSERT_FALSE(llvm::sys::fs::copy_file(
          ((NoEntry && (File == LeafFile || File == MiddleFile)
                ? Directory / NoEntryDirectory
                : Directory) /
           File)
              .string(),
          (Root / File).string()));
    for (const char *File : {ProgramFile, StaticProgramFile}) {
      const auto Program = (Root / File).string();
      const auto Output = (Root / StdoutFile).string(),
                 Error = (Root / StderrFile).string();
      const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Output,
                                                          Error};
      for (const auto &C : Cases) {
        if (File == StaticProgramFile && &C != &Cases[0])
          continue;
        if (NoEntry &&
            (C.Argument[1] == FailedMiddleMode || C.Argument[1] == LeafRole ||
             C.Argument[1] == NestedFailureMode))
          continue;
        SCOPED_TRACE(C.Name);
        std::string Diagnostic;
        bool Failed = false;
        const int Status = llvm::sys::ExecuteAndWait(
            Program, {Program, C.Argument}, std::nullopt, Redirects,
            NativeTimeoutSeconds, 0, &Diagnostic, &Failed);
        ASSERT_FALSE(Failed) << Diagnostic;
        auto Out = llvm::MemoryBuffer::getFile(Output),
             Err = llvm::MemoryBuffer::getFile(Error);
        ASSERT_TRUE(bool(Out));
        ASSERT_TRUE(bool(Err));
        llvm::outs() << ObservationLabel << NoEntry << ' ' << File << ' '
                     << C.Argument << ' ' << Status << ' '
                     << llvm::toHex((*Out)->getBuffer()) << '\n';
        EXPECT_EQ(Status, ExitStatus) << llvm::toHex((*Err)->getBuffer());
        EXPECT_TRUE((*Err)->getBuffer().empty());
      }
    }
  }
#endif
}
} // namespace
} // namespace neverd::emulation
