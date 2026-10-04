//===- WindowsAlignmentProcessTests.cpp - Windows SSE fault delivery ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsNativeTestSupport.h"
#include "gtest/gtest.h"
#include "os/windows/process/WindowsProcessExceptions.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/ProcessReport.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"

namespace neverd::emulation {
namespace {
#define NEVERD_ALIGNMENT_PROCESS_VALUE(Name, Value)                            \
  constexpr uint64_t Name = Value;
#define NEVERD_ALIGNMENT_PROCESS_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/WindowsAlignmentProcessCases.def"
#undef NEVERD_ALIGNMENT_PROCESS_TEXT
#undef NEVERD_ALIGNMENT_PROCESS_VALUE
#define NEVERD_WINDOWS_ALIGNMENT_VALUE(Name, Value)                            \
  constexpr uint64_t Name = Value;
#define NEVERD_WINDOWS_ALIGNMENT_WIDE(Name, Value)                             \
  constexpr uint64_t Name = Value;
#include "fixtures/WindowsAlignmentCases.def"
#undef NEVERD_WINDOWS_ALIGNMENT_WIDE
#undef NEVERD_WINDOWS_ALIGNMENT_VALUE
enum Scenario {
#define NEVERD_WINDOWS_ALIGNMENT_SCENARIO(Name) Scenario##Name,
#include "fixtures/WindowsAlignmentCases.def"
#undef NEVERD_WINDOWS_ALIGNMENT_SCENARIO
  ScenarioCount
};
struct Operation {
  const char *Name;
  bool Store;
};
constexpr Operation Operations[] = {
#define NEVERD_ALIGNMENT_PROCESS_RESULT(Name, Store, Low, High) {#Name, Store},
#include "fixtures/WindowsAlignmentProcessCases.def"
#undef NEVERD_ALIGNMENT_PROCESS_RESULT
};
void expectOutput(llvm::StringRef Bytes) {
  const size_t PageCount = std::size(Operations) * (ScenarioCount + 1);
  const size_t ResultBytes = llvm::StringRef(ExpectedOutput).size() / 2;
  ASSERT_EQ(Bytes.size(), PageCount * PageBytes + ResultBytes);
  for (const auto &Op : Operations) {
    SCOPED_TRACE(Op.Name);
    for (size_t Case = 0; Case <= ScenarioCount; ++Case) {
      SCOPED_TRACE(Case);
      for (size_t Word = 0; Word < PageBytes / sizeof(uint64_t); ++Word) {
        uint64_t Expected = VectorHigh ^ Word;
        if (Case == ScenarioCount && Word < VectorBytes / sizeof(uint64_t))
          Expected = Op.Store ? (Word == 0 ? VectorLow : VectorHigh) : 0;
        ASSERT_EQ(llvm::support::endian::read64le(Bytes.data()), Expected)
            << Word;
        Bytes = Bytes.drop_front(sizeof(uint64_t));
      }
    }
  }
  EXPECT_EQ(llvm::toHex(Bytes), ExpectedOutput);
}
struct Profile {
  const char *Name;
  ExecutionBackendKind Backend;
};
constexpr Profile Profiles[] = {
#define NEVERD_ALIGNMENT_PROCESS_BACKEND(Name, Backend)                        \
  {#Name, ExecutionBackendKind::Backend},
#include "fixtures/WindowsAlignmentProcessCases.def"
#undef NEVERD_ALIGNMENT_PROCESS_BACKEND
};
void PrintTo(const Profile &P, std::ostream *OS) { *OS << P.Name; }
class WindowsAlignment : public testing::TestWithParam<Profile> {};
TEST(WindowsAlignmentMapping, RejectsUnclassifiedAndInconsistentFaults) {
  using windows_process::ExceptionDispatcher;
  BackendFault Fault{BackendFaultKind::Interrupt, FaultPC};
  Fault.Interrupt = GeneralProtectionVector;
  Fault.ErrorCode = 0;
  Fault.Cause = BackendFaultCause::OperandAlignment;
  const auto Mapped =
      ExceptionDispatcher::exception(GuestArchitecture::X64, Fault);
  ASSERT_TRUE(Mapped);
  EXPECT_EQ(Mapped->Code, AccessViolation);
  EXPECT_EQ(Mapped->Flags, 0u);
  EXPECT_EQ(Mapped->Address, FaultPC);
  EXPECT_EQ(Mapped->Arguments, (std::vector<uint64_t>{0, UINT64_MAX}));
  EXPECT_TRUE(ExceptionDispatcher::recoverable(GuestArchitecture::X64, Fault));
  EXPECT_FALSE(
      ExceptionDispatcher::exception(GuestArchitecture::AArch64, Fault));
#define NEVERD_ALIGNMENT_INVALID_FAULT(Name, Member, Value)                    \
  {                                                                            \
    SCOPED_TRACE(#Name);                                                       \
    auto Invalid = Fault;                                                      \
    Invalid.Member = Value;                                                    \
    EXPECT_FALSE(                                                              \
        ExceptionDispatcher::exception(GuestArchitecture::X64, Invalid));      \
    EXPECT_FALSE(                                                              \
        ExceptionDispatcher::recoverable(GuestArchitecture::X64, Invalid));    \
  }
#include "fixtures/WindowsAlignmentProcessCases.def"
#undef NEVERD_ALIGNMENT_INVALID_FAULT
}
TEST_P(WindowsAlignment, ExecutesOriginalFaultAndRetryScenarios) {
#ifndef NEVERD_WINDOWS_EXCEPTION_FIXTURE_DIR
  GTEST_SKIP() << MissingTools;
#else
  ExecutionConfiguration Config;
  Config.Architecture = GuestArchitecture::X64;
  Config.Backend = GetParam().Backend;
  Config.Contract = ExecutionContract::CheckedUserX64;
  Config.Privilege = ExecutionPrivilege::User;
  auto Probe = probeExecutionBackend(Config);
  ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
  if (Probe->Availability != BackendAvailability::Available)
    GTEST_SKIP() << Probe->Reason;
  ProcessOptions Options;
  Options.Backend = GetParam().Backend;
  Options.Limits.Instructions = InstructionLimit;
  Options.Limits.TimeoutMicroseconds = TimeoutMicroseconds;
  const auto Path =
      std::filesystem::path(NEVERD_WINDOWS_EXCEPTION_FIXTURE_DIR) / X64Dir /
      ProgramFile;
  auto Result = emulateProcess(Path, ProcessProfile::WindowsPE64, Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, ProcessStopReason::Exited)
      << processResultJSON(*Result);
  EXPECT_EQ(Result->ExitStatus, ExitStatus)
      << llvm::toHex(Result->StandardError);
  EXPECT_TRUE(Result->StandardError.empty())
      << llvm::toHex(Result->StandardError);
  expectOutput(Result->StandardOutput);
#endif
}
INSTANTIATE_TEST_SUITE_P(Backends, WindowsAlignment,
                         testing::ValuesIn(Profiles),
                         [](const auto &P) { return P.param.Name; });
TEST(WindowsAlignmentNative, RunsOriginalFaultAndRetryExecutable) {
#if !defined(_WIN32) || !defined(_M_X64)
  GTEST_SKIP() << NativeOnly;
#elif !defined(NEVERD_WINDOWS_EXCEPTION_FIXTURE_DIR)
  FAIL() << MissingTools;
#else
  llvm::SmallString<128> Temporary;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(TempPrefix, Temporary));
  const std::filesystem::path Root(Temporary.c_str());
  auto Cleanup = llvm::scope_exit(
      [&] { llvm::sys::fs::remove_directories(Root.string()); });
  const auto Program =
      (std::filesystem::path(NEVERD_WINDOWS_EXCEPTION_FIXTURE_DIR) / X64Dir /
       ProgramFile)
          .string();
  const auto Output = (Root / StdoutFile).string();
  const auto Error = (Root / StderrFile).string();
  auto Status =
      native_test::observeNativeProcess(Program, Output, Error, TimeoutSeconds);
  ASSERT_TRUE(bool(Status)) << llvm::toString(Status.takeError());
  auto Out = llvm::MemoryBuffer::getFile(Output);
  auto Err = llvm::MemoryBuffer::getFile(Error);
  ASSERT_TRUE(bool(Out));
  ASSERT_TRUE(bool(Err));
  EXPECT_EQ(*Status, ExitStatus) << llvm::toHex((*Err)->getBuffer());
  EXPECT_TRUE((*Err)->getBuffer().empty()) << llvm::toHex((*Err)->getBuffer());
  expectOutput((*Out)->getBuffer());
#endif
}
} // namespace
} // namespace neverd::emulation
