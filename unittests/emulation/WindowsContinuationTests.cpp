//===- WindowsContinuationTests.cpp - Vectored continuation contracts -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "gtest/gtest.h"
#include "os/windows/process/WindowsProcessExceptions.h"

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

#include <array>

namespace neverd::emulation {
namespace {
namespace win = windows_process;
#define NEVERD_VEH_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_VEH_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/WindowsExceptionCases.def"
#undef NEVERD_VEH_TEXT
#undef NEVERD_VEH_VALUE
#define NEVERD_VCH_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_VCH_TEXT(Name, Text) constexpr char Name[] = Text;
#define NEVERD_VCH_MODE(Name, Value) constexpr uint8_t Name = Value;
#include "fixtures/WindowsContinuationCases.def"
#undef NEVERD_VCH_MODE
#undef NEVERD_VCH_TEXT
#undef NEVERD_VCH_VALUE

struct Case {
  const char *Name, *Argument;
  uint64_t Trace, Exceptions, Continuations;
};
constexpr Case Cases[] = {
#define NEVERD_VCH_CASE(Name, Argument, Trace, Exceptions, Continuations)      \
  {#Name, Argument, Trace, Exceptions, Continuations},
#include "fixtures/WindowsContinuationCases.def"
#undef NEVERD_VCH_CASE
};
std::string expected(const Case &C) {
  const uint64_t Values[] = {uint8_t(C.Argument[1]), C.Trace, C.Exceptions,
                             C.Continuations};
  static_assert(std::size(Values) == ObservationWords);
  std::array<uint8_t, sizeof(Values)> Bytes;
  for (size_t I = 0; I < std::size(Values); ++I)
    llvm::support::endian::write64le(Bytes.data() + I * sizeof(uint64_t),
                                     Values[I]);
  return llvm::toHex(Bytes);
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
class WindowsContinuations : public testing::TestWithParam<Profile> {
protected:
  ExecutionConfiguration Config;
  ProcessOptions Options;
  std::filesystem::path Path;
  void SetUp() override {
#ifndef NEVERD_WINDOWS_EXCEPTION_FIXTURE_DIR
    if (requireHvf(GetParam().Backend, GetParam().ISA))
      FAIL() << MissingTools;
    GTEST_SKIP() << MissingTools;
#else
    const auto &P = GetParam();
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
    Path = std::filesystem::path(NEVERD_WINDOWS_EXCEPTION_FIXTURE_DIR) /
           P.Directory / ContinueProgramFile;
    Options.Backend = P.Backend;
    Options.Windows.emplace();
    Options.Windows->Modules.push_back(
        {LibraryFile, Path.parent_path() / LibraryFile});
    Options.Limits.Instructions = InstructionLimit;
    Options.Limits.TimeoutMicroseconds = TimeoutMicroseconds;
#endif
  }
};
TEST_P(WindowsContinuations, ExecutesOriginalContinuationScenarios) {
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Name);
    Options.Arguments = {ContinueProgramFile, C.Argument};
    auto R = emulateProcess(Path, ProcessProfile::WindowsPE64, Options);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
    EXPECT_EQ(R->ExitStatus, CompletionStatus) << llvm::toHex(R->StandardError);
    EXPECT_TRUE(R->StandardError.empty()) << llvm::toHex(R->StandardError);
    EXPECT_EQ(llvm::toHex(R->StandardOutput), expected(C));
  }
}
TEST_P(WindowsContinuations, RejectsInvalidCallbacksAndContinuationState) {
  struct Negative {
    const char *Argument, *Diagnostic;
  };
  const Negative NegativeCases[] = {
#define NEVERD_VCH_NEGATIVE(Argument, Diagnostic)                              \
  {Argument, win::text::Diagnostic},
#include "fixtures/WindowsContinuationCases.def"
#undef NEVERD_VCH_NEGATIVE
  };
  for (const auto &C : NegativeCases) {
    SCOPED_TRACE(C.Argument);
    Options.Arguments = {ContinueProgramFile, C.Argument};
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
TEST_P(WindowsContinuations, NestedCallbacksShareTheProcessEventBudget) {
  Options.Arguments = {ContinueProgramFile, RecursiveArgument};
  Options.Limits.Events = SmallEventLimit;
  auto R = emulateProcess(Path, ProcessProfile::WindowsPE64, Options);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->Stop, ProcessStopReason::EventLimit) << R->Diagnostic;
  EXPECT_EQ(R->Events, SmallEventLimit);
  EXPECT_FALSE(R->ExitStatus);
  EXPECT_TRUE(R->StandardOutput.empty());
  EXPECT_TRUE(R->StandardError.empty());
}
TEST_P(WindowsContinuations, HandlerNamespacesShareAndReclaimCapacity) {
  auto RAM = llvm::cantFail(PhysicalMemory::create(Options.MemoryLimit));
  auto Space = llvm::cantFail(AddressSpace::create(RAM, Options.MemoryLimit));
  auto Backend = llvm::cantFail(createExecutionBackend(Config, Space));
  auto ABI =
      llvm::cantFail(IntegerABI::get(GetParam().ISA == GuestArchitecture::X64
                                         ? IntegerCallingConvention::Win64
                                         : IntegerCallingConvention::AAPCS64));
  win::VectoredExceptions Handlers(*Backend.CPU, ABI,
                                   win::value::StackTop - Options.StackSize);
  using Kind = win::VectoredExceptions::HandlerKind;
  uint64_t Last = 0;
  for (unsigned Round = 0; Round < 2; ++Round) {
    std::vector<std::pair<Kind, uint64_t>> Handles;
    for (unsigned I = 0; I < ModelHandlerLimit; ++I) {
      const auto K = I % 2 ? Kind::Exception : Kind::Continue;
      auto H = Handlers.add(K, false, win::value::GateBase);
      ASSERT_TRUE(bool(H)) << llvm::toString(H.takeError());
      EXPECT_GT(*H, Last);
      Last = *H;
      Handles.push_back({K, *H});
    }
    auto Full = Handlers.add(Kind::Continue, false, win::value::GateBase);
    ASSERT_FALSE(bool(Full));
    EXPECT_NE(llvm::toString(Full.takeError()).find(win::text::ExceptionLimit),
              std::string::npos);
    for (const auto &[K, H] : Handles) {
      const auto Other =
          K == Kind::Exception ? Kind::Continue : Kind::Exception;
      EXPECT_EQ(Handlers.remove(Other, H), 0u);
      EXPECT_EQ(Handlers.remove(K, H), 1u);
      EXPECT_EQ(Handlers.remove(K, H), 0u);
    }
  }
}
INSTANTIATE_TEST_SUITE_P(Backends, WindowsContinuations,
                         testing::ValuesIn(Profiles),
                         [](const auto &P) { return P.param.Name; });

TEST(WindowsContinuationsNative, RunsOriginalContinuationExecutable) {
#if !defined(_WIN32) || !defined(_M_X64)
  GTEST_SKIP() << NativeOnly;
#elif !defined(NEVERD_WINDOWS_EXCEPTION_FIXTURE_DIR)
  FAIL() << MissingTools;
#else
  llvm::SmallString<128> Temporary;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory(ContinuationTempPrefix, Temporary));
  const std::filesystem::path Root(Temporary.c_str());
  auto Cleanup = llvm::scope_exit(
      [&] { llvm::sys::fs::remove_directories(Root.string()); });
  const auto Program =
      (std::filesystem::path(NEVERD_WINDOWS_EXCEPTION_FIXTURE_DIR) / X64Dir /
       ContinueProgramFile)
          .string();
  const auto Output = (Root / StdoutFile).string();
  const auto Error = (Root / StderrFile).string();
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Output,
                                                      Error};
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Name);
    const unsigned Repetitions =
        C.Argument[1] == MutationMode ? NativeMutationRepetitions : 1;
    for (unsigned I = 0; I < Repetitions; ++I) {
      SCOPED_TRACE(I);
      std::string Diagnostic;
      bool Failed = false;
      int Status = llvm::sys::ExecuteAndWait(
          Program, {Program, C.Argument}, std::nullopt, Redirects,
          NativeTimeoutSeconds, 0, &Diagnostic, &Failed);
      ASSERT_FALSE(Failed) << Diagnostic;
      auto Out = llvm::MemoryBuffer::getFile(Output);
      auto Err = llvm::MemoryBuffer::getFile(Error);
      ASSERT_TRUE(bool(Out));
      ASSERT_TRUE(bool(Err));
      llvm::outs() << ContinuationLabel << C.Argument << ' ' << Status << ' '
                   << llvm::toHex((*Out)->getBuffer()) << '\n';
      EXPECT_EQ(Status, CompletionStatus) << llvm::toHex((*Err)->getBuffer());
      EXPECT_TRUE((*Err)->getBuffer().empty())
          << llvm::toHex((*Err)->getBuffer());
      EXPECT_EQ(llvm::toHex((*Out)->getBuffer()), expected(C));
    }
  }
#endif
}
} // namespace
} // namespace neverd::emulation
