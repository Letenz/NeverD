//===- WindowsDeferredTests.cpp - Deferred loader facts and observation ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessObserver.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

#include <array>
#include <filesystem>
#include <functional>

namespace neverd::emulation {
namespace {
#define NEVERD_DEFERRED_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_DEFERRED_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/WindowsDeferredCases.def"
#undef NEVERD_DEFERRED_TEXT
#undef NEVERD_DEFERRED_VALUE
struct Call {
  const char *Name, *Argument, *Stopped;
};
constexpr Call Calls[] = {
#define NEVERD_DEFERRED_CALL(Name, Argument, Stopped)                          \
  {#Name, Argument, Stopped},
#include "fixtures/WindowsDeferredCases.def"
#undef NEVERD_DEFERRED_CALL
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

class WindowsDeferred : public testing::TestWithParam<Profile> {
protected:
  ProcessOptions Options;
  std::filesystem::path Path;
  void SetUp() override {
#ifndef NEVERD_WINDOWS_DEFERRED_FIXTURE_DIR
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
    Path = std::filesystem::path(NEVERD_WINDOWS_DEFERRED_FIXTURE_DIR) /
           P.Directory / ProgramFile;
    Options.Backend = P.Backend;
    Options.Limits.Instructions = InstructionLimit;
    Options.Limits.TimeoutMicroseconds = TimeoutMicroseconds;
    Options.Arguments = {ProgramFile, QueryArgument};
    Options.Windows.emplace().DeferUnmodeled = true;
#endif
  }
  CPURegister pc() const {
    return GetParam().ISA == GuestArchitecture::X64 ? CPURegister::X64PC
                                                    : CPURegister::AArch64PC;
  }
};

/// Delegates each callback so a test states only the behavior it checks.
struct Observer final : ProcessObserver {
  std::function<llvm::Expected<std::vector<ExecutionWatch>>(ProcessView &)>
      Started = [](ProcessView &) { return std::vector<ExecutionWatch>(); };
  std::function<llvm::Expected<std::optional<std::vector<ExecutionWatch>>>(
      ProcessView &, uint64_t)>
      Watched = [](ProcessView &, uint64_t) { return std::nullopt; };
  unsigned Starts = 0, Watches = 0;
  llvm::Expected<std::vector<ExecutionWatch>>
  started(ProcessView &Process) override {
    ++Starts;
    return Started(Process);
  }
  llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
  watched(ProcessView &Process, uint64_t PC) override {
    ++Watches;
    return Watched(Process, PC);
  }
};

TEST_P(WindowsDeferred, UnmodeledImportsAreRejectedUnlessDeferred) {
  Options.Windows->DeferUnmodeled = false;
  auto Strict = emulateProcess(Path, ProcessProfile::WindowsPE64, Options);
  ASSERT_FALSE(bool(Strict));
  const auto Reason = llvm::toString(Strict.takeError());
  EXPECT_NE(Reason.find(UnmodeledExport), std::string::npos) << Reason;
}

TEST_P(WindowsDeferred, ResolvingOpaqueEntriesNeverExecutesThem) {
  auto Result = emulateProcess(Path, ProcessProfile::WindowsPE64, Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, ExitStatus)
      << llvm::toHex(Result->StandardError);
  EXPECT_TRUE(Result->StandardError.empty());
}

TEST_P(WindowsDeferred, ExecutingAnOpaqueEntryStopsAndNamesIt) {
  for (const auto &C : Calls) {
    SCOPED_TRACE(C.Name);
    Options.Arguments = {ProgramFile, C.Argument};
    auto Result = emulateProcess(Path, ProcessProfile::WindowsPE64, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, ProcessStopReason::UnsupportedService);
    EXPECT_FALSE(Result->ExitStatus);
    EXPECT_NE(Result->Diagnostic.find(C.Stopped), std::string::npos)
        << Result->Diagnostic;
    EXPECT_TRUE(Result->StandardError.empty())
        << llvm::toHex(Result->StandardError);
  }
}

TEST_P(WindowsDeferred, ObserverSeesTheProcessBeforeItsFirstInstruction) {
  Observer O;
  uint64_t Entry = 0;
  O.Started =
      [&](ProcessView &P) -> llvm::Expected<std::vector<ExecutionWatch>> {
    EXPECT_EQ(P.architecture(), GetParam().ISA);
    const auto Modules = P.modules();
    EXPECT_GE(Modules.size(), 5u);
    if (Modules.empty())
      return std::vector<ExecutionWatch>();
    // The process image leads; providers and the opaque module follow.
    EXPECT_TRUE(Modules.front().Main);
    EXPECT_FALSE(Modules.front().Modeled);
    EXPECT_EQ(Modules.front().Base, ProgramBaseAddress);
    EXPECT_EQ(Modules.front().Name, ProgramFile);
    Entry = Modules.front().Entry;
    for (const char *Name : {KernelModule, AbsentModule}) {
      const auto Found =
          llvm::find_if(Modules, [&](const auto &M) { return M.Name == Name; });
      EXPECT_NE(Found, Modules.end()) << Name;
      if (Found != Modules.end()) {
        EXPECT_TRUE(Found->Modeled) << Name;
        EXPECT_FALSE(Found->Main) << Name;
      }
    }
    EXPECT_EQ(llvm::cantFail(P.readRegister(pc()))[0], Entry);
    std::array<uint8_t, HeaderBytes> Header{};
    llvm::cantFail(P.read(ProgramBaseAddress, Header));
    EXPECT_EQ(uint64_t(Header[0] | Header[1] << 8), DOSMagic);
    // Unmapped memory is an error, not zero bytes.
    auto Unmapped = P.read(UnmappedAddress, Header);
    EXPECT_TRUE(bool(Unmapped));
    llvm::consumeError(std::move(Unmapped));
    const auto Mappings = llvm::cantFail(P.mappings());
    EXPECT_TRUE(llvm::any_of(Mappings, [](const AddressMapping &M) {
      return M.Address == ProgramBaseAddress && !M.Device;
    }));
    // Modeled services and opaque entries are both bindable identities.
    const auto Exports = P.exports();
    for (const auto &[Module, Name] : {std::pair{KernelModule, ModeledExport},
                                       std::pair{KernelModule, UnmodeledExport},
                                       std::pair{AbsentModule, AbsentExport}})
      EXPECT_EQ(llvm::count_if(Exports,
                               [&](const ProcessExportView &E) {
                                 return E.Module == Module && E.Name == Name;
                               }),
                1)
          << Module << '!' << Name;
    return std::vector<ExecutionWatch>{{Entry, 1}};
  };
  O.Watched = [&](ProcessView &P, uint64_t PC)
      -> llvm::Expected<std::optional<std::vector<ExecutionWatch>>> {
    EXPECT_EQ(PC, Entry);
    EXPECT_EQ(llvm::cantFail(P.readRegister(pc()))[0], Entry);
    return std::nullopt;
  };
  auto Result = observeProcess(Path, ProcessProfile::WindowsPE64, Options, O);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(O.Starts, 1u);
  EXPECT_EQ(O.Watches, 1u);
  EXPECT_EQ(Result->Stop, ProcessStopReason::Observer);
  EXPECT_EQ(Result->PC, Entry);
  // The watched instruction was never admitted.
  EXPECT_EQ(Result->Instructions, 0u);
  EXPECT_FALSE(Result->ExitStatus);
}

TEST_P(WindowsDeferred, ResumedObservationRunsTheProcessUnchanged) {
  auto Plain = emulateProcess(Path, ProcessProfile::WindowsPE64, Options);
  ASSERT_TRUE(bool(Plain)) << llvm::toString(Plain.takeError());
  Observer O;
  O.Started = [](ProcessView &P) {
    return std::vector<ExecutionWatch>{{P.modules().front().Entry, 1}};
  };
  // An empty watch set resumes without any further stop.
  O.Watched = [](ProcessView &, uint64_t) {
    return std::optional(std::vector<ExecutionWatch>());
  };
  auto Result = observeProcess(Path, ProcessProfile::WindowsPE64, Options, O);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(O.Watches, 1u);
  EXPECT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, Plain->ExitStatus);
  EXPECT_EQ(Result->Instructions, Plain->Instructions);
  EXPECT_EQ(Result->Events, Plain->Events);
}

TEST_P(WindowsDeferred, ObserverFailuresAreNotGuestOutcomes) {
  Observer O;
  O.Started = [](ProcessView &) -> llvm::Expected<std::vector<ExecutionWatch>> {
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   ObserverFailure);
  };
  auto Refused = observeProcess(Path, ProcessProfile::WindowsPE64, Options, O);
  ASSERT_FALSE(bool(Refused));
  EXPECT_EQ(llvm::toString(Refused.takeError()), ObserverFailure);

  Observer Invalid;
  Invalid.Started = [](ProcessView &) {
    return std::vector<ExecutionWatch>{{ProgramBaseAddress, 0}};
  };
  auto Rejected =
      observeProcess(Path, ProcessProfile::WindowsPE64, Options, Invalid);
  EXPECT_FALSE(bool(Rejected));
  llvm::consumeError(Rejected.takeError());

  Observer Later;
  Later.Started = [](ProcessView &P) {
    return std::vector<ExecutionWatch>{{P.modules().front().Entry, 1}};
  };
  Later.Watched = [](ProcessView &, uint64_t)
      -> llvm::Expected<std::optional<std::vector<ExecutionWatch>>> {
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   ObserverFailure);
  };
  auto Failed =
      observeProcess(Path, ProcessProfile::WindowsPE64, Options, Later);
  ASSERT_TRUE(bool(Failed)) << llvm::toString(Failed.takeError());
  EXPECT_EQ(Failed->Stop, ProcessStopReason::RuntimeFailure);
  EXPECT_EQ(Failed->Diagnostic, ObserverFailure);
}

TEST_P(WindowsDeferred, ProfilesWithoutObservationRefuseAnObserver) {
  Observer O;
  Options.Windows.reset();
  for (auto Profile :
       {ProcessProfile::LinuxELF64, ProcessProfile::MacOSMachO64}) {
    auto Result = observeProcess(Path, Profile, Options, O);
    EXPECT_FALSE(bool(Result));
    llvm::consumeError(Result.takeError());
  }
  EXPECT_EQ(O.Starts, 0u);
}

INSTANTIATE_TEST_SUITE_P(Backends, WindowsDeferred, testing::ValuesIn(Profiles),
                         [](const testing::TestParamInfo<Profile> &P) {
                           return P.param.Name;
                         });
} // namespace
} // namespace neverd::emulation
