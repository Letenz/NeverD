//===- UnpackDriverTests.cpp - Recovery through the driver environment ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../lib/unpack/format/pe/PEDriverRelocations.h"
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
#include "llvm/Support/SHA256.h"

#ifdef NEVERD_UNPACK_DRIVER_CLI
#include "neverd/sdk/NeverDCAPI.h"
#endif
#ifdef _WIN32
#include <windows.h>
// ImageHlp declarations require the Windows types above.
#include <imagehlp.h>
// SDK macros otherwise expand LLVM's qualified PE constants below.
#undef IMAGE_FILE_DLL
#undef IMAGE_FILE_RELOCS_STRIPPED
#undef IMAGE_REL_BASED_DIR64
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
  std::filesystem::path writeWatchInput(bool SameValue = false) {
    auto Bytes = Original.File;
    const auto *Program = Original.section(".prog");
    const auto *Data = Original.section(".pack");
    if (!Program || !Data || Program->FileSize < 52 || Data->FileSize < 8) {
      ADD_FAILURE() << "driver write-watch control has no storage";
      return {};
    }
    std::fill_n(Bytes.begin() + Data->FileOffset, 8, 0);
    // Independently encoded mov dword ptr [rip+disp32], imm32 stores.
    // Only the first cell is watched. The second witnesses whether the CPU
    // ran past each committed watched store before notifying the observer.
    const std::vector<std::pair<uint32_t, uint32_t>> Stores =
        SameValue
            ? std::vector<std::pair<uint32_t, uint32_t>>{{0, 0}, {0, 0}, {0, 0}}
            : std::vector<std::pair<uint32_t, uint32_t>>{
                  {4, 0}, {0, 1}, {4, 0x11}, {0, 2}, {4, 0x22}};
    for (size_t I = 0; I < std::size(Stores); ++I) {
      auto *Code = Bytes.data() + Program->FileOffset + I * 10;
      Code[0] = 0xc7;
      Code[1] = 0x05;
      write32le(Code + 2,
                Data->RVA + Stores[I].first - (Program->RVA + I * 10 + 10));
      write32le(Code + 6, Stores[I].second);
    }
    Bytes[Program->FileOffset + 10 * Stores.size()] = 0x90;
    Bytes[Program->FileOffset + 10 * Stores.size() + 1] = 0xc3;
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
    std::vector<uint8_t> Code;
    auto Emit = [&](std::initializer_list<uint8_t> Bytes) {
      Code.insert(Code.end(), Bytes.begin(), Bytes.end());
    };
    Emit({0x55});                               // push rbp
    Emit({0x48, 0x83, 0xec, 0x20});             // sub rsp,32
    Emit({0x48, 0x8d, 0x6c, 0x24, 0x20});       // lea rbp,[rsp+32]
    Emit({0x48, 0x89, 0x0c, 0x24});             // mov [rsp],rcx
    Emit({0x48, 0x89, 0x54, 0x24, 0x08});       // mov [rsp+8],rdx
    Emit({0xf3, 0x0f, 0x7f, 0x74, 0x24, 0x10}); // movdqu [rsp+16],xmm6
    Emit({0x49, 0xc7, 0xc0, 0x2a, 0, 0, 0});    // mov r8,42
    Emit({0x66, 0x48, 0x0f, 0x6e, 0xf1});       // movq xmm6,rcx
    Emit({0x48, 0x8b, 0x44, 0x24, 0x30});       // mov rax,[rsp+48]
    Emit({0x48, 0x89, 0x4c, 0x24, 0x30});       // mov [rsp+48],rcx
    Emit({0x48, 0x89, 0x44, 0x24, 0x30});       // mov [rsp+48],rax
    Emit({0xf3, 0x0f, 0x6f, 0x74, 0x24, 0x10}); // movdqu xmm6,[rsp+16]
    Emit({0x48, 0x83, 0xc4, 0x20});             // add rsp,32
    Emit({0x5d});                               // pop rbp
    Emit({0x90, 0xc3});                         // nop; ret
    if (!Program || Program->FileSize < Code.size()) {
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
  void expectSameOpaqueSnapshot(const UnpackResult &Opaque,
                                const UnpackResult &Checked) {
    // Opaque software execution cannot retain loader-field provenance. Compare
    // every input section and the existing import metadata byte for byte while
    // checking the independently decoded relocation difference explicitly.
    const auto A = test::readImage(Opaque.Image);
    const auto B = test::readImage(Checked.Image);
    EXPECT_EQ(A.Base, B.Base);
    EXPECT_EQ(A.Entry, B.Entry);
    EXPECT_EQ(A.DLLCharacteristics, B.DLLCharacteristics);
    EXPECT_EQ(A.FileCharacteristics & ~llvm::COFF::IMAGE_FILE_RELOCS_STRIPPED,
              B.FileCharacteristics & ~llvm::COFF::IMAGE_FILE_RELOCS_STRIPPED);
    EXPECT_EQ(A.Imports, B.Imports);
    EXPECT_EQ(A.Exports, B.Exports);
    for (const auto &S : Original.Sections) {
      SCOPED_TRACE(S.Name);
      const uint64_t Size = std::max(S.VirtualSize, S.FileSize);
      ASSERT_LE(uint64_t(S.RVA) + Size, A.Mapped.size());
      ASSERT_LE(uint64_t(S.RVA) + Size, B.Mapped.size());
      EXPECT_EQ(llvm::ArrayRef(A.Mapped).slice(S.RVA, Size),
                llvm::ArrayRef(B.Mapped).slice(S.RVA, Size));
    }
    const auto AD = A.directory(llvm::COFF::BASE_RELOCATION_TABLE);
    const auto BD = B.directory(llvm::COFF::BASE_RELOCATION_TABLE);
    EXPECT_EQ(AD.Size, 0u);
    EXPECT_GT(BD.Size, 0u);
    const auto *Metadata = A.section(".neverd");
    ASSERT_NE(Metadata, nullptr);
    ASSERT_NE(B.section(".neverd"), nullptr);
    EXPECT_EQ(Metadata->RVA, B.section(".neverd")->RVA);
    EXPECT_EQ(
        llvm::ArrayRef(A.Mapped).slice(Metadata->RVA, Metadata->VirtualSize),
        llvm::ArrayRef(B.Mapped).slice(Metadata->RVA, Metadata->VirtualSize));
    EXPECT_GE(BD.RelativeVirtualAddress, Metadata->RVA + Metadata->VirtualSize);
    EXPECT_EQ(Opaque.Diagnostic, Checked.Diagnostic);
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
  std::filesystem::path relocationHistoryInput(bool DebugRecords = false) {
    auto Bytes = Original.File;
    const auto *Program = Original.section(".prog");
    const auto *Data = Original.section(".pack");
    if (!Program || !Data || Program->FileSize < 50 || Data->FileSize < 80) {
      ADD_FAILURE() << "driver relocation control has no storage";
      return {};
    }
    const llvm::MemoryBufferRef Buffer(
        llvm::StringRef(reinterpret_cast<const char *>(Bytes.data()),
                        Bytes.size()),
        "relocation-history");
    auto Object = llvm::object::COFFObjectFile::create(Buffer);
    if (!Object) {
      ADD_FAILURE() << llvm::toString(Object.takeError());
      return {};
    }
    const auto *Directory =
        (*Object)->getDataDirectory(llvm::COFF::BASE_RELOCATION_TABLE);
    if (!Directory) {
      ADD_FAILURE() << "driver relocation control has no directory slot";
      return {};
    }
    const auto DirectoryOffset =
        reinterpret_cast<const uint8_t *>(Directory) - Bytes.data();
    if (DebugRecords) {
      const auto *Debug =
          (*Object)->getDataDirectory(llvm::COFF::DEBUG_DIRECTORY);
      if (!Debug) {
        ADD_FAILURE()
            << "driver relocation control has no debug directory slot";
        return {};
      }
      const auto Offset =
          reinterpret_cast<const uint8_t *>(Debug) - Bytes.data();
      write32le(Bytes.data() + Offset, Data->RVA);
      write32le(Bytes.data() + Offset + 4,
                sizeof(llvm::object::debug_directory));
    }
    write32le(Bytes.data() + DirectoryOffset, Data->RVA + 64);
    write32le(Bytes.data() + DirectoryOffset + 4, 16);
    uint8_t *Record = Bytes.data() + Data->FileOffset;
    const uint64_t Values[] = {Original.Base + Program->RVA + 16,
                               Original.Base + Program->RVA + 24,
                               UINT64_MAX - 16};
    for (size_t I = 0; I < std::size(Values); ++I)
      llvm::support::endian::write64le(Record + I * 8, Values[I]);
    write32le(Record + 64, Data->RVA);
    write32le(Record + 68, 16);
    for (size_t I = 0; I < 4; ++I)
      llvm::support::endian::write16le(Record + 72 + I * 2,
                                       I < 3 ? 0xa000 + I * 8 : 0);
    std::vector<uint8_t> Code;
    auto RIP = [&](std::initializer_list<uint8_t> Opcode, uint32_t Field,
                   bool Immediate = false) {
      Code.insert(Code.end(), Opcode.begin(), Opcode.end());
      const auto Offset = Code.size();
      Code.resize(Offset + 4);
      write32le(Code.data() + Offset,
                Data->RVA + Field -
                    (Program->RVA + Code.size() + (Immediate ? 4 : 0)));
      if (Immediate)
        Code.insert(Code.end(), {0x34, 0x12, 0, 0});
    };
    RIP({0xc7, 0x05}, 32, true); // Unrelated scalar store.
    Code.push_back(0x90);        // First stopped observation at byte 10.
    RIP({0x48, 0x8b, 0x05}, 0);  // mov rax,[field 0]
    RIP({0x48, 0x89, 0x05}, 0);  // Same-value store.
    RIP({0x48, 0x8b, 0x05}, 8);  // mov rax,[field 1]
    RIP({0x48, 0xff, 0x05}, 8);  // inc qword ptr [field 1]
    RIP({0x48, 0xff, 0x0d}, 8);  // dec qword ptr [field 1]; restore bytes.
    Code.insert(Code.end(), {0x90, 0x31, 0xc0, 0xc3});
    std::copy(Code.begin(), Code.end(), Bytes.begin() + Program->FileOffset);
    write32le(Bytes.data() + Original.EntryOffset, Program->RVA);
    const auto Path = Scratch / "relocation-history.sys";
    test::writeFile(Path, Bytes);
    return Path;
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
  EXPECT_NE(Result->Diagnostic.find("additional owned OS resources"),
            std::string::npos);
  // Loader fields alone do not cover generated pointers or a fresh caller.
  // A successful model replay cannot certify the kernel loader boundary.
  Options.SnapshotOnly = true;
  Result = unpackFile(Input, Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Outcome, UnpackOutcome::Snapshot)
      << unpackResultJSON(*Result);
  EXPECT_NE(Result->Diagnostic.find("kernel image relocation state"),
            std::string::npos);
  EXPECT_NE(Result->Diagnostic.find("kernel image relocation state"),
            std::string::npos);
  EXPECT_EQ(Result->Profile, profile::ReportProfile);
  EXPECT_EQ(Result->EntryRVA, Original.Entry);
  EXPECT_EQ(Result->ProcessStop, "observer");
  ASSERT_FALSE(Result->Transfers.empty());
  EXPECT_TRUE(Result->Transfers.back().ProgramInvocation);
  EXPECT_TRUE(Result->RuntimeState.AdditionalStateInventoryKnown);
  EXPECT_TRUE(Result->RuntimeState.HasAdditionalDependencies);
  EXPECT_EQ(Result->RuntimeState.AdditionalDependencyReasons,
            (std::vector<std::string>{"DriverEntry continuation requires "
                                      "fresh-caller reconstruction"}));
  const auto Rebuilt = test::readImage(Result->Image);
  EXPECT_NE(Rebuilt.FileCharacteristics & llvm::COFF::IMAGE_FILE_DLL, 0u);
  EXPECT_EQ(Rebuilt.Entry, Original.Entry);
  for (const auto &I : Rebuilt.Imports)
    EXPECT_EQ(I.Module, "ntoskrnl.exe");
  const auto Output = Scratch / "recovered.sys";
  test::writeFile(Output, Result->Image);
  checkLifecycle(Output);
}

TEST_P(UnpackDriver,
       CheckedDriverEntryRejectsAClobberedCallerBeforePublishingStatus) {
  const auto *Program = Original.section(".prog");
  ASSERT_NE(Program, nullptr);
  const std::vector<uint8_t> Bodies[] = {
      // A valid callee restores R13 before returning STATUS_SUCCESS.
      {0x41, 0x55, 0x49, 0xc7, 0xc5, 77, 0, 0, 0, 0x41, 0x5d, 0x31, 0xc0, 0xc3},
      // Balanced RSP and a successful NTSTATUS do not excuse the clobber.
      {0x49, 0xc7, 0xc5, 77, 0, 0, 0, 0x31, 0xc0, 0xc3},
  };
  for (size_t I = 0; I < std::size(Bodies); ++I) {
    ASSERT_LE(Bodies[I].size(), Program->FileSize);
    auto Bytes = Original.File;
    std::copy(Bodies[I].begin(), Bodies[I].end(),
              Bytes.begin() + Program->FileOffset);
    write32le(Bytes.data() + Original.EntryOffset, Program->RVA);
    const auto Input = Scratch / "entry-caller.sys";
    test::writeFile(Input, Bytes);
    DriverOptions D;
    D.Backend = GetParam().Kind;
    D.Contract = ExecutionContract::CheckedX64;
    auto Run = emulateDriver(Input, D);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    if (!I) {
      EXPECT_EQ(Run->Stop, DriverStopReason::Returned) << Run->Diagnostic;
      EXPECT_EQ(Run->NTStatus, 0u);
    } else {
      EXPECT_EQ(Run->Stop, DriverStopReason::ModelError) << Run->Diagnostic;
      EXPECT_EQ(Run->Diagnostic,
                "DriverEntry returned with changed nonvolatile r13");
      EXPECT_FALSE(Run->NTStatus);
      EXPECT_FALSE(Run->UnloadCompleted);
    }
    EXPECT_TRUE(Run->Requests.empty());
  }
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
  EXPECT_TRUE(Result->RuntimeState.HasAdditionalDependencies);
  // The released MDL no longer contributes a dependency; projecting the
  // executed prefix onto a fresh DriverEntry caller remains unproved.
  EXPECT_EQ(Result->RuntimeState.AdditionalDependencyReasons,
            (std::vector<std::string>{"DriverEntry continuation requires "
                                      "fresh-caller reconstruction"}));
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
    expectSameOpaqueSnapshot(*Legacy, *Result);
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

TEST_P(UnpackDriver,
       UnusedFileSystemDeclarationCannotAuthorizeRuntimeRecovery) {
  const auto Input = packed(); // Its decoder never accesses a filesystem.
  Options.Driver.emplace();
  Options.Driver->FileSystem.emplace();
  std::vector<std::string> Reasons;
  for (unsigned Mode = 0; Mode != 3; ++Mode) {
    auto Current = Options;
    Current.SnapshotOnly = Mode == 1;
    Current.RestoreRuntime = Mode == 2;
    auto Result = unpackFile(Input, Current);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Outcome, Mode == 1 ? UnpackOutcome::Snapshot
                                         : UnpackOutcome::UnsupportedState)
        << Result->Diagnostic;
    EXPECT_EQ(Result->Image.empty(), Mode != 1);
    EXPECT_TRUE(Result->RuntimeState.HasAdditionalDependencies);
    if (!Mode)
      Reasons = Result->RuntimeState.AdditionalDependencyReasons;
    EXPECT_EQ(Result->RuntimeState.AdditionalDependencyReasons, Reasons);
    EXPECT_NE(std::find(Reasons.begin(), Reasons.end(),
                        "unreconstructed kernel or environment effects"),
              Reasons.end());
  }
}

TEST_P(UnpackDriver, UnusedLoaderDeclarationCannotAuthorizeRuntimeRecovery) {
  const auto Input = packed(); // Its decoder never accesses the loader view.
  Options.Driver.emplace();
  Options.Driver->LoaderView = DriverLoaderView{1, 0, "unused view control"};
  std::vector<std::string> Reasons;
  for (unsigned Mode = 0; Mode != 3; ++Mode) {
    auto Current = Options;
    Current.SnapshotOnly = Mode == 1;
    Current.RestoreRuntime = Mode == 2;
    auto Result = unpackFile(Input, Current);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Outcome, Mode == 1 ? UnpackOutcome::Snapshot
                                         : UnpackOutcome::UnsupportedState)
        << Result->Diagnostic;
    EXPECT_EQ(Result->Image.empty(), Mode != 1);
    EXPECT_TRUE(Result->RuntimeState.HasAdditionalDependencies);
    if (!Mode)
      Reasons = Result->RuntimeState.AdditionalDependencyReasons;
    EXPECT_EQ(Result->RuntimeState.AdditionalDependencyReasons, Reasons);
    EXPECT_NE(std::find(Reasons.begin(), Reasons.end(),
                        "unreconstructed kernel or environment effects"),
              Reasons.end());
  }
}

TEST_P(UnpackDriver, UnusedVersionQueryCannotAuthorizeRuntimeRecovery) {
  const auto Input = packed(); // Its decoder never calls the version query.
  Options.Driver.emplace();
  Options.Driver->VersionQuery =
      DriverVersionQuery{1, 42, "unused query", true};
  std::vector<std::string> Reasons;
  for (unsigned Mode = 0; Mode != 3; ++Mode) {
    auto Current = Options;
    Current.SnapshotOnly = Mode == 1;
    Current.RestoreRuntime = Mode == 2;
    auto Result = unpackFile(Input, Current);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Outcome, Mode == 1 ? UnpackOutcome::Snapshot
                                         : UnpackOutcome::UnsupportedState)
        << Result->Diagnostic;
    EXPECT_EQ(Result->Image.empty(), Mode != 1);
    EXPECT_TRUE(Result->RuntimeState.HasAdditionalDependencies);
    if (!Mode)
      Reasons = Result->RuntimeState.AdditionalDependencyReasons;
    EXPECT_EQ(Result->RuntimeState.AdditionalDependencyReasons, Reasons);
    EXPECT_NE(std::find(Reasons.begin(), Reasons.end(),
                        "unreconstructed kernel or environment effects"),
              Reasons.end());
  }
}

TEST_P(UnpackDriver, UnusedVaShadowQueryCannotAuthorizeRuntimeRecovery) {
  const auto Input = packed(); // Its decoder never calls this system query.
  Options.Driver.emplace();
  Options.Driver->KernelVaShadowQuery =
      DriverKernelVaShadowQuery{1, 42, "unused query"};
  std::vector<std::string> Reasons;
  for (unsigned Mode = 0; Mode != 3; ++Mode) {
    auto Current = Options;
    Current.SnapshotOnly = Mode == 1;
    Current.RestoreRuntime = Mode == 2;
    auto Result = unpackFile(Input, Current);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Outcome, Mode == 1 ? UnpackOutcome::Snapshot
                                         : UnpackOutcome::UnsupportedState)
        << Result->Diagnostic;
    EXPECT_EQ(Result->Image.empty(), Mode != 1);
    EXPECT_TRUE(Result->RuntimeState.HasAdditionalDependencies);
    if (!Mode)
      Reasons = Result->RuntimeState.AdditionalDependencyReasons;
    EXPECT_EQ(Result->RuntimeState.AdditionalDependencyReasons, Reasons);
    EXPECT_NE(std::find(Reasons.begin(), Reasons.end(),
                        "unreconstructed kernel or environment effects"),
              Reasons.end());
  }
}

