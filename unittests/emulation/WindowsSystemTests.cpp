//===- WindowsSystemTests.cpp - System image and native oracle tests ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "WindowsNativeTestSupport.h"
#include "gtest/gtest.h"
#include "os/windows/process/WindowsProcessModules.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <initializer_list>

namespace neverd::emulation {
namespace {
namespace win = windows_process;
#define NEVERD_SYSTEM_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_SYSTEM_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/WindowsSystemCases.def"
#undef NEVERD_SYSTEM_TEXT
#undef NEVERD_SYSTEM_VALUE
struct Case {
  const char *Name, *Argument, *Expected;
};
constexpr Case Cases[] = {
#define NEVERD_SYSTEM_CASE(Name, Argument, Expected)                           \
  {#Name, Argument, Expected},
#include "fixtures/WindowsSystemCases.def"
#undef NEVERD_SYSTEM_CASE
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
class WindowsSystem : public testing::TestWithParam<Profile> {
protected:
  ExecutionConfiguration Config;
  ProcessOptions Options;
  std::filesystem::path Path;
  void SetUp() override {
#ifndef NEVERD_WINDOWS_SYSTEM_FIXTURE_DIR
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
    Path = std::filesystem::path(NEVERD_WINDOWS_SYSTEM_FIXTURE_DIR) /
           P.Directory / ProgramFile;
    Options.Backend = P.Backend;
    Options.Windows.emplace();
    Options.Windows->Modules.push_back(
        {ForwardFile, Path.parent_path() / ForwardFile});
    Options.Limits.Instructions = InstructionLimit;
    Options.Limits.TimeoutMicroseconds = TimeoutMicroseconds;
#endif
  }
};

TEST_P(WindowsSystem, ExecutesOriginalSystemModuleScenarios) {
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Name);
    Options.Arguments = {ProgramFile, C.Argument};
    auto R = emulateProcess(Path, ProcessProfile::WindowsPE64, Options);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
    EXPECT_EQ(R->ExitStatus, ExitStatus) << llvm::toHex(R->StandardError);
    EXPECT_TRUE(R->StandardError.empty());
    EXPECT_EQ(llvm::toHex(R->StandardOutput), C.Expected);
    if (llvm::StringRef(C.Argument) == ReturnArgument)
      EXPECT_EQ(R->ReturnValue, ExitStatus);
  }
}
TEST_P(WindowsSystem, RejectsUnmodeledExportsOrdinalsAndChangedImages) {
  struct Negative {
    const char *Argument, *Diagnostic;
  };
  const Negative Cases[] = {
#define NEVERD_SYSTEM_NEGATIVE(Argument, Diagnostic)                           \
  {Argument, win::text::Diagnostic},
#include "fixtures/WindowsSystemCases.def"
#undef NEVERD_SYSTEM_NEGATIVE
  };
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Argument);
    Options.Arguments = {ProgramFile, C.Argument};
    auto R = emulateProcess(Path, ProcessProfile::WindowsPE64, Options);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::RuntimeFailure) << R->Diagnostic;
    EXPECT_NE(R->Diagnostic.find(C.Diagnostic), std::string::npos);
    EXPECT_FALSE(R->ExitStatus);
    EXPECT_TRUE(R->StandardOutput.empty());
    EXPECT_TRUE(R->StandardError.empty());
    ASSERT_FALSE(R->NativeCalls.empty());
    EXPECT_FALSE(R->NativeCalls.back().Result);
  }
}
TEST_P(WindowsSystem, ProviderImagesUseSharedMappingsAndExportResolution) {
  auto RAM = llvm::cantFail(PhysicalMemory::create(Options.MemoryLimit));
  auto Space = llvm::cantFail(AddressSpace::create(RAM, Options.MemoryLimit));
  win::VirtualMemory Virtual(*Space, Options);
  auto Budget = llvm::cantFail(ExecutionBudget::create(Options.Limits));
  // A main image cannot shadow the reserved provider namespace, even when its
  // basename has been supplied outside the explicit DLL catalogue.
  auto Conflict = win::loadProgram(Path.parent_path() / Kernel32Name, Options,
                                   *Budget, Virtual);
  ASSERT_FALSE(bool(Conflict));
  EXPECT_NE(llvm::toString(Conflict.takeError()).find(win::text::ModuleName),
            std::string::npos);
  auto P = win::loadProgram(Path, Options, *Budget, Virtual);
  ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
  for (const auto &M : P->Modules)
    for (const auto &R : M.Loaded.Regions) {
      ASSERT_FALSE(bool(Space->map(R.Address, R.Bytes.size(),
                                   Read | Write | UserAccessible)));
      ASSERT_FALSE(bool(Space->write(R.Address, R.Bytes)));
      ASSERT_FALSE(
          bool(Space->protect(R.Address, R.Bytes.size(), R.Permissions)));
    }
  auto Backend = createExecutionBackend(Config, Space);
  ASSERT_TRUE(bool(Backend)) << llvm::toString(Backend.takeError());
  const char *Providers[] = {Kernel32Name, KernelBaseName, NtdllName};
  for (const char *Name : Providers) {
    auto I = win::findModule(*P, Name);
    ASSERT_TRUE(I);
    const auto &M = P->Modules[*I];
    EXPECT_TRUE(M.System && M.Pinned && !M.Attached);
    EXPECT_EQ(M.State, win::ModuleState::Ready);
    EXPECT_EQ(M.Loaded.Entry, 0u);
    EXPECT_EQ(M.Loaded.TLSIndex, 0u);
    auto Info = llvm::cantFail(Virtual.query(M.Loaded.Base));
    ASSERT_TRUE(Info);
    EXPECT_EQ(Info->Type, MemImage);
    EXPECT_EQ(Info->State, MemCommit);
    EXPECT_EQ(Info->AllocationBase, M.Loaded.Base);
    EXPECT_FALSE(
        llvm::cantFail(Backend->CPU->canAccess(M.Loaded.Base, 1, Write)));
    EXPECT_FALSE(
        llvm::cantFail(Backend->CPU->canAccess(M.Loaded.Base, 1, Execute)));
    auto Ref = win::moduleRef(*P, *I);
    auto E = win::retireModule(*P, Ref, Virtual);
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
    EXPECT_TRUE(win::current(*P, Ref));
  }
  for (const auto &Gate : P->Gates) {
    auto Index = win::findModule(*P, Gate.Module);
    ASSERT_TRUE(Index);
    const auto &Image = P->Modules[*Index].Loaded;
    EXPECT_GE(Gate.Gate, Image.Base);
    EXPECT_LT(Gate.Gate, Image.Base + Image.Size);
    // Enumerating the entire advertised catalogue checks identity lookup.
    // Authenticate mapped metadata for each implemented service below;
    // repeating that scan for thousands of opaque names exhausts the shared
    // work budget without testing a different mapping.
    auto Address =
        win::resolveExport(*P, *Index, Gate.Name, std::nullopt, *Budget,
                           Gate.Target ? Backend->CPU.get() : nullptr);
    ASSERT_TRUE(bool(Address)) << llvm::toString(Address.takeError());
    EXPECT_EQ(Address->Address, Gate.Gate);
    EXPECT_TRUE(llvm::cantFail(
        Backend->CPU->canAccess(Gate.Gate, 1, Execute | UserAccessible)));
    EXPECT_FALSE(llvm::cantFail(Backend->CPU->canAccess(Gate.Gate, 1, Write)));
  }
  for (const auto &Import : P->Modules.front().Loaded.Imports) {
    ASSERT_NE(Import.Target, nullptr);
    auto Index = win::findModule(*P, Import.Module);
    ASSERT_TRUE(Index);
    auto Address = win::resolveExport(*P, *Index, Import.Name, std::nullopt,
                                      *Budget, Backend->CPU.get());
    ASSERT_TRUE(bool(Address)) << llvm::toString(Address.takeError());
    EXPECT_EQ(Address->Address, Import.Gate);
  }
  P->Reads.MetadataBytes = 0;
  auto Index = win::findModule(*P, Kernel32Name);
  ASSERT_TRUE(Index);
  auto Exhausted = win::resolveExport(*P, *Index, HeapName, std::nullopt,
                                      *Budget, Backend->CPU.get());
  ASSERT_FALSE(bool(Exhausted));
  EXPECT_NE(llvm::toString(Exhausted.takeError()).find(win::text::ExportBudget),
            std::string::npos);
}
INSTANTIATE_TEST_SUITE_P(Backends, WindowsSystem, testing::ValuesIn(Profiles),
                         [](const auto &P) { return P.param.Name; });

