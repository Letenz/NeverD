//===- UnpackLibraryTests.cpp - Recover DLLs through the guest loader
//------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "UnpackLibraryTestSupport.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessObserver.h"
#include "neverd/unpack/Unpack.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"

namespace neverd::unpack {
namespace {
using namespace emulation;
namespace fixture = test::library;
struct Profile {
  const char *Name;
  ExecutionBackendKind Backend;
  GuestArchitecture ISA;
  ExecutionContract Contract;
  const char *Directory;
};
constexpr Profile Profiles[] = {
#define NEVERD_USER_PROFILE(Name, Backend, ISA, Contract)                      \
  {#Name, ExecutionBackendKind::Backend, GuestArchitecture::ISA,               \
   ExecutionContract::Contract, #ISA},
#include "UserExecutionCases.def"
#undef NEVERD_USER_PROFILE
    {"UnicornDirectX64", ExecutionBackendKind::Unicorn, GuestArchitecture::X64,
     ExecutionContract::DirectUserX64, "X64"},
    {"KvmDirectX64", ExecutionBackendKind::KVM, GuestArchitecture::X64,
     ExecutionContract::DirectUserX64, "X64"},
    {"WhpDirectX64", ExecutionBackendKind::WHP, GuestArchitecture::X64,
     ExecutionContract::DirectUserX64, "X64"},
};
void PrintTo(const Profile &P, std::ostream *OS) { *OS << P.Name; }
class UnpackLibrary : public testing::TestWithParam<Profile> {
protected:
  void SetUp() override {
#ifndef NEVERD_UNPACK_LIBRARY_FIXTURE_DIR
    GTEST_SKIP() << fixture::MissingTools;
#else
    const auto &P = GetParam();
    ExecutionConfiguration Config;
    Config.Backend = P.Backend;
    Config.Architecture = P.ISA;
    Config.Contract = P.Contract;
    auto Probe = probeExecutionBackend(Config);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available) {
      if (requireHvf(P.Backend, P.ISA))
        FAIL() << Probe->Reason;
      GTEST_SKIP() << Probe->Reason;
    }
    Directory =
        std::filesystem::path(NEVERD_UNPACK_LIBRARY_FIXTURE_DIR) / P.Directory;
    Original = test::readImage(Directory / fixture::InputFile);
    ASSERT_FALSE(HasFailure());
    llvm::SmallString<128> Created;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-dll", Created));
    Scratch = Created.str().str();
    for (const char *Name : {fixture::DependencyFile, fixture::HostFile})
      std::filesystem::copy_file(Directory / Name, Scratch / Name);
    Options.Process.Backend = P.Backend;
    Options.Process.Contract = P.Contract;
    Options.Process.Limits.Instructions = fixture::InstructionLimit;
    Options.Process.Limits.TimeoutMicroseconds = fixture::TimeoutMicroseconds;
    Options.Process.Windows->Modules.push_back(
        {fixture::DependencyFile, Directory / fixture::DependencyFile});
#endif
  }
  void TearDown() override {
    if (!Scratch.empty())
      std::filesystem::remove_all(Scratch);
  }
  void expectExecution(const std::filesystem::path &Path,
                       uint32_t ExitStatus = 0) {
    auto Run =
        emulateProcess(Path, ProcessProfile::WindowsPE64, Options.Process);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    EXPECT_EQ(Run->Stop, ProcessStopReason::Exited) << Run->Diagnostic;
    EXPECT_EQ(Run->ExitStatus, ExitStatus);
    EXPECT_TRUE(Run->StandardError.empty());
    EXPECT_EQ(
        std::string(Run->StandardOutput.begin(), Run->StandardOutput.end()),
        std::string(fixture::AttachText) + fixture::DetachText);
  }
  std::filesystem::path Directory, Scratch;
  test::Image Original;
  UnpackOptions Options;
};

TEST_P(UnpackLibrary, RecoversAttachEntryAndPreservesExports) {
  expectExecution(Directory / fixture::InputFile);
  for (uint32_t Mode :
       {fixture::LoaderMode, fixture::TLSMode, fixture::PrivateTLSMode}) {
    SCOPED_TRACE(Mode);
    const auto Path = Scratch / fixture::InputFile;
    ASSERT_TRUE(fixture::write(Path, fixture::pack(Original, Mode)));
    auto Recovered = unpackFile(Path, Options);
    ASSERT_TRUE(bool(Recovered)) << llvm::toString(Recovered.takeError());
    ASSERT_EQ(Recovered->Outcome, UnpackOutcome::Unpacked)
        << Recovered->ProcessDiagnostic;
    EXPECT_EQ(Recovered->EntryRVA, Original.Entry);
    EXPECT_EQ(Recovered->Source, EntrySource::Transfer);
    EXPECT_EQ(Recovered->MaterializedTLSCallbacks, 1u);
    ASSERT_FALSE(Recovered->Transfers.empty());
    EXPECT_TRUE(Recovered->Transfers.back().StackBalanced);
    EXPECT_TRUE(Recovered->Transfers.back().ProgramInvocation);
    if (Mode == fixture::TLSMode) {
      ASSERT_GT(Recovered->Transfers.size(), 1u);
      EXPECT_FALSE(Recovered->Transfers.front().ProgramInvocation);
    }
    if (Mode == fixture::PrivateTLSMode) {
      ASSERT_GT(Recovered->Transfers.size(), 1u);
      EXPECT_TRUE(Recovered->Transfers.front().ProgramInvocation);
      EXPECT_FALSE(Recovered->Transfers.front().StackBalanced);
    }
    const auto Image = test::readImage(Recovered->Image);
    EXPECT_TRUE(Image.FileCharacteristics & llvm::COFF::IMAGE_FILE_DLL);
    EXPECT_EQ(Image.Exports, Original.Exports);
    EXPECT_TRUE(llvm::none_of(Recovered->Imports, [](const auto &I) {
      return I.Module == fixture::InputFile;
    }));
    EXPECT_EQ(
        Image.directory(llvm::COFF::EXPORT_TABLE).RelativeVirtualAddress,
        Original.directory(llvm::COFF::EXPORT_TABLE).RelativeVirtualAddress);
    if (GetParam().ISA == GuestArchitecture::X64)
      EXPECT_GE(Recovered->ImportRepair.RepairedCalls, 2u)
          << Recovered->ImportRepair.Diagnostic;
    ASSERT_TRUE(fixture::write(Path, Recovered->Image));
    expectExecution(Path);
  }
}

TEST_P(UnpackLibrary, OriginalAndNoEntryLibrariesDoNotInventAnEntry) {
  for (bool NoEntry : {false, true}) {
    SCOPED_TRACE(NoEntry);
    auto Bytes = Original.File;
    if (NoEntry)
      llvm::support::endian::write32le(Bytes.data() + Original.EntryOffset, 0);
    const auto Path = Scratch / fixture::InputFile;
    ASSERT_TRUE(fixture::write(Path, Bytes));
    auto Result = unpackFile(Path, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Outcome, UnpackOutcome::NoEntry);
    EXPECT_FALSE(Result->EntryRVA);
    EXPECT_TRUE(Result->Image.empty());
    EXPECT_TRUE(Result->Transfers.empty());
    EXPECT_EQ(Result->ProcessStop,
              processStopReasonName(ProcessStopReason::Exited))
        << Result->ProcessDiagnostic;
  }
}

TEST_P(UnpackLibrary, WrappedEntriesRequireExplicitTransferEvidence) {
  const auto Path = Scratch / fixture::InputFile;
  ASSERT_TRUE(
      fixture::write(Path, fixture::pack(Original, fixture::WrappedEntryMode)));
  auto Default = unpackFile(Path, Options);
  ASSERT_TRUE(bool(Default)) << llvm::toString(Default.takeError());
  EXPECT_EQ(Default->Outcome, UnpackOutcome::NoEntry);
  EXPECT_TRUE(Default->Image.empty());
  EXPECT_EQ(Default->ProcessStop,
            processStopReasonName(ProcessStopReason::Exited))
      << Default->ProcessDiagnostic;
  ASSERT_FALSE(Default->Transfers.empty());
  EXPECT_FALSE(Default->Transfers.front().StackBalanced);
  EXPECT_TRUE(Default->Transfers.front().ProgramInvocation);

  Options.Transfer = 1;
  auto Selected = unpackFile(Path, Options);
  ASSERT_TRUE(bool(Selected)) << llvm::toString(Selected.takeError());
  ASSERT_EQ(Selected->Outcome, UnpackOutcome::Unpacked)
      << Selected->ProcessDiagnostic;
  EXPECT_EQ(Selected->EntryRVA, Original.Entry);
  EXPECT_EQ(Selected->MaterializedTLSCallbacks, 1u);
  ASSERT_TRUE(fixture::write(Path, Selected->Image));
  expectExecution(Path);
}

TEST_P(UnpackLibrary, FailedAttachUsesOrdinaryLoaderCleanup) {
  auto Bytes = Original.File;
  const auto *Record = Original.section(fixture::RecordSection);
  ASSERT_NE(Record, nullptr);
  llvm::support::endian::write32le(Bytes.data() + Record->FileOffset + 12, 1);
  const auto Path = Scratch / fixture::InputFile;
  ASSERT_TRUE(fixture::write(Path, Bytes));
  expectExecution(Path, 1114);
  auto Result = unpackFile(Path, Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Outcome, UnpackOutcome::NoEntry);
  EXPECT_TRUE(Result->Image.empty());
  EXPECT_EQ(Result->ProcessStop,
            processStopReasonName(ProcessStopReason::Exited));
}

TEST_P(UnpackLibrary, InputIdentityIsDistinctFromTheHostProcess) {
  class Observer final : public ProcessObserver {
  public:
    unsigned Entries = 0;
    bool SawMappedInput = false, SawUnloadedInput = false;
    llvm::Expected<std::vector<ExecutionWatch>>
    started(ProcessView &P) override {
      EXPECT_FALSE(P.inputModule());
      EXPECT_FALSE(P.programInvocation());
      const auto Modules = P.modules();
      expectMain(Modules);
      return std::vector<ExecutionWatch>();
    }
    void expectMain(llvm::ArrayRef<ProcessModuleView> Modules) {
      EXPECT_FALSE(Modules.empty());
      if (!Modules.empty()) {
        EXPECT_TRUE(Modules.front().Main);
        EXPECT_TRUE(Modules.front().Modeled);
      }
    }
    llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
    invoking(ProcessView &P) override {
      const auto Input = P.inputModule();
      if (Input) {
        SawMappedInput = true;
        EXPECT_FALSE(Input->Main);
        EXPECT_FALSE(Input->Modeled);
        EXPECT_EQ(Input->Name, fixture::InputFile);
        EXPECT_NE(Input->Base, P.modules().front().Base);
      } else if (SawMappedInput)
        SawUnloadedInput = true;
      if (P.programInvocation()) {
        ++Entries;
        EXPECT_TRUE(Input);
        EXPECT_EQ(P.completedInitializers().size(), 1u);
        auto TLS = P.threadLocalMemory();
        EXPECT_TRUE(bool(TLS));
        if (TLS)
          EXPECT_TRUE(*TLS);
        else
          llvm::consumeError(TLS.takeError());
      }
      return std::nullopt;
    }
    llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
    watched(ProcessView &, uint64_t) override {
      return std::nullopt;
    }
  } Observer;
  auto Result =
      observeProcess(Directory / fixture::InputFile,
                     ProcessProfile::WindowsPE64, Options.Process, Observer);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  EXPECT_EQ(Result->ExitStatus, 0u);
  EXPECT_GE(Observer.Entries, 1u);
  EXPECT_TRUE(Observer.SawMappedInput);
  EXPECT_TRUE(Observer.SawUnloadedInput);
}

TEST_P(UnpackLibrary, RecoveredLibrariesLoadAndExportOnNativeWindows) {
#if !defined(_WIN32) || !defined(_M_X64)
  GTEST_SKIP() << "requires native Windows x64 DLL loading";
#else
  if (GetParam().ISA != GuestArchitecture::X64)
    GTEST_SKIP() << "native host architecture differs";
  const auto Path = Scratch / fixture::InputFile;
  auto Native = [&](uint32_t ExitStatus, llvm::StringRef Expected) {
    const auto Host = (Scratch / fixture::HostFile).string();
    const auto Output = (Scratch / "stdout").string();
    const auto Error = (Scratch / "stderr").string();
    const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Output,
                                                        Error};
    std::string Diagnostic;
    bool Failed = false;
    const int Status = llvm::sys::ExecuteAndWait(
        Host, {Host}, std::nullopt, Redirects, 30, 0, &Diagnostic, &Failed);
    ASSERT_FALSE(Failed) << Diagnostic;
    EXPECT_EQ(Status, ExitStatus);
    const auto Out = test::readFile(Output), Err = test::readFile(Error);
    EXPECT_TRUE(Err.empty());
    EXPECT_EQ(std::string(Out.begin(), Out.end()), Expected);
  };
  for (uint32_t Mode :
       {0u, uint32_t(fixture::LoaderMode), uint32_t(fixture::TLSMode),
        uint32_t(fixture::PrivateTLSMode),
        uint32_t(fixture::WrappedEntryMode)}) {
    SCOPED_TRACE(Mode);
    auto Bytes = Mode ? fixture::pack(Original, Mode) : Original.File;
    ASSERT_TRUE(fixture::write(Path, Bytes));
    if (Mode) {
      Options.Transfer = Mode == fixture::WrappedEntryMode ? 1 : 0;
      auto Result = unpackFile(Path, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      ASSERT_EQ(Result->Outcome, UnpackOutcome::Unpacked)
          << Result->ProcessDiagnostic;
      ASSERT_TRUE(fixture::write(Path, Result->Image));
    }
    Native(0, std::string(fixture::AttachText) + fixture::QueryText +
                  fixture::DetachText);
  }
  auto Failed = Original.File;
  const auto *Record = Original.section(fixture::RecordSection);
  ASSERT_NE(Record, nullptr);
  llvm::support::endian::write32le(Failed.data() + Record->FileOffset + 12, 1);
  ASSERT_TRUE(fixture::write(Path, Failed));
  Native(1114, std::string(fixture::AttachText) + fixture::DetachText);
#endif
}

INSTANTIATE_TEST_SUITE_P(Backends, UnpackLibrary, testing::ValuesIn(Profiles),
                         [](const testing::TestParamInfo<Profile> &P) {
                           return P.param.Name;
                         });
} // namespace
} // namespace neverd::unpack
