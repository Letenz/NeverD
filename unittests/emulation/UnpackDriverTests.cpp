//===- UnpackDriverTests.cpp - Recovery through the driver environment ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../TestProcess.h"
#include "UnpackTestSupport.h"

#include "neverd/emulation/DriverSession.h"
#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessObserver.h"
#include "neverd/emulation/WindowsDriverState.h"
#include "neverd/object/PEChecksum.h"
#include "neverd/unpack/Unpack.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"

#ifdef NEVERD_UNPACK_DRIVER_CLI
#include "neverd/sdk/NeverDCAPI.h"
#endif
#ifdef _WIN32
#include <windows.h>
// ImageHlp declarations require the Windows types above.
#include <imagehlp.h>
#endif

namespace neverd::unpack {
namespace {
using namespace emulation;
using llvm::support::endian::read32le;
using llvm::support::endian::write32le;

struct Backend {
  const char *Name;
  ExecutionBackendKind Kind;
};
void PrintTo(const Backend &B, std::ostream *Out) { *Out << B.Name; }

class UnpackDriver : public testing::TestWithParam<Backend> {
protected:
  std::filesystem::path Scratch;
  test::Image Original;
  UnpackOptions Options;
  void SetUp() override {
    ExecutionConfiguration Config;
    Config.Backend = GetParam().Kind;
    Config.Architecture = GuestArchitecture::X64;
    Config.Contract = ExecutionContract::CheckedX64;
    auto Probe = probeExecutionBackend(Config);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available)
      GTEST_SKIP() << Probe->Reason;
    llvm::SmallString<128> Directory;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-unpack-driver",
                                                      Directory));
    Scratch = Directory.str().str();
    Original = test::readImage(NEVERD_UNPACK_DRIVER_FIXTURE);
    ASSERT_FALSE(HasFailure());
    Options.Process.Backend = GetParam().Kind;
  }
  void TearDown() override {
    if (!Scratch.empty()) {
      std::error_code Ignored;
      std::filesystem::remove_all(Scratch, Ignored);
    }
  }
  std::filesystem::path packed(unsigned Mode = 0) {
    auto Bytes = Original.File;
    const auto *Program = Original.section(".prog");
    const auto *Record = Original.section(".pack");
    const auto Entry = Original.Exports.find("packed_entry");
    if (!Program || !Record || Entry == Original.Exports.end() ||
        Original.Entry < Program->RVA ||
        Original.Entry - Program->RVA >= Program->VirtualSize ||
        std::count_if(
            Original.Sections.begin(), Original.Sections.end(),
            [](const auto &S) { return S.Name == ".prog"; }) != 1 ||
        Program->VirtualSize > 65536 ||
        Program->VirtualSize > Program->FileSize || Record->FileSize < 65552) {
      ADD_FAILURE() << "independent driver pack record is incomplete";
      return {};
    }
    uint8_t *Data = Bytes.data() + Record->FileOffset;
    write32le(Data, Mode);
    write32le(Data + 4, 0xa5);
    write32le(Data + 8, Program->VirtualSize);
    for (uint32_t I = 0; I < Program->VirtualSize; ++I)
      Data[16 + I] = uint8_t(Bytes[Program->FileOffset + I] ^ 0xa5);
    std::fill_n(Bytes.begin() + Program->FileOffset, Program->FileSize, 0);
    write32le(Bytes.data() + Original.EntryOffset, Entry->second);
    const auto Path = Scratch / "packed.sys";
    test::writeFile(Path, Bytes);
    return Path;
  }
  std::filesystem::path writeWatchInput() {
    auto Bytes = Original.File;
    const auto *Program = Original.section(".prog");
    const auto *Data = Original.section(".pack");
    if (!Program || !Data || Program->FileSize < 52 || Data->FileSize < 8) {
      ADD_FAILURE() << "driver write-watch control has no storage";
      return {};
    }
    std::fill_n(Bytes.begin() + Data->FileOffset, 8, 0);
    // Five independently encoded mov dword ptr [rip+disp32], imm32 stores.
    // Only the first cell is watched. The second witnesses whether the CPU
    // ran past each committed watched store before notifying the observer.
    const std::pair<uint32_t, uint32_t> Stores[] = {
        {4, 0}, {0, 1}, {4, 0x11}, {0, 2}, {4, 0x22}};
    for (size_t I = 0; I < std::size(Stores); ++I) {
      auto *Code = Bytes.data() + Program->FileOffset + I * 10;
      Code[0] = 0xc7;
      Code[1] = 0x05;
      write32le(Code + 2,
                Data->RVA + Stores[I].first - (Program->RVA + I * 10 + 10));
      write32le(Code + 6, Stores[I].second);
    }
    Bytes[Program->FileOffset + 50] = 0x90; // Watched before execution.
    Bytes[Program->FileOffset + 51] = 0xc3;
    write32le(Bytes.data() + Original.EntryOffset, Program->RVA);
    const auto Path = Scratch / "write-watch.sys";
    test::writeFile(Path, Bytes);
    return Path;
  }
  std::filesystem::path entryContextInput() {
    auto Bytes = Original.File;
    const auto *Program = Original.section(".prog");
    // Save a nonvolatile register, move the stack, retain the two arguments,
    // and alter an XMM register and the caller's shadow space. Restore the
    // required incoming state after the first stop; R8 deliberately stays 42.
    const uint8_t Code[] = {0x55,                         // push rbp
                            0x48, 0x83, 0xec, 0x20,       // sub rsp,32
                            0x48, 0x8d, 0x6c, 0x24, 0x20, // lea rbp,[rsp+32]
                            0x48, 0x89, 0x0c, 0x24,       // mov [rsp],rcx
                            0x48, 0x89, 0x54, 0x24, 0x08, // mov [rsp+8],rdx
                            0x49, 0xc7, 0xc0, 0x2a, 0,    0, 0, // mov r8,42
                            0x66, 0x48, 0x0f, 0x6e, 0xf1,       // movq xmm6,rcx
                            0x48, 0x89, 0x4c, 0x24, 0x30, // mov [rsp+48],rcx
                            0x48, 0xc7, 0x44, 0x24, 0x30, 0, 0,
                            0,    0,                // mov qword [rsp+48],0
                            0x66, 0x0f, 0xef, 0xf6, // pxor xmm6,xmm6
                            0x48, 0x83, 0xc4, 0x20, // add rsp,32
                            0x5d,                   // pop rbp
                            0x90, 0xc3};            // nop; ret
    if (!Program || Program->FileSize < sizeof(Code)) {
      ADD_FAILURE() << "driver context control has no code storage";
      return {};
    }
    std::copy(std::begin(Code), std::end(Code),
              Bytes.begin() + Program->FileOffset);
    write32le(Bytes.data() + Original.EntryOffset, Program->RVA);
    const auto Path = Scratch / "entry-context.sys";
    test::writeFile(Path, Bytes);
    return Path;
  }
  DriverOptions scenario() const {
    DriverOptions D;
    D.Backend = GetParam().Kind;
    D.Contract = ExecutionContract::CheckedX64;
    D.Unload = true;
    if (Options.Driver) {
      D.CPUID = Options.Driver->CPUID;
      D.LoadAddress = Options.Driver->LoadAddress;
    }
    D.Requests.push_back({DriverRequestKind::Create});
    DriverRequest IO;
    IO.ControlCode = 0x222000;
    IO.Input = {0x10, 0xe1, 0, 0xff};
    IO.OutputSize = IO.Input.size();
    D.Requests.push_back(IO);
    D.Requests.push_back({DriverRequestKind::Cleanup});
    D.Requests.push_back({DriverRequestKind::Close});
    return D;
  }
  void checkLifecycle(const std::filesystem::path &Path) {
    auto Run = emulateDriver(Path, scenario());
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    ASSERT_EQ(Run->Stop, DriverStopReason::Returned) << Run->Diagnostic;
    EXPECT_EQ(Run->NTStatus, 0u);
    ASSERT_EQ(Run->Requests.size(), 4u);
    for (const auto &Q : Run->Requests) {
      EXPECT_TRUE(Q.Completed);
      EXPECT_EQ(Q.IOStatus, 0u);
    }
    EXPECT_EQ(Run->Requests[1].Information, 4u);
    EXPECT_EQ(Run->Requests[1].Output,
              (std::vector<uint8_t>{0x4a, 0xbb, 0x5a, 0xa5}));
    EXPECT_TRUE(Run->UnloadCompleted);
    EXPECT_TRUE(Run->Devices.empty());
  }
};