TEST(WindowsSystemNative, RunsOriginalSystemModuleExecutable) {
#if !defined(_WIN32) || !defined(_M_X64)
  GTEST_SKIP() << NativeOnly;
#elif !defined(NEVERD_WINDOWS_SYSTEM_FIXTURE_DIR)
  FAIL() << MissingTools;
#else
  llvm::SmallString<128> Temporary;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(TempPrefix, Temporary));
  const std::filesystem::path Root(Temporary.c_str());
  auto Cleanup = llvm::scope_exit(
      [&] { llvm::sys::fs::remove_directories(Root.string()); });
  const auto Program =
      (std::filesystem::path(NEVERD_WINDOWS_SYSTEM_FIXTURE_DIR) / X64Dir /
       ProgramFile)
          .string();
  const auto Output = (Root / StdoutFile).string();
  const auto Error = (Root / StderrFile).string();
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Output,
                                                      Error};
  for (const auto &C : Cases) {
    const bool Returns = llvm::StringRef(C.Argument) == ReturnArgument;
    for (unsigned I = 0; I < (Returns ? RepeatCount : 1); ++I) {
      SCOPED_TRACE(C.Name);
      int Status;
      if (Returns) {
        auto Observed = native_test::observeNativeThread(
            Program, C.Argument, Output, Error, NativeTimeoutSeconds);
        ASSERT_TRUE(bool(Observed)) << llvm::toString(Observed.takeError());
        Status = *Observed;
      } else {
        std::string Diagnostic;
        bool Failed = false;
        Status = llvm::sys::ExecuteAndWait(
            Program, {Program, C.Argument}, std::nullopt, Redirects,
            NativeTimeoutSeconds, 0, &Diagnostic, &Failed);
        ASSERT_FALSE(Failed) << Diagnostic;
      }
      auto Out = llvm::MemoryBuffer::getFile(Output);
      auto Err = llvm::MemoryBuffer::getFile(Error);
      ASSERT_TRUE(bool(Out));
      ASSERT_TRUE(bool(Err));
      llvm::outs() << ObservationLabel << C.Argument << ' ' << Status << ' '
                   << llvm::toHex((*Out)->getBuffer()) << '\n';
      EXPECT_EQ(Status, ExitStatus) << llvm::toHex((*Err)->getBuffer());
      EXPECT_TRUE((*Err)->getBuffer().empty());
      EXPECT_EQ(llvm::toHex((*Out)->getBuffer()), C.Expected);
    }
  }
#endif
}
} // namespace
} // namespace neverd::emulation