TEST_P(UnpackDriver, UnusedPhysicalInventoryCannotAuthorizeRuntimeRecovery) {
  const auto Input = packed(); // Its decoder never queries physical ranges.
  Options.Driver.emplace();
  Options.Driver->PhysicalMemoryInventory = DriverPhysicalMemoryInventory{
      1, DriverPhysicalMemoryInventory::SourceKind::ModelIdentityWindow,
      "unused inventory"};
  std::vector<std::string> Reasons;
  for (unsigned Mode = 0; Mode != 3; ++Mode) {
    auto Current = Options;
    Current.SnapshotOnly = Mode == 1;
    Current.RestoreRuntime = Mode == 2;
    auto Result = unpackFile(Input, Current);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Outcome, Mode == 1 ? UnpackOutcome::Snapshot
                                         : UnpackOutcome::UnsupportedState)
        << Result->Diagnostic;
    EXPECT_EQ(Result->Image.empty(), Mode != 1);
    EXPECT_TRUE(Result->RuntimeState.HasAdditionalDependencies);
    if (!Mode)
      Reasons = Result->RuntimeState.AdditionalDependencyReasons;
    EXPECT_EQ(Result->RuntimeState.AdditionalDependencyReasons, Reasons);
    EXPECT_NE(std::find(Reasons.begin(), Reasons.end(),
                        "unreconstructed kernel or environment effects"),
              Reasons.end());
  }
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
  size_t Invocations = 0, Exports = 0, PureResumes = 0;
  llvm::Expected<std::vector<ExecutionWatch>> started(ProcessView &P) override {
    EXPECT_FALSE(P.watchedMemoryUnchanged());
    if (Fail == "started")
      return llvm::createStringError("driver observer failure");
    auto M = P.inputModule();
    return std::vector<ExecutionWatch>{{M->Base, M->Size}};
  }
  llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
  invoking(ProcessView &P) override {
    EXPECT_FALSE(P.watchedMemoryUnchanged());
    ++Invocations;
    auto M = P.inputModule();
    return std::vector<ExecutionWatch>{{M->Base, M->Size}};
  }
  llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
  watched(ProcessView &P, uint64_t) override {
    EXPECT_TRUE(P.watchedMemoryUnchanged());
    if (Fail == "watched")
      return llvm::createStringError("driver observer failure");
    Entries.push_back(P.programInvocation());
    return std::vector<ExecutionWatch>{};
  }
  llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
  resuming(ProcessView &P) override {
    if (P.watchedMemoryUnchanged())
      ++PureResumes;
    if (Fail == "resuming")
      return llvm::createStringError("driver observer failure");
    return std::nullopt;
  }
  llvm::Error exporting(ProcessView &P, const ProcessExportView &,
                        std::optional<uint64_t>) override {
    EXPECT_FALSE(P.watchedMemoryUnchanged());
    ++Exports;
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
  std::vector<bool> MemoryUnchanged;
  bool FinalMemoryUnchanged = false;
  llvm::Expected<std::vector<ExecutionWatch>> started(ProcessView &P) override {
    EXPECT_FALSE(P.watchedMemoryUnchanged());
    Base = P.inputModule()->Base;
    return std::vector<ExecutionWatch>{{Base + StopRVA, 1}};
  }
  std::vector<MemoryWriteWatch> writeWatches() const override {
    return {{Base + DataRVA, 4}};
  }
  llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
  resuming(ProcessView &P) override {
    MemoryUnchanged.push_back(P.watchedMemoryUnchanged());
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
    FinalMemoryUnchanged = P.watchedMemoryUnchanged();
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
  explicit DriverContextObserver(std::vector<uint64_t> Stops,
                                 std::vector<uint64_t> Fields = {})
      : Stops(std::move(Stops)), Fields(std::move(Fields)) {}
  std::shared_ptr<const ProcessRuntimeState> Initial;
  std::vector<std::shared_ptr<const ProcessRuntimeState>> Metadata, Contents;
  std::vector<std::vector<uint64_t>> FieldValues;
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
    std::vector<uint64_t> Values;
    for (auto RVA : Fields) {
      std::array<uint8_t, 8> Bytes;
      if (auto E = P.read(Base + RVA, Bytes))
        return std::move(E);
      Values.push_back(llvm::support::endian::read64le(Bytes.data()));
    }
    FieldValues.push_back(std::move(Values));
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
  std::vector<uint64_t> Fields;
  uint64_t Base = 0;
  size_t Index = 0;
};

class DirectDriver : public UnpackDriver {
protected:
  DriverOptions directScenario() const {
    auto D = scenario();
    D.Contract = ExecutionContract::DirectDriverX64;
    D.TraceMemoryWrites = false;
    return D;
  }
  std::filesystem::path codeInput(llvm::ArrayRef<uint8_t> Code) {
    const auto *Program = Original.section(".prog");
    if (!Program || Program->FileSize < Code.size()) {
      ADD_FAILURE() << "direct driver control has no code storage";
      return {};
    }
    auto Bytes = Original.File;
    std::copy(Code.begin(), Code.end(), Bytes.begin() + Program->FileOffset);
    write32le(Bytes.data() + Original.EntryOffset, Program->RVA);
    const auto Path = Scratch / "direct-driver.sys";
    test::writeFile(Path, Bytes);
    return Path;
  }
};

TEST_P(DirectDriver, PackedDriverUsesExistingAPIsCallbacksAndReturnGates) {
  const auto Packed = packed();
  ASSERT_FALSE(HasFailure());
  for (const auto &Input :
       {std::filesystem::path(NEVERD_UNPACK_DRIVER_FIXTURE), Packed}) {
    for (const uint64_t Base : {0x180000000ULL, 0xfffff80010000000ULL}) {
      SCOPED_TRACE(Input.string());
      SCOPED_TRACE(Base);
      auto D = directScenario();
      D.LoadAddress = Base;
      // Direct execution is bounded by time and events, not an invented
      // instruction count. A single instruction credit must not stop it.
      D.InstructionLimit = 1;
      DriverObserver Observer;
      auto Run = observeDriver(Input, D, Observer);
      ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
      ASSERT_EQ(Run->Stop, DriverStopReason::Returned) << Run->Diagnostic;
      EXPECT_EQ(Run->NTStatus, 0u);
      EXPECT_EQ(Run->Instructions, 0u);
      EXPECT_TRUE(Run->Writes.empty());
      EXPECT_TRUE(Run->UnloadCompleted);
      EXPECT_TRUE(Run->Devices.empty());
      ASSERT_EQ(Run->Requests.size(), 4u);
      for (const auto &Request : Run->Requests) {
        EXPECT_TRUE(Request.Completed);
        EXPECT_EQ(Request.IOStatus, 0u);
      }
      EXPECT_EQ(Run->Requests[1].Output,
                (std::vector<uint8_t>{0x4a, 0xbb, 0x5a, 0xa5}));
      EXPECT_EQ(Observer.Entries.size(), 6u);
      EXPECT_EQ(Observer.Invocations, 5u);
      EXPECT_GT(Observer.Exports, 0u);
    }
  }
}

TEST_P(DirectDriver, UnobservedDriverDispatchRegistrationSurvivesRequests) {
  // No ProcessObserver requests instruction stops. Registration must still
  // reach the kernel owner's committed-byte initialization inventory.
  for (const uint64_t Base : {0x180000000ULL, 0xfffff80010000000ULL}) {
    SCOPED_TRACE(Base);
    auto D = directScenario();
    D.LoadAddress = Base;
    auto Run = emulateDriver(NEVERD_UNPACK_DRIVER_FIXTURE, D);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    ASSERT_EQ(Run->Stop, DriverStopReason::Returned) << Run->Diagnostic;
    EXPECT_EQ(Run->NTStatus, 0u);
    EXPECT_TRUE(Run->UnloadCompleted);
    ASSERT_EQ(Run->Requests.size(), 4u);
    for (unsigned Major : {0u, 2u, 14u, 18u}) {
      EXPECT_GE(Run->MajorFunctions[Major], Base);
      EXPECT_LT(Run->MajorFunctions[Major] - Base, Original.Mapped.size());
      EXPECT_EQ(Run->MajorFunctions[Major], Run->MajorFunctions[0]);
    }
    for (const auto &Request : Run->Requests) {
      EXPECT_TRUE(Request.Completed);
      EXPECT_EQ(Request.IOStatus, 0u);
    }
    EXPECT_EQ(Run->Requests[1].Output,
              (std::vector<uint8_t>{0x4a, 0xbb, 0x5a, 0xa5}));
  }
}

TEST_P(DirectDriver, SameValueDispatchStoresKeepExactInitializationWidths) {
  // DRIVER_OBJECT.MajorFunction[1] is at byte 0x78. These stores leave its
  // initially zero bytes unchanged, so a byte comparison cannot witness
  // either the partial write or the two separate complete halves.
  const std::vector<std::vector<uint8_t>> Stores = {
      {0xc6, 0x41, 0x78, 0},                // mov byte [rcx+0x78],0
      {0xc7, 0x41, 0x78, 0, 0, 0, 0},       // mov dword [rcx+0x78],0
      {0x48, 0xc7, 0x41, 0x78, 0, 0, 0, 0}, // mov qword [rcx+0x78],0
      {0xc7, 0x41, 0x78, 0, 0, 0, 0,        // mov dword [rcx+0x78],0
       0xc7, 0x41, 0x7c, 0, 0, 0, 0}};      // mov dword [rcx+0x7c],0
  for (size_t I = 0; I < Stores.size(); ++I) {
    SCOPED_TRACE(I);
    auto Code = Stores[I];
    Code.insert(Code.end(), {0x31, 0xc0, 0xc3}); // xor eax,eax; ret
    const auto Input = codeInput(Code);
    ASSERT_FALSE(HasFailure());
    auto D = directScenario();
    D.Requests.clear();
    D.Unload = false;
    auto Run = emulateDriver(Input, D);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    if (I < 2) {
      EXPECT_EQ(Run->Stop, DriverStopReason::ModelError);
      EXPECT_NE(Run->Diagnostic.find("partially initialized dispatch pointer"),
                std::string::npos)
          << Run->Diagnostic;
    } else {
      EXPECT_EQ(Run->Stop, DriverStopReason::Returned) << Run->Diagnostic;
      EXPECT_EQ(Run->NTStatus, 0u);
      EXPECT_EQ(Run->MajorFunctions[1], 0u);
    }
  }
}

TEST_P(DirectDriver, RepeatedWatchResumesRetainOpaquePrefixDependencies) {
  // XOR r8d,r8d; INC r8; CMP r8d,3; JNE INC; XOR eax,eax; RET.
  const uint8_t Code[] = {0x45, 0x31, 0xc0, 0x49, 0xff, 0xc0, 0x41, 0x83,
                          0xf8, 0x03, 0x75, 0xf7, 0x31, 0xc0, 0xc3};
  const auto Input = codeInput(Code);
  ASSERT_FALSE(HasFailure());
  const uint64_t RVA = Original.section(".prog")->RVA;
  DriverContextObserver Observer({RVA + 3, RVA + 3, RVA + 3, RVA + 12});
  auto Run = observeDriver(Input, directScenario(), Observer);
  ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
  ASSERT_EQ(Run->Stop, DriverStopReason::Observer) << Run->Diagnostic;
  EXPECT_EQ(Run->Instructions, 0u);
  ASSERT_TRUE(Observer.Initial);
  EXPECT_FALSE(Observer.Initial->hasAdditionalDependencies());
  ASSERT_EQ(Observer.Contents.size(), 4u);
  for (size_t I = 0; I != Observer.Contents.size(); ++I) {
    const auto &State =
        static_cast<const WindowsDriverState &>(*Observer.Contents[I]);
    ASSERT_TRUE(State.Entry);
    EXPECT_EQ(State.Entry->Registers.at(CPURegister::X64R8)[0], I);
    EXPECT_NE(std::find(State.Reasons.begin(), State.Reasons.end(),
                        "DriverEntry continuation requires fresh-caller "
                        "reconstruction"),
              State.Reasons.end());
    EXPECT_NE(std::find(State.Reasons.begin(), State.Reasons.end(),
                        "unreconstructed kernel or environment effects"),
              State.Reasons.end());
  }
}

TEST_P(DirectDriver, WriteWatchesObserveCommittedAndSameValueStores) {
  for (const bool SameValue : {false, true}) {
    SCOPED_TRACE(SameValue);
    const auto Input = writeWatchInput(SameValue);
    ASSERT_FALSE(HasFailure());
    DriverWriteObserver Observer(
        Original.section(".pack")->RVA,
        Original.section(".prog")->RVA + (SameValue ? 30 : 50), false);
    auto Run = observeDriver(Input, directScenario(), Observer);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    ASSERT_EQ(Run->Stop, DriverStopReason::Observer) << Run->Diagnostic;
    EXPECT_EQ(Run->Instructions, 0u);
    EXPECT_TRUE(Run->Writes.empty());
    EXPECT_TRUE(Observer.FinalMemoryUnchanged);
    EXPECT_TRUE(llvm::none_of(Observer.MemoryUnchanged,
                              [](bool Unchanged) { return Unchanged; }));
    if (SameValue) {
      EXPECT_TRUE(Observer.Changes.empty());
      EXPECT_EQ(Observer.Final, (std::pair<uint32_t, uint32_t>{0, 0}));
      EXPECT_GE(Observer.MemoryUnchanged.size(), 4u);
    } else {
      EXPECT_EQ(Observer.Changes, (std::vector<std::pair<uint32_t, uint32_t>>{
                                      {1, 0}, {2, 0x11}}));
      EXPECT_EQ(Observer.Final, (std::pair<uint32_t, uint32_t>{2, 0x22}));
    }
  }
}

TEST_P(DirectDriver, ProcessorViewIsPreparedBeforeOriginalGSAndCR8Reads) {
  // MOV r8,gs:[0x188]; MOV rax,cr8; NOP; RET.
  const uint8_t Code[] = {0x65, 0x4c, 0x8b, 0x04, 0x25, 0x88, 0x01, 0x00,
                          0x00, 0x44, 0x0f, 0x20, 0xc0, 0x90, 0xc3};
  const auto Input = codeInput(Code);
  ASSERT_FALSE(HasFailure());
  DriverContextObserver Observer({Original.section(".prog")->RVA + 13});
  auto Run = observeDriver(Input, directScenario(), Observer);
  ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
  ASSERT_EQ(Run->Stop, DriverStopReason::Observer) << Run->Diagnostic;
  ASSERT_EQ(Observer.Contents.size(), 1u);
  const auto &State =
      static_cast<const WindowsDriverState &>(*Observer.Contents.front());
  ASSERT_TRUE(State.Entry);
  EXPECT_EQ(State.Entry->Registers.at(CPURegister::X64AX)[0], 0u);
  const auto Thread = State.Entry->Registers.at(CPURegister::X64R8)[0];
  EXPECT_GE(Thread, profile::KernelArenaBase);
  EXPECT_LT(Thread, profile::KernelArenaBase + profile::KernelArenaSize);
}

TEST_P(DirectDriver, ChangedIRQLAndGSFailBeforeObserversOrModeledAPIs) {
  const auto Import = std::find_if(
      Original.Imports.begin(), Original.Imports.end(),
      [](const auto &I) { return I.Name == "KeQueryActiveProcessors"; });
  ASSERT_NE(Import, Original.Imports.end());
  const uint64_t RVA = Original.section(".prog")->RVA;
  for (const bool ChangeGS : {false, true}) {
    for (const bool CallAPI : {false, true}) {
      SCOPED_TRACE(ChangeGS);
      SCOPED_TRACE(CallAPI);
      // MOV eax,2; MOV cr8,rax, or WRMSR(IA32_GS_BASE,0x12345000).
      std::vector<uint8_t> Code =
          ChangeGS
              ? std::vector<uint8_t>{0xb9, 0x01, 0x01, 0x00, 0xc0, 0xb8, 0x00,
                                     0x50, 0x34, 0x12, 0x31, 0xd2, 0x0f, 0x30}
              : std::vector<uint8_t>{0xb8, 2, 0, 0, 0, 0x44, 0x0f, 0x22, 0xc0};
      const uint64_t Stop = RVA + Code.size();
      if (CallAPI) {
        Code.insert(Code.end(), {0xff, 0x25}); // JMP [rip+IAT].
        const size_t At = Code.size();
        Code.resize(At + 4);
        write32le(Code.data() + At, Import->Slot - (RVA + Code.size()));
      } else
        Code.insert(Code.end(), {0x90, 0xc3});
      const auto Input = codeInput(Code);
      ASSERT_FALSE(HasFailure());
      DriverContextObserver Observer({Stop});
      auto Run = CallAPI ? emulateDriver(Input, directScenario())
                         : observeDriver(Input, directScenario(), Observer);
      ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
      EXPECT_EQ(Run->Stop, DriverStopReason::UnsupportedInstruction)
          << Run->Diagnostic;
      EXPECT_NE(
          Run->Diagnostic.find(ChangeGS ? "modeled GS base" : "modeled IRQL"),
          std::string::npos);
      EXPECT_TRUE(Observer.Contents.empty());
      EXPECT_TRUE(Run->Calls.empty());
      EXPECT_FALSE(Run->NTStatus);
    }
  }
}

TEST_P(DirectDriver, InteriorServiceGatesAndRawProcessorFaultsFailExplicitly) {
  for (const uint64_t Target :
       {profile::ThunkBase + 1,
        profile::KernelModuleBase + profile::KernelModuleCodeRVA + 1}) {
    SCOPED_TRACE(Target);
    std::vector<uint8_t> Code{0x48, 0xb8}; // MOV rax,imm64; JMP rax.
    Code.resize(10);
    llvm::support::endian::write64le(Code.data() + 2, Target);
    Code.insert(Code.end(), {0xff, 0xe0});
    const auto Input = codeInput(Code);
    ASSERT_FALSE(HasFailure());
    auto Run = emulateDriver(Input, directScenario());
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    EXPECT_EQ(Run->Stop, DriverStopReason::UnsupportedAPI) << Run->Diagnostic;
    EXPECT_NE(Run->Diagnostic.find("interior service gate"), std::string::npos);
    EXPECT_FALSE(Run->NTStatus);
  }
  const uint8_t Service[] = {0x90, 0x0f, 0x05, 0xc3}; // NOP; SYSCALL; RET.
  auto Run = emulateDriver(codeInput(Service), directScenario());
  ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
  EXPECT_EQ(Run->Stop, DriverStopReason::UnsupportedInstruction)
      << Run->Diagnostic;
  ASSERT_TRUE(Run->Fault);
  EXPECT_EQ(Run->Fault->Interrupt, 6u);
  EXPECT_EQ(Run->PC, Original.Base + Original.section(".prog")->RVA + 1);
  EXPECT_EQ(Run->Fault->PC, Run->PC);
  EXPECT_FALSE(Run->NTStatus);
}

TEST_P(DirectDriver, ProcessorIdentityAndTimestampRemainRuntimeDependencies) {
  for (unsigned Mode : {17u, 19u}) {
    SCOPED_TRACE(Mode);
    DriverPoolObserver Observer(Original.Entry);
    auto Run = observeDriver(packed(Mode), directScenario(), Observer);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    ASSERT_EQ(Run->Stop, DriverStopReason::Observer) << Run->Diagnostic;
    EXPECT_EQ(Run->Instructions, 0u);
    ASSERT_TRUE(Observer.Contents);
    const auto &State =
        static_cast<const WindowsDriverState &>(*Observer.Contents);
    EXPECT_NE(std::find(State.Reasons.begin(), State.Reasons.end(),
                        "unreconstructed kernel or environment effects"),
              State.Reasons.end());
  }
}

TEST_P(DirectDriver, DriverEntryReturnStillChecksTheNativeABI) {
  // MOV ebx,1; XOR eax,eax; RET. Returning success cannot hide clobbered RBX.
  const uint8_t Code[] = {0xbb, 1, 0, 0, 0, 0x31, 0xc0, 0xc3};
  const auto Input = codeInput(Code);
  ASSERT_FALSE(HasFailure());
  auto Run = emulateDriver(Input, directScenario());
  ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
  EXPECT_EQ(Run->Stop, DriverStopReason::ModelError) << Run->Diagnostic;
  EXPECT_NE(Run->Diagnostic.find("changed nonvolatile rbx"), std::string::npos);
  EXPECT_FALSE(Run->NTStatus);
}

TEST_P(DirectDriver, TimeoutAndAPIEventLimitsRemainEnforced) {
  const uint8_t Loop[] = {0x90, 0xeb, 0xfe};
  const auto Input = codeInput(Loop);
  ASSERT_FALSE(HasFailure());
  auto D = directScenario();
  D.TimeoutMilliseconds = 20;
  auto Run = emulateDriver(Input, D);
  ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
  EXPECT_EQ(Run->Stop, DriverStopReason::Timeout) << Run->Diagnostic;
  EXPECT_EQ(Run->Instructions, 0u);
  EXPECT_EQ(Run->PC, Original.Base + Original.section(".prog")->RVA + 1);
  D = directScenario();
  D.EventLimit = 1;
  Run = emulateDriver(NEVERD_UNPACK_DRIVER_FIXTURE, D);
  ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
  EXPECT_EQ(Run->Stop, DriverStopReason::EventLimit) << Run->Diagnostic;
  EXPECT_EQ(Run->Calls.size(), 1u);
}

TEST_P(UnpackDriver, RelocationFactsRequireUninterruptedLoaderWrites) {
  const auto Input = relocationHistoryInput();
  ASSERT_FALSE(HasFailure());
  const uint64_t ProgramRVA = Original.section(".prog")->RVA;
  const uint64_t DataRVA = Original.section(".pack")->RVA;
  for (uint64_t Base :
       {Original.Base, uint64_t(0x130000000), uint64_t(0xfffff80140000000)}) {
    SCOPED_TRACE(Base);
    DriverContextObserver Observer({ProgramRVA + 10, ProgramRVA + 46},
                                   {DataRVA, DataRVA + 8, DataRVA + 16});
    auto D = scenario();
    D.LoadAddress = Base;
    auto Run = observeDriver(Input, D, Observer);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    ASSERT_EQ(Run->Stop, DriverStopReason::Observer) << Run->Diagnostic;
    ASSERT_TRUE(Observer.Initial);
    ASSERT_EQ(Observer.Contents.size(), 2u);
    ASSERT_EQ(Observer.Metadata.size(), 2u);
    ASSERT_EQ(Observer.FieldValues.size(), 2u);
    const std::vector<uint64_t> Expected{
        Base + ProgramRVA + 16, Base + ProgramRVA + 24,
        UINT64_MAX - 16 + (Base - Original.Base)};
    EXPECT_EQ(Observer.FieldValues[0], Expected);
    EXPECT_EQ(Observer.FieldValues[1], Expected); // Equal bytes after writes.
    const auto &Initial =
        static_cast<const WindowsDriverState &>(*Observer.Initial);
    ASSERT_TRUE(Initial.Relocations);
    EXPECT_EQ(Initial.Relocations->ImageBase, Base);
    EXPECT_EQ(Initial.Relocations->PreferredBase, Original.Base);
    EXPECT_EQ(Initial.Relocations->FileHash,
              llvm::SHA256::hash(test::readFile(Input)));
    ASSERT_EQ(Initial.Relocations->Fields.size(), 3u);
    for (size_t I = 0; I < 3; ++I) {
      EXPECT_EQ(Initial.Relocations->Fields[I].RVA, DataRVA + I * 8);
      EXPECT_EQ(Initial.Relocations->Fields[I].LoadedValue, Expected[I]);
      EXPECT_TRUE(Initial.Relocations->Fields[I].LoaderWritePreserved);
    }
    for (size_t Stop = 0; Stop < 2; ++Stop)
      for (const auto &Capture :
           {Observer.Metadata[Stop], Observer.Contents[Stop]}) {
        const auto &State = static_cast<const WindowsDriverState &>(*Capture);
        ASSERT_TRUE(State.Relocations);
        ASSERT_EQ(State.Relocations->Fields.size(), 3u);
        for (size_t I = 0; I < 3; ++I)
          EXPECT_EQ(State.Relocations->Fields[I].LoaderWritePreserved,
                    Stop == 0 || I == 2);
      }
    // Earlier facts remain owned after later writes and session destruction.
    for (const auto &Field : Initial.Relocations->Fields)
      EXPECT_TRUE(Field.LoaderWritePreserved);
  }
}

TEST_P(UnpackDriver, SnapshotPreservesOnlyUnmodifiedLoaderRelocationFacts) {
  const uint64_t ProgramRVA = Original.section(".prog")->RVA;
  const uint32_t DataRVA = Original.section(".pack")->RVA;
  for (auto [Stop, DebugRecords] :
       {std::pair<uint64_t, bool>{10, false}, {46, false}, {10, true}}) {
    const auto Input = relocationHistoryInput(DebugRecords);
    ASSERT_FALSE(HasFailure());
    DriverContextObserver Observed({ProgramRVA + Stop},
                                   {DataRVA, DataRVA + 8, DataRVA + 16});
    auto D = scenario();
    D.LoadAddress = 0xfffff80140000000;
    auto OriginalRun = observeDriver(Input, D, Observed);
    ASSERT_TRUE(bool(OriginalRun)) << llvm::toString(OriginalRun.takeError());
    ASSERT_EQ(Observed.Contents.size(), 1u);
    const auto File = test::readFile(Input);
    auto InputImage = pe::Image::read(File);
    ASSERT_TRUE(bool(InputImage)) << llvm::toString(InputImage.takeError());
    const auto OriginalImage = test::readImage(File);
    Capture C{};
    C.Base = D.LoadAddress;
    C.EntryRVA = ProgramRVA + Stop;
    C.Memory = C.Baseline = OriginalImage.Mapped;
    C.PageAccess.assign(C.Memory.size() / 4096, Read | Write | Execute);
    C.OwnedState = Observed.Contents.front();
    for (size_t I = 0; I < Observed.FieldValues.front().size(); ++I)
      llvm::support::endian::write64le(C.Memory.data() + DataRVA + I * 8,
                                       Observed.FieldValues.front()[I]);
    auto Result = pe::rebuild(**InputImage, C, {});
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    // Retaining loader fields does not close generated-pointer or invocation
    // dependencies. The production orchestrator still refuses normal recovery.
    EXPECT_EQ(Result->RuntimeDependency,
              "kernel image relocation state requires reconstruction");
    const auto Image = test::readImage(Result->File);
    const auto Directory = Image.directory(llvm::COFF::BASE_RELOCATION_TABLE);
    EXPECT_EQ(Directory.Size != 0, !DebugRecords);
    EXPECT_EQ(bool(Image.FileCharacteristics &
                   llvm::COFF::IMAGE_FILE_RELOCS_STRIPPED),
              DebugRecords);
    std::set<uint32_t> Fields;
    for (uint64_t Offset = 0; Offset < Directory.Size;) {
      const auto *Block =
          Image.Mapped.data() + Directory.RelativeVirtualAddress + Offset;
      const uint32_t Page = read32le(Block), Size = read32le(Block + 4);
      ASSERT_GE(Size, 8u);
      ASSERT_LE(Size, Directory.Size - Offset);
      ASSERT_EQ(Size % 4, 0u);
      for (uint32_t At = 8; At < Size; At += 2) {
        const uint16_t Record = llvm::support::endian::read16le(Block + At);
        if (!Record)
          continue;
        ASSERT_EQ(Record >> 12, llvm::COFF::IMAGE_REL_BASED_DIR64);
        EXPECT_TRUE(Fields.insert(Page + (Record & 4095)).second);
      }
      Offset += Size;
    }
    const std::set<uint32_t> Expected =
        DebugRecords ? std::set<uint32_t>{}
        : Stop == 10 ? std::set<uint32_t>{DataRVA, DataRVA + 8, DataRVA + 16}
                     : std::set<uint32_t>{DataRVA + 16};
    EXPECT_EQ(Fields, Expected);
    for (uint32_t Field : Fields) {
      const auto Index = (Field - DataRVA) / 8;
      const uint64_t Value =
          Index == 2 ? UINT64_MAX - 16 + (Image.Base - Original.Base)
                     : Image.Base + ProgramRVA + 16 + Index * 8;
      EXPECT_EQ(llvm::support::endian::read64le(Image.Mapped.data() + Field),
                Value);
    }
    const auto Output = Scratch / (DebugRecords ? "debug-records.sys"
                                   : Stop == 10 ? "intact.sys"
                                                : "changed.sys");
    test::writeFile(Output, Result->File);
    auto Rebased = scenario();
    Rebased.LoadAddress = 0xfffff80150000000;
    DriverContextObserver Observer({ProgramRVA + Stop});
    auto Run = observeDriver(Output, Rebased, Observer);
    if (DebugRecords) {
      ASSERT_FALSE(bool(Run));
      EXPECT_NE(llvm::toString(Run.takeError()).find("relocat"),
                std::string::npos);
      continue;
    }
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    ASSERT_EQ(Run->Stop, DriverStopReason::Observer) << Run->Diagnostic;
    const auto &State =
        static_cast<const WindowsDriverState &>(*Observer.Initial);
    ASSERT_TRUE(State.Relocations);
    ASSERT_EQ(State.Relocations->Fields.size(), Expected.size());
    for (const auto &Field : State.Relocations->Fields)
      EXPECT_TRUE(Field.LoaderWritePreserved);
  }
}

TEST_P(UnpackDriver, SnapshotRelocationFactsBindSourceAndFormatWrites) {
  const auto Input = relocationHistoryInput();
  ASSERT_FALSE(HasFailure());
  const auto ProgramRVA = Original.section(".prog")->RVA;
  const auto DataRVA = Original.section(".pack")->RVA;
  DriverContextObserver Observer({ProgramRVA + 10});
  auto Run = observeDriver(Input, scenario(), Observer);
  ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
  ASSERT_TRUE(Observer.Initial);
  auto File = test::readFile(Input);
  auto Image = pe::Image::read(File);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const auto Bytes = test::readImage(File);
  Capture C{};
  C.Base = Original.Base;
  C.OwnedState = Observer.Initial;
  const auto &Initial =
      static_cast<const WindowsDriverState &>(*Observer.Initial);
  auto Encode = [&](const Capture &State,
                    llvm::ArrayRef<std::pair<uint64_t, uint64_t>> Replaced) {
    std::vector<uint8_t> Metadata;
    return pe::retainDriverRelocations(**Image, State, Bytes.Mapped, Replaced,
                                       Bytes.Mapped.size(), Metadata);
  };
  auto Intact = Encode(C, {});
  ASSERT_TRUE(bool(Intact)) << llvm::toString(Intact.takeError());
  EXPECT_EQ(Intact->Fields,
            (std::vector<uint32_t>{DataRVA, DataRVA + 8, DataRVA + 16}));
  // Replacing a field invalidates its role even if formatting wrote the same
  // numeric bytes. This is not inferred from a before/after byte comparison.
  const std::pair<uint64_t, uint64_t> Replaced{DataRVA + 8, DataRVA + 16};
  auto Edited = Encode(C, Replaced);
  ASSERT_TRUE(bool(Edited)) << llvm::toString(Edited.takeError());
  EXPECT_EQ(Edited->Fields, (std::vector<uint32_t>{DataRVA, DataRVA + 16}));
  for (unsigned Fault = 0; Fault < 3; ++Fault) {
    auto Wrong = std::make_shared<WindowsDriverState>(Initial);
    if (Fault == 0)
      Wrong->Relocations->FileHash[0] ^= 1;
    else if (Fault == 1)
      ++Wrong->Relocations->ImageBase;
    else
      Wrong->Relocations->Fields[1].RVA = DataRVA;
    Capture Broken = C;
    Broken.OwnedState = std::move(Wrong);
    auto Rejected = Encode(Broken, {});
    ASSERT_FALSE(bool(Rejected));
    EXPECT_NE(llvm::toString(Rejected.takeError()).find("relocation"),
              std::string::npos);
  }

  // A loader-authenticated value can become zero after rebasing. It must
  // remain file backed beyond an earlier debug record, rather than being
  // trimmed as section padding or losing its required extent to debug layout.
  const auto *Data = Original.section(".pack");
  ASSERT_GE(Data->FileSize, 1032u);
  const uint32_t ZeroField = DataRVA + 1024;
  const uint64_t LoadBase = 0xfffff80140000000;
  llvm::support::endian::write16le(File.data() + Data->FileOffset + 76, 0xa400);
  llvm::support::endian::write64le(File.data() + Data->FileOffset + 1024,
                                   Original.Base - LoadBase);
  const llvm::MemoryBufferRef Buffer(
      llvm::StringRef(reinterpret_cast<const char *>(File.data()), File.size()),
      "zero-relocation-field");
  auto Object = llvm::object::COFFObjectFile::create(Buffer);
  ASSERT_TRUE(bool(Object)) << llvm::toString(Object.takeError());
  const auto *Debug = (*Object)->getDataDirectory(llvm::COFF::DEBUG_DIRECTORY);
  ASSERT_NE(Debug, nullptr);
  const auto DebugOffset =
      reinterpret_cast<const uint8_t *>(Debug) - File.data();
  write32le(File.data() + DebugOffset, DataRVA + 64);
  write32le(File.data() + DebugOffset + 4,
            sizeof(llvm::object::debug_directory));
  const auto ZeroInput = Scratch / "zero-field.sys";
  test::writeFile(ZeroInput, File);
  auto D = scenario();
  D.LoadAddress = LoadBase;
  DriverContextObserver ZeroObserver({ProgramRVA + 10},
                                     {DataRVA, DataRVA + 8, ZeroField});
  auto ZeroRun = observeDriver(ZeroInput, D, ZeroObserver);
  ASSERT_TRUE(bool(ZeroRun)) << llvm::toString(ZeroRun.takeError());
  ASSERT_EQ(ZeroObserver.Contents.size(), 1u);
  EXPECT_EQ(ZeroObserver.FieldValues.front()[2], 0u);
  auto ZeroImage = pe::Image::read(File);
  ASSERT_TRUE(bool(ZeroImage)) << llvm::toString(ZeroImage.takeError());
  const auto OriginalZero = test::readImage(File);
  Capture ZeroCapture{};
  ZeroCapture.Base = LoadBase;
  ZeroCapture.EntryRVA = ProgramRVA + 10;
  ZeroCapture.Memory = ZeroCapture.Baseline = OriginalZero.Mapped;
  ZeroCapture.PageAccess.assign(ZeroCapture.Memory.size() / 4096,
                                Read | Write | Execute);
  ZeroCapture.OwnedState = ZeroObserver.Contents.front();
  for (size_t I = 0; I < 3; ++I)
    llvm::support::endian::write64le(ZeroCapture.Memory.data() +
                                         (I == 2 ? ZeroField : DataRVA + I * 8),
                                     ZeroObserver.FieldValues.front()[I]);
  auto ZeroOutput = pe::rebuild(**ZeroImage, ZeroCapture, {});
  ASSERT_TRUE(bool(ZeroOutput)) << llvm::toString(ZeroOutput.takeError());
  const auto Rebuilt = test::readImage(ZeroOutput->File);
  ASSERT_NE(Rebuilt.section(".pack"), nullptr);
  EXPECT_GE(Rebuilt.section(".pack")->FileSize, 1032u);
  EXPECT_EQ(llvm::support::endian::read64le(Rebuilt.Mapped.data() + ZeroField),
            0u);
  const auto ZeroSnapshot = Scratch / "zero-snapshot.sys";
  test::writeFile(ZeroSnapshot, ZeroOutput->File);
  D.LoadAddress += 0x10000000;
  DriverContextObserver Rebased({ProgramRVA + 10});
  auto Replay = observeDriver(ZeroSnapshot, D, Rebased);
  ASSERT_TRUE(bool(Replay)) << llvm::toString(Replay.takeError());
  ASSERT_TRUE(Rebased.Initial);
  const auto &State = static_cast<const WindowsDriverState &>(*Rebased.Initial);
  ASSERT_TRUE(State.Relocations);
  ASSERT_EQ(State.Relocations->Fields.size(), 3u);
  EXPECT_EQ(State.Relocations->Fields.back().RVA, ZeroField);
  EXPECT_EQ(State.Relocations->Fields.back().LoadedValue, 0x10000000u);
}

TEST_P(UnpackDriver, StoppedViewOwnsEntryRegistersFramesAndStack) {
  const auto Input = entryContextInput();
  ASSERT_FALSE(HasFailure());
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
    SCOPED_TRACE(int(Contract));
    auto D = scenario();
    D.Contract = Contract;
    DriverContextObserver Observer({Original.section(".prog")->RVA + 47});
    auto Run = observeDriver(Input, D, Observer);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    ASSERT_EQ(Run->Stop, DriverStopReason::Observer) << Run->Diagnostic;
    EXPECT_EQ(Run->Instructions, 10u);
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

TEST_P(UnpackDriver, EqualEntryValuesDoNotProveFreshCallerBindings) {
  const auto *Program = Original.section(".prog");
  ASSERT_NE(Program, nullptr);
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
    SCOPED_TRACE(int(Contract));
    for (unsigned Mode : {0u, 1u}) {
      SCOPED_TRACE(Mode);
      // Each XOR leaves the model's incoming zero value unchanged. A fresh
      // caller can supply another value, and skipping the AX write also loses
      // the computed DriverEntry status. Numeric equality is not a binding.
      const auto Register = Mode ? CPURegister::X64R13 : CPURegister::X64AX;
      const std::vector<uint8_t> Code =
          Mode ? std::vector<uint8_t>{0x45, 0x31, 0xed, 0x90, 0xc3}
               : std::vector<uint8_t>{0x31, 0xc0, 0x90, 0xc3};
      ASSERT_GE(Program->FileSize, Code.size());
      auto Bytes = Original.File;
      std::copy(Code.begin(), Code.end(), Bytes.begin() + Program->FileOffset);
      write32le(Bytes.data() + Original.EntryOffset, Program->RVA);
      const auto Input = Scratch / "equal-entry.sys";
      test::writeFile(Input, Bytes);
      auto D = scenario();
      D.Contract = Contract;
      DriverContextObserver Observer(
          {Program->RVA, Program->RVA + Code.size() - 2});
      auto Run = observeDriver(Input, D, Observer);
      ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
      ASSERT_EQ(Run->Stop, DriverStopReason::Observer) << Run->Diagnostic;
      EXPECT_EQ(Run->Instructions, 1u);
      ASSERT_TRUE(Observer.Initial);
      const auto &Initial =
          static_cast<const WindowsDriverState &>(*Observer.Initial);
      ASSERT_TRUE(Initial.Entry);
      EXPECT_FALSE(Initial.hasAdditionalDependencies());
      ASSERT_EQ(Initial.Entry->InitialRegisters.at(Register)[0], 0u);
      ASSERT_EQ(Observer.Contents.size(), 2u);
      const auto &Before =
          static_cast<const WindowsDriverState &>(*Observer.Contents.front());
      EXPECT_FALSE(Before.hasAdditionalDependencies());
      const auto &State =
          static_cast<const WindowsDriverState &>(*Observer.Contents.back());
      ASSERT_TRUE(State.Entry);
      const auto &Entry = *State.Entry;
      EXPECT_EQ(Entry.Registers.at(Register),
                Entry.InitialRegisters.at(Register));
      EXPECT_EQ(Entry.Registers.at(CPURegister::X64SP),
                Entry.InitialRegisters.at(CPURegister::X64SP));
      EXPECT_EQ(Entry.Frame, Entry.InitialFrame);
      EXPECT_TRUE(State.hasAdditionalDependencies());
      EXPECT_EQ(State.Reasons,
                (std::vector<std::string>{"DriverEntry continuation requires "
                                          "fresh-caller reconstruction"}));
    }
  }
}

TEST_P(UnpackDriver, LaterRestorationDoesNotChangeEarlierContextCaptures) {
  const auto Input = entryContextInput();
  ASSERT_FALSE(HasFailure());
  DriverContextObserver Observer({Original.section(".prog")->RVA + 47,
                                  Original.section(".prog")->RVA + 63});
  auto Run = observeDriver(Input, scenario(), Observer);
  ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
  ASSERT_EQ(Run->Stop, DriverStopReason::Observer) << Run->Diagnostic;
  EXPECT_EQ(Run->Instructions, 14u);
  ASSERT_EQ(Observer.Contents.size(), 2u);
  const auto &Before =
      static_cast<const WindowsDriverState &>(*Observer.Contents[0]);
  const auto &After =
      static_cast<const WindowsDriverState &>(*Observer.Contents[1]);
  ASSERT_TRUE(Before.Entry);
  ASSERT_TRUE(After.Entry);
  EXPECT_TRUE(Before.hasAdditionalDependencies());
  EXPECT_TRUE(After.hasAdditionalDependencies());
  EXPECT_EQ(After.Reasons,
            (std::vector<std::string>{"DriverEntry continuation requires "
                                      "fresh-caller reconstruction"}));
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
    EXPECT_TRUE(Observer.FinalMemoryUnchanged);
    EXPECT_TRUE(
        llvm::none_of(Observer.MemoryUnchanged, [](bool V) { return V; }));
  }
}