TEST_P(UnpackDriver, RecoversDriverEntryImportsAndLifecycle) {
  ASSERT_NE(Original.FileCharacteristics & llvm::COFF::IMAGE_FILE_DLL, 0u);
  checkLifecycle(NEVERD_UNPACK_DRIVER_FIXTURE);
  ASSERT_FALSE(HasFatalFailure());
  const auto Input = packed();
  checkLifecycle(Input);
  ASSERT_FALSE(HasFatalFailure());
  auto Result = unpackFile(Input, Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Outcome, UnpackOutcome::UnsupportedState)
      << unpackResultJSON(*Result);
  EXPECT_TRUE(Result->Image.empty());
  EXPECT_NE(Result->Diagnostic.find("kernel image relocation state"),
            std::string::npos);
  // Native Windows rejects the fixed output even when this fresh model replay
  // passes. A snapshot must not certify the kernel loader boundary.
  Options.SnapshotOnly = true;
  Result = unpackFile(Input, Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Outcome, UnpackOutcome::Snapshot)
      << unpackResultJSON(*Result);
  EXPECT_NE(Result->Diagnostic.find("kernel image relocation state"),
            std::string::npos);
  EXPECT_EQ(Result->Profile, profile::ReportProfile);
  EXPECT_EQ(Result->EntryRVA, Original.Entry);
  EXPECT_EQ(Result->ProcessStop, "observer");
  ASSERT_FALSE(Result->Transfers.empty());
  EXPECT_TRUE(Result->Transfers.back().ProgramInvocation);
  EXPECT_TRUE(Result->RuntimeState.AdditionalStateInventoryKnown);
  EXPECT_FALSE(Result->RuntimeState.HasAdditionalDependencies);
  EXPECT_TRUE(Result->RuntimeState.AdditionalDependencyReasons.empty());
  const auto Rebuilt = test::readImage(Result->Image);
  EXPECT_NE(Rebuilt.FileCharacteristics & llvm::COFF::IMAGE_FILE_DLL, 0u);
  EXPECT_EQ(Rebuilt.Entry, Original.Entry);
  for (const auto &I : Rebuilt.Imports)
    EXPECT_EQ(I.Module, "ntoskrnl.exe");
  const auto Output = Scratch / "recovered.sys";
  test::writeFile(Output, Result->Image);
  checkLifecycle(Output);
}

TEST_P(UnpackDriver, RetainedKernelObjectsAndBorrowedPointersAreNotRecovery) {
  for (unsigned Mode : {1, 2, 3, 10, 12, 13, 14, 15, 16, 17, 18, 19, 20}) {
    SCOPED_TRACE(Mode);
    if (Mode == 19) {
      Options.Driver.emplace();
      Options.Driver->CPUID = {{0, std::nullopt, {1, 2, 3, 4}}};
    }
    const auto Input = packed(Mode);
    if (Mode >= 15 && Mode <= 19) {
      checkLifecycle(Input);
      ASSERT_FALSE(HasFatalFailure());
    }
    auto Result = unpackFile(Input, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Outcome, UnpackOutcome::UnsupportedState)
        << Result->Diagnostic;
    EXPECT_TRUE(Result->Image.empty());
    EXPECT_FALSE(Result->Diagnostic.empty());
    if (Mode == 3 || Mode == 10)
      EXPECT_GT(Result->RuntimeState.PossibleHeapReferences, 0u);
    else
      EXPECT_TRUE(Result->RuntimeState.HasAdditionalDependencies);
    const auto &Reasons = Result->RuntimeState.AdditionalDependencyReasons;
    auto HasReason = [&](llvm::StringRef Reason) {
      EXPECT_NE(std::find(Reasons.begin(), Reasons.end(), Reason),
                Reasons.end());
    };
    if (Mode == 20) {
      HasReason("live kernel pool allocations");
      HasReason("changed kernel loader objects");
    } else if (Mode == 1)
      HasReason("live kernel pool allocations");
    else if (Mode == 2)
      HasReason("changed kernel loader objects");
    else if (Mode == 12)
      HasReason("live kernel MDLs");
    else if (Mode == 13 || Mode == 14)
      HasReason("observed kernel MDL physical identity");
    else if (Mode >= 15)
      HasReason("unreconstructed kernel or environment effects");
    auto Snapshot = Options;
    Snapshot.SnapshotOnly = true;
    auto Bytes = unpackFile(Input, Snapshot);
    ASSERT_TRUE(bool(Bytes)) << llvm::toString(Bytes.takeError());
    EXPECT_EQ(Bytes->Outcome, UnpackOutcome::Snapshot) << Bytes->Diagnostic;
    EXPECT_FALSE(Bytes->Image.empty());
    EXPECT_FALSE(Bytes->Diagnostic.empty());
    EXPECT_EQ(Bytes->RuntimeState.AdditionalDependencyReasons, Reasons);
    auto Report = llvm::json::parse(unpackResultJSON(*Bytes));
    ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
    const auto *State = Report->getAsObject()->getObject("runtime_state");
    ASSERT_NE(State, nullptr);
    const auto *ReportedReasons =
        State->getArray("additional_dependency_reasons");
    ASSERT_NE(ReportedReasons, nullptr);
    ASSERT_EQ(ReportedReasons->size(), Reasons.size());
    for (size_t I = 0; I < Reasons.size(); ++I)
      EXPECT_EQ((*ReportedReasons)[I].getAsString(), Reasons[I]);
    if (GetParam().Kind == ExecutionBackendKind::Unicorn && Mode >= 12) {
      auto Strict = Options;
      Strict.Process.Contract = ExecutionContract::Legacy;
      auto Legacy = unpackFile(Input, Strict);
      ASSERT_TRUE(bool(Legacy)) << llvm::toString(Legacy.takeError());
      EXPECT_EQ(Legacy->Outcome, UnpackOutcome::UnsupportedState)
          << Legacy->Diagnostic;
      EXPECT_TRUE(Legacy->RuntimeState.HasAdditionalDependencies);
    }
  }
}

TEST_P(UnpackDriver, RecoversAfterReleasingTransientImageMDLs) {
  Options.SnapshotOnly = true;
  const auto Input = packed(11);
  checkLifecycle(Input);
  ASSERT_FALSE(HasFatalFailure());
  auto Result = unpackFile(Input, Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Outcome, UnpackOutcome::Snapshot)
      << unpackResultJSON(*Result);
  EXPECT_EQ(Result->EntryRVA, Original.Entry);
  EXPECT_FALSE(Result->RuntimeState.HasAdditionalDependencies);
  EXPECT_TRUE(Result->RuntimeState.AdditionalDependencyReasons.empty());
  const auto Output = Scratch / "released-mdl.sys";
  test::writeFile(Output, Result->Image);
  checkLifecycle(Output);
  if (GetParam().Kind == ExecutionBackendKind::Unicorn) {
    auto Strict = Options;
    Strict.Process.Contract = ExecutionContract::Legacy;
    auto Legacy = unpackFile(Input, Strict);
    ASSERT_TRUE(bool(Legacy)) << llvm::toString(Legacy.takeError());
    ASSERT_EQ(Legacy->Outcome, UnpackOutcome::Snapshot) << Legacy->Diagnostic;
    EXPECT_EQ(Legacy->EntryRVA, Result->EntryRVA);
    EXPECT_EQ(Legacy->Image, Result->Image);
  }
}

TEST_P(UnpackDriver,
       EntryArgumentsRegistersAndShadowSpaceRequireTheirOwnProof) {
  for (unsigned Mode : {5, 6, 7, 8, 9}) {
    SCOPED_TRACE(Mode);
    auto Result = unpackFile(packed(Mode), Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Outcome, UnpackOutcome::UnsupportedState)
        << Result->Diagnostic;
    EXPECT_TRUE(Result->RuntimeState.HasAdditionalDependencies);
    EXPECT_EQ(
        Result->RuntimeState.AdditionalDependencyReasons,
        (std::vector<std::string>{"changed DriverEntry invocation context"}));
    EXPECT_TRUE(Result->Image.empty());
  }
}

TEST_P(UnpackDriver, DynamicKernelExportIdentitySurvivesRebinding) {
  Options.SnapshotOnly = true;
  for (uint64_t Base :
       {Original.Base, uint64_t(0x190000000), uint64_t(0xfffff80140000000)}) {
    SCOPED_TRACE(Base);
    Options.Driver.emplace();
    Options.Driver->LoadAddress = Base;
    const auto Input = packed(4);
    checkLifecycle(Input);
    ASSERT_FALSE(HasFatalFailure());
    auto Result = unpackFile(Input, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Outcome, UnpackOutcome::Snapshot)
        << unpackResultJSON(*Result);
    EXPECT_EQ(Result->ImageBase, Base);
    EXPECT_EQ(Result->EntryRVA, Original.Entry);
    EXPECT_EQ(test::readImage(Result->Image).Base, Base);
    EXPECT_TRUE(std::any_of(
        Result->Imports.begin(), Result->Imports.end(), [](const auto &I) {
          return I.Module == "ntoskrnl.exe" && I.Name == "IofCompleteRequest" &&
                 I.Origin == ImportOrigin::Runtime;
        }));
    const auto Output = Scratch / "dynamic.sys";
    test::writeFile(Output, Result->Image);
    // Let the output select its own preferred base. It remains a fixed image;
    // a caller-provided rebase must not hide an incorrect reconstructed header.
    Options.Driver->LoadAddress = 0;
    checkLifecycle(Output);
    ASSERT_FALSE(HasFatalFailure());
  }
}