TEST_P(UnpackDriver, SameValueWritesDoNotSupplyAnUnchangedMemoryProof) {
  const auto Input = writeWatchInput(true);
  ASSERT_FALSE(HasFailure());
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
    SCOPED_TRACE(int(Contract));
    DriverOptions D;
    D.Backend = GetParam().Kind;
    D.Contract = Contract;
    D.TraceMemoryWrites = false;
    DriverWriteObserver Observer(Original.section(".pack")->RVA,
                                 Original.section(".prog")->RVA + 30, false);
    auto Run = observeDriver(Input, D, Observer);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    ASSERT_EQ(Run->Stop, DriverStopReason::Observer) << Run->Diagnostic;
    EXPECT_EQ(Run->Instructions, 3u);
    EXPECT_TRUE(Run->Writes.empty());
    EXPECT_TRUE(Observer.Changes.empty());
    EXPECT_EQ(Observer.Final, (std::pair<uint32_t, uint32_t>{0, 0}));
    ASSERT_EQ(Observer.MemoryUnchanged.size(), 4u);
    EXPECT_TRUE(
        llvm::none_of(Observer.MemoryUnchanged, [](bool V) { return V; }));
    EXPECT_TRUE(Observer.FinalMemoryUnchanged);
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
                                        "DriverEntry continuation requires "
                                        "fresh-caller reconstruction"}));
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
  EXPECT_EQ(Observer.Invocations, 5u);
  EXPECT_EQ(Observer.PureResumes, 6u);
  EXPECT_GT(Observer.Exports, 0u);
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
  if (GetParam().Kind == ExecutionBackendKind::Unicorn)
    expectSameOpaqueSnapshot(*Legacy, *Unscheduled);
  else
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

INSTANTIATE_TEST_SUITE_P(Native, DirectDriver,
                         testing::Values(Backend{"Kvm",
                                                 ExecutionBackendKind::KVM}),
                         [](const auto &I) { return I.param.Name; });

INSTANTIATE_TEST_SUITE_P(
    Backends, UnpackDriver,
    testing::Values(Backend{"Unicorn", ExecutionBackendKind::Unicorn},
                    Backend{"Kvm", ExecutionBackendKind::KVM},
                    Backend{"Whp", ExecutionBackendKind::WHP}),
    [](const auto &I) { return I.param.Name; });
} // namespace
} // namespace neverd::unpack