TEST_P(UnpackDriver, UserRuntimeInputsAndRestorationAreRejected) {
  const auto Input = packed();
  auto Restore = Options;
  Restore.RestoreRuntime = true;
  auto Result = unpackFile(Input, Restore);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Outcome, UnpackOutcome::UnsupportedState);
  EXPECT_TRUE(Result->Image.empty());
  auto UserInputs = Options;
  UserInputs.Process.Windows->PEBVersion = WindowsPEBVersion{10, 0, 19043, 2};
  auto Invalid = unpackFile(Input, UserInputs);
  ASSERT_FALSE(bool(Invalid));
  EXPECT_NE(llvm::toString(Invalid.takeError()).find("user-process inputs"),
            std::string::npos);
}

TEST_P(UnpackDriver, MalformedExportMetadataFailsBeforeExecution) {
  const auto Directory = Original.directory(llvm::COFF::EXPORT_TABLE);
  auto Offset = [&](uint32_t RVA) -> uint64_t {
    for (const auto &S : Original.Sections)
      if (RVA >= S.RVA && RVA - S.RVA < S.FileSize)
        return S.FileOffset + RVA - S.RVA;
    return UINT64_MAX;
  };
  const uint64_t Table = Offset(Directory.RelativeVirtualAddress);
  ASSERT_LT(Table, Original.File.size());
  const auto *Data = Original.File.data() + Table;
  const uint64_t Names = Offset(read32le(Data + 32));
  const uint64_t Ordinals = Offset(read32le(Data + 36));
  const uint64_t Addresses = Offset(read32le(Data + 28));
  ASSERT_LT(Names + 4, Original.File.size());
  ASSERT_LT(Ordinals + 2, Original.File.size());
  ASSERT_LT(Addresses + 4, Original.File.size());
  for (unsigned Case = 0; Case < 3; ++Case) {
    SCOPED_TRACE(Case);
    auto Bytes = Original.File;
    if (Case == 0)
      llvm::support::endian::write16le(Bytes.data() + Ordinals, 0xffff);
    else if (Case == 1)
      write32le(Bytes.data() + Addresses,
                0x800); // Unmapped header/section gap.
    else
      write32le(Bytes.data() + Names, 0xfffffff0);
    const auto Path = Scratch / "invalid-export.sys";
    test::writeFile(Path, Bytes);
    auto Run = emulateDriver(Path, scenario());
    ASSERT_FALSE(bool(Run));
    EXPECT_NE(llvm::toString(Run.takeError()).find("export"),
              std::string::npos);
  }
}

class DriverObserver final : public ProcessObserver {
public:
  std::string Fail;
  std::vector<bool> Entries;
  llvm::Expected<std::vector<ExecutionWatch>> started(ProcessView &P) override {
    if (Fail == "started")
      return llvm::createStringError("driver observer failure");
    auto M = P.inputModule();
    return std::vector<ExecutionWatch>{{M->Base, M->Size}};
  }
  llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
  invoking(ProcessView &P) override {
    auto M = P.inputModule();
    return std::vector<ExecutionWatch>{{M->Base, M->Size}};
  }
  llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
  watched(ProcessView &P, uint64_t) override {
    if (Fail == "watched")
      return llvm::createStringError("driver observer failure");
    Entries.push_back(P.programInvocation());
    return std::vector<ExecutionWatch>{};
  }
  llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
  resuming(ProcessView &) override {
    if (Fail == "resuming")
      return llvm::createStringError("driver observer failure");
    return std::nullopt;
  }
  llvm::Error exporting(ProcessView &, const ProcessExportView &,
                        std::optional<uint64_t>) override {
    if (Fail == "exporting")
      return llvm::createStringError("driver observer failure");
    return llvm::Error::success();
  }
};

class DriverPoolObserver final : public ProcessObserver {
public:
  explicit DriverPoolObserver(uint64_t EntryRVA) : EntryRVA(EntryRVA) {}
  std::shared_ptr<const ProcessRuntimeState> Initial, Metadata, Contents;
  llvm::Expected<std::vector<ExecutionWatch>> started(ProcessView &P) override {
    auto State = P.runtimeState(true);
    if (!State)
      return State.takeError();
    Initial = std::move(*State);
    auto M = P.inputModule();
    return std::vector<ExecutionWatch>{{M->Base + EntryRVA, 1}};
  }
  llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
  watched(ProcessView &P, uint64_t) override {
    auto State = P.runtimeState(false);
    if (!State)
      return State.takeError();
    Metadata = std::move(*State);
    State = P.runtimeState(true);
    if (!State)
      return State.takeError();
    Contents = std::move(*State);
    return std::nullopt;
  }

private:
  uint64_t EntryRVA;
};

class DriverWriteObserver final : public ProcessObserver {
public:
  DriverWriteObserver(uint64_t DataRVA, uint64_t StopRVA, bool Fail)
      : DataRVA(DataRVA), StopRVA(StopRVA), Fail(Fail) {}
  std::vector<std::pair<uint32_t, uint32_t>> Changes;
  std::pair<uint32_t, uint32_t> Final{};
  llvm::Expected<std::vector<ExecutionWatch>> started(ProcessView &P) override {
    Base = P.inputModule()->Base;
    return std::vector<ExecutionWatch>{{Base + StopRVA, 1}};
  }
  std::vector<MemoryWriteWatch> writeWatches() const override {
    return {{Base + DataRVA, 4}};
  }
  llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
  resuming(ProcessView &P) override {
    auto Value = read(P);
    if (!Value)
      return Value.takeError();
    if (Value->first != Last) {
      Last = Value->first;
      Changes.push_back(*Value);
      if (Fail)
        return llvm::createStringError(
            "driver committed-write observer failure");
    }
    return std::nullopt;
  }
  llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
  watched(ProcessView &P, uint64_t) override {
    auto Value = read(P);
    if (!Value)
      return Value.takeError();
    Final = *Value;
    return std::nullopt;
  }

private:
  llvm::Expected<std::pair<uint32_t, uint32_t>> read(ProcessView &P) {
    std::array<uint8_t, 8> Bytes;
    if (auto E = P.read(Base + DataRVA, Bytes))
      return std::move(E);
    return std::pair(read32le(Bytes.data()), read32le(Bytes.data() + 4));
  }
  uint64_t DataRVA, StopRVA, Base = 0;
  uint32_t Last = 0;
  bool Fail;
};

class DriverContextObserver final : public ProcessObserver {
public:
  explicit DriverContextObserver(std::vector<uint64_t> Stops)
      : Stops(std::move(Stops)) {}
  std::shared_ptr<const ProcessRuntimeState> Initial;
  std::vector<std::shared_ptr<const ProcessRuntimeState>> Metadata, Contents;
  llvm::Expected<std::vector<ExecutionWatch>> started(ProcessView &P) override {
    auto State = P.runtimeState(true);
    if (!State)
      return State.takeError();
    Initial = std::move(*State);
    Base = P.inputModule()->Base;
    return next();
  }
  llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
  watched(ProcessView &P, uint64_t) override {
    auto State = P.runtimeState(false);
    if (!State)
      return State.takeError();
    Metadata.push_back(std::move(*State));
    State = P.runtimeState(true);
    if (!State)
      return State.takeError();
    Contents.push_back(std::move(*State));
    ++Index;
    if (Index == Stops.size())
      return std::nullopt;
    return next();
  }

private:
  std::vector<ExecutionWatch> next() const {
    return {{Base + Stops.at(Index), 1}};
  }
  std::vector<uint64_t> Stops;
  uint64_t Base = 0;
  size_t Index = 0;
};

TEST_P(UnpackDriver, StoppedViewOwnsEntryRegistersFramesAndStack) {
  const auto Input = entryContextInput();
  ASSERT_FALSE(HasFailure());
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
    SCOPED_TRACE(int(Contract));
    auto D = scenario();
    D.Contract = Contract;
    DriverContextObserver Observer({Original.section(".prog")->RVA + 36});
    auto Run = observeDriver(Input, D, Observer);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    ASSERT_EQ(Run->Stop, DriverStopReason::Observer) << Run->Diagnostic;
    EXPECT_EQ(Run->Instructions, 8u);
    ASSERT_TRUE(Observer.Initial);
    const auto &Initial =
        static_cast<const WindowsDriverState &>(*Observer.Initial);
    ASSERT_TRUE(Initial.Entry);
    const auto &Start = *Initial.Entry;
    EXPECT_EQ(Start.Registers, Start.InitialRegisters);
    EXPECT_EQ(Start.Frame, Start.InitialFrame);
    EXPECT_TRUE(Start.DriverEntryActive);
    EXPECT_FALSE(Initial.hasAdditionalDependencies());
    const uint64_t SP = Start.Registers.at(CPURegister::X64SP)[0];
    const uint64_t Driver = Start.Registers.at(CPURegister::X64CX)[0];
    const uint64_t Path = Start.Registers.at(CPURegister::X64DX)[0];
    EXPECT_EQ(Start.Registers.size(), 36u);
    ASSERT_NE(Driver, 0u);
    ASSERT_NE(Path, 0u);
    ASSERT_EQ(Observer.Metadata.size(), 1u);
    ASSERT_EQ(Observer.Contents.size(), 1u);
    for (const auto &State :
         {Observer.Metadata.front(), Observer.Contents.front()}) {
      const auto &Captured = static_cast<const WindowsDriverState &>(*State);
      ASSERT_TRUE(Captured.Entry);
      const auto &Entry = *Captured.Entry;
      EXPECT_EQ(Entry.InitialRegisters, Start.InitialRegisters);
      EXPECT_EQ(Entry.InitialFrame, Start.InitialFrame);
      EXPECT_EQ(Entry.Registers.size(), 36u);
      EXPECT_EQ(Entry.Registers.at(CPURegister::X64SP)[0], SP - 40);
      EXPECT_EQ(Entry.Registers.at(CPURegister::X64BP)[0], SP - 8);
      EXPECT_EQ(Entry.Registers.at(CPURegister::X64R8)[0], 42u);
      EXPECT_EQ(Entry.Registers.at(CPURegister::X64V6),
                (RegisterValue{Driver, 0}));
      auto Frame = Start.Frame;
      llvm::support::endian::write64le(Frame.data() + 8, Driver);
      EXPECT_EQ(Entry.Frame, Frame);
      EXPECT_TRUE(Entry.DriverEntryActive);
      EXPECT_EQ(Captured.Reasons,
                (std::vector<std::string>{"changed DriverEntry invocation "
                                          "context"}));
      if (State == Observer.Metadata.front()) {
        EXPECT_FALSE(Entry.StackBytes);
        continue;
      }
      ASSERT_TRUE(Entry.StackBytes);
      ASSERT_EQ(Entry.StackBytes->size(), Entry.StackSize);
      ASSERT_GE(SP - 40, Entry.StackBase);
      const uint64_t Offset = SP - 40 - Entry.StackBase;
      ASSERT_LE(Offset + 80, Entry.StackSize);
      const auto *Bytes = Entry.StackBytes->data() + Offset;
      EXPECT_EQ(llvm::support::endian::read64le(Bytes), Driver);
      EXPECT_EQ(llvm::support::endian::read64le(Bytes + 8), Path);
      EXPECT_EQ(llvm::support::endian::read64le(Bytes + 32),
                Start.Registers.at(CPURegister::X64BP)[0]);
      EXPECT_TRUE(
          std::equal(Entry.Frame.begin(), Entry.Frame.end(), Bytes + 40));
      ASSERT_TRUE(Start.StackBytes);
      EXPECT_NE(*Entry.StackBytes, *Start.StackBytes);
    }
  }
}

TEST_P(UnpackDriver, LaterRestorationDoesNotChangeEarlierContextCaptures) {
  const auto Input = entryContextInput();
  ASSERT_FALSE(HasFailure());
  DriverContextObserver Observer({Original.section(".prog")->RVA + 36,
                                  Original.section(".prog")->RVA + 54});
  auto Run = observeDriver(Input, scenario(), Observer);
  ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
  ASSERT_EQ(Run->Stop, DriverStopReason::Observer) << Run->Diagnostic;
  EXPECT_EQ(Run->Instructions, 12u);
  ASSERT_EQ(Observer.Contents.size(), 2u);
  const auto &Before =
      static_cast<const WindowsDriverState &>(*Observer.Contents[0]);
  const auto &After =
      static_cast<const WindowsDriverState &>(*Observer.Contents[1]);
  ASSERT_TRUE(Before.Entry);
  ASSERT_TRUE(After.Entry);
  EXPECT_TRUE(Before.hasAdditionalDependencies());
  EXPECT_FALSE(After.hasAdditionalDependencies());
  const auto &Entry = *After.Entry;
  EXPECT_EQ(Entry.InitialRegisters, Before.Entry->InitialRegisters);
  EXPECT_EQ(Entry.Registers.at(CPURegister::X64SP),
            Entry.InitialRegisters.at(CPURegister::X64SP));
  EXPECT_EQ(Entry.Registers.at(CPURegister::X64BP),
            Entry.InitialRegisters.at(CPURegister::X64BP));
  EXPECT_EQ(Entry.Registers.at(CPURegister::X64V6),
            Entry.InitialRegisters.at(CPURegister::X64V6));
  EXPECT_EQ(Entry.Frame, Entry.InitialFrame);
  EXPECT_NE(Before.Entry->Frame, Entry.Frame);
  EXPECT_EQ(Entry.Registers.at(CPURegister::X64R8)[0], 42u);
  ASSERT_TRUE(Entry.StackBytes);
  ASSERT_TRUE(Before.Entry->StackBytes);
  EXPECT_NE(*Entry.StackBytes, *Before.Entry->StackBytes);
}

TEST_P(UnpackDriver, LaterInvocationsRetainTheOriginalEntryBaseline) {
  class InvocationObserver final : public ProcessObserver {
  public:
    std::vector<std::shared_ptr<const ProcessRuntimeState>> States;
    llvm::Expected<std::vector<ExecutionWatch>>
    started(ProcessView &P) override {
      auto State = P.runtimeState(false);
      if (!State)
        return State.takeError();
      States.push_back(std::move(*State));
      return std::vector<ExecutionWatch>{};
    }
    llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
    invoking(ProcessView &P) override {
      auto State = started(P);
      if (!State)
        return State.takeError();
      return std::nullopt;
    }
    llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
    watched(ProcessView &, uint64_t) override {
      return llvm::createStringError("unexpected driver invocation watch");
    }
  } Observer;
  auto Run = observeDriver(NEVERD_UNPACK_DRIVER_FIXTURE, scenario(), Observer);
  ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
  ASSERT_EQ(Run->Stop, DriverStopReason::Returned) << Run->Diagnostic;
  EXPECT_TRUE(Run->UnloadCompleted);
  ASSERT_EQ(Observer.States.size(), 6u);
  const auto &First =
      static_cast<const WindowsDriverState &>(*Observer.States.front());
  ASSERT_TRUE(First.Entry);
  for (size_t I = 0; I < Observer.States.size(); ++I) {
    const auto &State =
        static_cast<const WindowsDriverState &>(*Observer.States[I]);
    ASSERT_TRUE(State.Entry);
    EXPECT_EQ(State.Entry->InitialRegisters, First.Entry->InitialRegisters);
    EXPECT_EQ(State.Entry->InitialFrame, First.Entry->InitialFrame);
    EXPECT_EQ(State.Entry->DriverEntryActive, I == 0);
    EXPECT_FALSE(State.Entry->StackBytes);
    if (I)
      EXPECT_NE(std::find(State.Reasons.begin(), State.Reasons.end(),
                          "changed DriverEntry invocation context"),
                State.Reasons.end());
  }
}

TEST_P(UnpackDriver, CommittedWriteWatchesSuspendBeforeTheNextInstruction) {
  const auto Input = writeWatchInput();
  ASSERT_FALSE(HasFailure());
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
    SCOPED_TRACE(int(Contract));
    auto D = scenario();
    D.Contract = Contract;
    D.TraceMemoryWrites = false;
    D.EventLimit = 1;
    DriverWriteObserver Observer(Original.section(".pack")->RVA,
                                 Original.section(".prog")->RVA + 50, false);
    auto Run = observeDriver(Input, D, Observer);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    ASSERT_EQ(Run->Stop, DriverStopReason::Observer) << Run->Diagnostic;
    EXPECT_EQ(Observer.Changes,
              (std::vector<std::pair<uint32_t, uint32_t>>{{1, 0}, {2, 0x11}}));
    EXPECT_EQ(Observer.Final, (std::pair<uint32_t, uint32_t>{2, 0x22}));
    EXPECT_EQ(Run->Instructions, 5u);
    EXPECT_TRUE(Run->Writes.empty());
  }
}

TEST_P(UnpackDriver, CommittedWriteObserverFailuresRemainAPIErrors) {
  const auto Input = writeWatchInput();
  ASSERT_FALSE(HasFailure());
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
    SCOPED_TRACE(int(Contract));
    auto D = scenario();
    D.Contract = Contract;
    D.TraceMemoryWrites = false;
    DriverWriteObserver Observer(Original.section(".pack")->RVA,
                                 Original.section(".prog")->RVA + 50, true);
    auto Run = observeDriver(Input, D, Observer);
    ASSERT_FALSE(bool(Run));
    EXPECT_NE(llvm::toString(Run.takeError())
                  .find("committed-write observer failure"),
              std::string::npos);
    EXPECT_EQ(Observer.Changes,
              (std::vector<std::pair<uint32_t, uint32_t>>{{1, 0}}));
  }
}

TEST_P(UnpackDriver, StoppedViewOwnsLivePoolContentsAndContracts) {
  DriverPoolObserver Observer(Original.Entry);
  auto Run = observeDriver(packed(1), scenario(), Observer);
  ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
  ASSERT_EQ(Run->Stop, DriverStopReason::Observer) << Run->Diagnostic;
  // The driver session and its memory have already been destroyed here.
  ASSERT_TRUE(Observer.Initial);
  ASSERT_EQ(Observer.Initial->Profile,
            ProcessRuntimeState::Kind::WindowsDriverX64);
  const auto &Initial =
      static_cast<const WindowsDriverState &>(*Observer.Initial);
  EXPECT_TRUE(Initial.Pools.empty());
  EXPECT_EQ(Initial.additionalDependencyReasons(), std::vector<std::string>{});
  EXPECT_FALSE(Initial.hasAdditionalDependencies());
  for (const auto &State : {Observer.Metadata, Observer.Contents}) {
    ASSERT_TRUE(State);
    ASSERT_EQ(State->Profile, ProcessRuntimeState::Kind::WindowsDriverX64);
    const auto &Driver = static_cast<const WindowsDriverState &>(*State);
    ASSERT_EQ(Driver.Pools.size(), 1u);
    const auto &Pool = Driver.Pools.front();
    EXPECT_NE(Pool.Address, 0u);
    EXPECT_EQ(Pool.RequestedSize, 32u);
    EXPECT_EQ(Pool.API, WindowsDriverState::PoolAPI::ExAllocatePoolWithTag);
    EXPECT_EQ(Pool.TypeOrFlags, 0u);
    EXPECT_EQ(Pool.Tag, 0x44564e55u);
    EXPECT_TRUE(Driver.hasAdditionalDependencies());
    EXPECT_EQ(Driver.additionalDependencyReasons(),
              (std::vector<std::string>{"live kernel pool allocations",
                                        "retained kernel object allocations"}));
    if (State == Observer.Metadata)
      EXPECT_FALSE(Pool.Bytes);
    else {
      ASSERT_TRUE(Pool.Bytes);
      EXPECT_EQ(*Pool.Bytes, std::vector<uint8_t>(32, 0xcd));
    }
  }
}

TEST_P(UnpackDriver, ObservationPreservesLifecycleAndCallbackProvenance) {
  DriverObserver Observer;
  auto Run = observeDriver(NEVERD_UNPACK_DRIVER_FIXTURE, scenario(), Observer);
  ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
  ASSERT_EQ(Run->Stop, DriverStopReason::Returned) << Run->Diagnostic;
  EXPECT_TRUE(Run->UnloadCompleted);
  ASSERT_EQ(Observer.Entries.size(), 6u);
  EXPECT_TRUE(Observer.Entries.front());
  EXPECT_TRUE(std::none_of(Observer.Entries.begin() + 1, Observer.Entries.end(),
                           [](bool E) { return E; }));
}

TEST_P(UnpackDriver, SchedulingSlicesPreserveInvocationAndTransferIdentity) {
  Options.SnapshotOnly = true;
  auto D = scenario();
  D.Scheduling = DriverScheduling{1, 1};
  DriverObserver Observer;
  auto Run = observeDriver(NEVERD_UNPACK_DRIVER_FIXTURE, D, Observer);
  ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
  ASSERT_EQ(Run->Stop, DriverStopReason::Returned) << Run->Diagnostic;
  ASSERT_EQ(Observer.Entries.size(), 6u);
  EXPECT_TRUE(Observer.Entries.front());
  EXPECT_TRUE(std::none_of(Observer.Entries.begin() + 1, Observer.Entries.end(),
                           [](bool E) { return E; }));
  const auto Input = packed();
  auto Unscheduled = unpackFile(Input, Options);
  ASSERT_TRUE(bool(Unscheduled)) << llvm::toString(Unscheduled.takeError());
  auto Scheduled = Options;
  Scheduled.Driver.emplace();
  Scheduled.Driver->Scheduling = D.Scheduling;
  auto Result = unpackFile(Input, Scheduled);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Outcome, UnpackOutcome::Snapshot)
      << unpackResultJSON(*Result);
  EXPECT_EQ(Result->EntryRVA, Unscheduled->EntryRVA);
  EXPECT_EQ(Result->Transfers.size(), Unscheduled->Transfers.size());
  EXPECT_EQ(Result->Image, Unscheduled->Image);
  // Raw write logs are not an input to recovery. A small API-event budget must
  // still observe the many decoder writes that produce the recovered entry.
  auto FewEvents = Options;
  FewEvents.Process.Limits.Events = 1;
  auto WithoutWriteLog = unpackFile(Input, FewEvents);
  ASSERT_TRUE(bool(WithoutWriteLog))
      << llvm::toString(WithoutWriteLog.takeError());
  ASSERT_EQ(WithoutWriteLog->Outcome, UnpackOutcome::Snapshot)
      << WithoutWriteLog->Diagnostic;
  EXPECT_EQ(WithoutWriteLog->EntryRVA, Unscheduled->EntryRVA);
  EXPECT_EQ(WithoutWriteLog->Image, Unscheduled->Image);
  FewEvents.Process.Contract = ExecutionContract::Legacy;
  auto Legacy = unpackFile(Input, FewEvents);
  ASSERT_TRUE(bool(Legacy)) << llvm::toString(Legacy.takeError());
  ASSERT_EQ(Legacy->Outcome, UnpackOutcome::Snapshot) << Legacy->Diagnostic;
  EXPECT_EQ(Legacy->EntryRVA, Unscheduled->EntryRVA);
  EXPECT_EQ(Legacy->Image, Unscheduled->Image);
}

TEST_P(UnpackDriver, ObserverFailuresAreAPIErrors) {
  for (const char *Where : {"started", "watched", "resuming", "exporting"}) {
    SCOPED_TRACE(Where);
    DriverObserver Observer;
    Observer.Fail = Where;
    auto Run =
        observeDriver(NEVERD_UNPACK_DRIVER_FIXTURE, scenario(), Observer);
    ASSERT_FALSE(bool(Run));
    EXPECT_NE(llvm::toString(Run.takeError()).find("driver observer failure"),
              std::string::npos);
  }
}

TEST_P(UnpackDriver, CAPIAndCLIUseTheDriverEnvironment) {
#ifndef NEVERD_UNPACK_DRIVER_CLI
  GTEST_SKIP() << "shared SDK and CLI are not built";
#else
  const auto Input = packed().string();
  const auto APIPath = (Scratch / "api.sys").string();
  const auto CLIPath = (Scratch / "cli.sys").string();
  auto Session = neverd_session_create();
  ASSERT_NE(Session, nullptr);
  auto Release = llvm::scope_exit([&] { neverd_session_destroy(Session); });
  std::string JSON = "{\"backend\":\"";
  JSON += executionBackendName(GetParam().Kind);
  JSON += "\",\"driver\":{\"service_name\":\"UnpackOracle\"}}";
  const std::string DefaultJSON = JSON;
  const std::vector<uint8_t> Existing{0x55, 0x7f, 0x23};
  test::writeFile(APIPath, Existing);
  const char *DefaultRaw = neverd_unpack_json(
      Session, Input.c_str(), APIPath.c_str(), DefaultJSON.c_str());
  ASSERT_NE(DefaultRaw, nullptr) << neverd_last_error(Session);
  auto Default = llvm::json::parse(DefaultRaw);
  neverd_free_string(DefaultRaw);
  ASSERT_TRUE(bool(Default)) << llvm::toString(Default.takeError());
  EXPECT_EQ(Default->getAsObject()->getString("outcome"), "unsupported_state");
  EXPECT_EQ(test::readFile(APIPath), Existing);
  JSON.pop_back();
  JSON += ",\"snapshot_only\":true}";
  const char *Raw =
      neverd_unpack_json(Session, Input.c_str(), APIPath.c_str(), JSON.c_str());
  ASSERT_NE(Raw, nullptr) << neverd_last_error(Session);
  auto Free = llvm::scope_exit([&] { neverd_free_string(Raw); });
  auto API = llvm::json::parse(Raw);
  ASSERT_TRUE(bool(API)) << llvm::toString(API.takeError());
  ASSERT_EQ(API->getAsObject()->getString("outcome"), "snapshot");
  ASSERT_NE(API->getAsObject()
                ->getString("diagnostic")
                ->find("kernel image relocation state"),
            llvm::StringRef::npos);
  const auto Report = (Scratch / "cli.json").string();
  const auto Command = neverd::test::shellQuote(NEVERD_UNPACK_DRIVER_CLI) +
                       " unpack " + neverd::test::shellQuote(Input) + " -o " +
                       neverd::test::shellQuote(CLIPath) +
                       " --options=" + neverd::test::shellQuote(JSON) +
                       neverd::test::redirectStdout(Report) +
                       neverd::test::silenceStderr();
  ASSERT_EQ(
      neverd::test::systemExitCode(neverd::test::runShellCommand(Command)), 0);
  EXPECT_EQ(test::readFile(APIPath), test::readFile(CLIPath));
  auto Bytes = test::readFile(Report);
  auto CLI = llvm::json::parse(llvm::StringRef(
      reinterpret_cast<const char *>(Bytes.data()), Bytes.size()));
  ASSERT_TRUE(bool(CLI)) << llvm::toString(CLI.takeError());
  API->getAsObject()->getObject("output")->erase("path");
  CLI->getAsObject()->getObject("output")->erase("path");
  EXPECT_EQ(*API, *CLI);
#endif
}

TEST(DriverChecksum, AgreesWithTheIndependentLinker) {
  const auto Bytes = test::readFile(NEVERD_UNPACK_DRIVER_FIXTURE);
  ASSERT_GT(Bytes.size(), 0x40u);
  const uint64_t Offset = uint64_t(read32le(Bytes.data() + 0x3c)) + 24 +
                          offsetof(llvm::object::pe32plus_header, CheckSum);
  ASSERT_LE(Offset + 4, Bytes.size());
  const uint32_t Linked = read32le(Bytes.data() + Offset);
  ASSERT_NE(Linked, 0u);
  EXPECT_EQ(computePEChecksum(Bytes, Offset), Linked);
  EXPECT_FALSE(computePEChecksum(Bytes, Bytes.size()));
}

TEST(DriverChecksum, NativeImageHlpValidatesOddAndEvenFileExtents) {
#ifdef _WIN32
  HMODULE Module = LoadLibraryW(L"imagehlp.dll");
  ASSERT_NE(Module, nullptr);
  auto Release = llvm::scope_exit([&] { FreeLibrary(Module); });
  auto Check = reinterpret_cast<decltype(&CheckSumMappedFile)>(
      GetProcAddress(Module, "CheckSumMappedFile"));
  ASSERT_NE(Check, nullptr);
  auto Bytes = test::readFile(NEVERD_UNPACK_DRIVER_FIXTURE);
  const uint64_t Offset = uint64_t(read32le(Bytes.data() + 0x3c)) + 24 +
                          offsetof(llvm::object::pe32plus_header, CheckSum);
  for (unsigned I = 0; I < 5; ++I) {
    DWORD Header = 0, Expected = 0;
    ASSERT_NE(Check(Bytes.data(), DWORD(Bytes.size()), &Header, &Expected),
              nullptr);
    EXPECT_EQ(computePEChecksum(Bytes, Offset), Expected);
    Bytes.push_back(uint8_t(0xef - I));
  }
#else
  GTEST_SKIP() << "native ImageHlp requires Windows";
#endif
}

INSTANTIATE_TEST_SUITE_P(
    Backends, UnpackDriver,
    testing::Values(Backend{"Unicorn", ExecutionBackendKind::Unicorn},
                    Backend{"Kvm", ExecutionBackendKind::KVM},
                    Backend{"Whp", ExecutionBackendKind::WHP}),
    [](const auto &I) { return I.param.Name; });
} // namespace
} // namespace neverd::unpack
