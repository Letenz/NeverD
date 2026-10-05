//===- AndroidFileTests.cpp - Shared guest file state and boundaries ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxFileTestMetadata.h"
#include "gtest/gtest.h"
#include "os/linux/android/AndroidInternal.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"
#include "neverd/loader/ELF/ELFLoader.h"
#include "neverd/loader/ELF/ELFProgramLinking.h"

#include "llvm/Support/Endian.h"

#include <tuple>

namespace neverd::emulation {
namespace {
class AndroidFiles : public testing::TestWithParam<
                         std::tuple<const char *, ExecutionBackendKind>> {
protected:
  std::filesystem::path Path;
  ProcessOptions Options;
  void SetUp() override {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
    GTEST_SKIP() << "Clang and ld.lld Android fixtures unavailable";
#else
    Path = std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
           (std::string(std::get<0>(GetParam())) + ".so");
    Options.Backend = std::get<1>(GetParam());
    ExecutionConfiguration C;
    C.Backend = Options.Backend;
    C.Architecture = GuestArchitecture::AArch64;
    C.Contract = ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(C);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available)
      GTEST_SKIP() << Probe->Reason;
    Options.Android.emplace();
    Options.Android->Initialize = false;
    Options.Android->ThreadLimit = 2;
    Options.LinuxFiles.emplace();
    Options.LinuxFiles->Files["/fixture/data"] = {0,    0xff, 0x41,
                                                  0x0a, 0x80, 0x5a};
    Options.InstructionQuantum = 31;
    Options.Limits.Instructions = 200000;
#endif
  }
  ProcessResult run(const char *Entry, std::vector<uint64_t> Args = {}) {
    Options.Android->EntrySymbol = Entry;
    Options.Android->Arguments = std::move(Args);
    return llvm::cantFail(
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options));
  }
  void returned(const ProcessResult &R, uint64_t Value = 0) {
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, Value);
  }
};

TEST_P(AndroidFiles, OpensOwnCursorsAndReuseTheLowestAvailableDescriptor) {
  returned(run("files_sequence"));
}
TEST_P(AndroidFiles, PageFaultsPreserveCopiedBytesAndAdvanceOnlyTheirCursor) {
  returned(run("files_faults"));
}
TEST_P(AndroidFiles, ExhaustionHasNoOpenSideEffectsAndCloseReleasesCapacity) {
  Options.LinuxFiles->DescriptorLimit = 4;
  returned(run("files_capacity"));
}
TEST_P(AndroidFiles, ExistenceQueriesPreserveDescriptorCapacityAndCursors) {
  Options.LinuxFiles->DescriptorLimit = 4;
  returned(run("files_access"));
}
TEST_P(AndroidFiles, ExistenceQueriesImportOnlyTheTerminatedUserPath) {
  returned(run("files_access_faults"));
}
TEST_P(AndroidFiles, EmptyCatalogueContainsOnlyTheRootDirectory) {
  Options.LinuxFiles->Files.clear();
  Options.LinuxFiles->DescriptorLimit = 3;
  returned(run("files_access_empty"));
}
TEST_P(AndroidFiles, AccessImportsAndThreadsShareTheCatalogueWithPrivateErrno) {
  auto R = run("files_access_bionic");
  returned(R);
  bool OtherThreadAccess = false;
  for (const auto &Call : R.NativeCalls)
    if (Call.Name == "access" && Call.ThreadID == 1001) {
      EXPECT_EQ(Call.Result, 0u);
      OtherThreadAccess = true;
    }
  EXPECT_TRUE(OtherThreadAccess);
}
TEST_P(AndroidFiles, AccessRequiresExplicitInputsAndDoesNotInferPermissions) {
  for (unsigned Mode = 0; Mode < 8; ++Mode) {
    SCOPED_TRACE(Mode);
    if (!Mode)
      Options.LinuxFiles.reset();
    else if (!Options.LinuxFiles) {
      Options.LinuxFiles.emplace();
      Options.LinuxFiles->Files["/fixture/data"] = {};
      Options.LinuxFiles->Metadata["/fixture/data"] = fileTestMetadata();
    }
    auto R = run("files_access_unsupported", {Mode});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    EXPECT_FALSE(R.ReturnValue);
    ASSERT_FALSE(R.Services.empty());
    EXPECT_EQ(R.Services.back().Number, 48u);
    EXPECT_FALSE(R.Services.back().Result);
  }
}
TEST_P(AndroidFiles, AccessDynamicBindingsRetainProviderIdentityAndLifetime) {
  Options.Android->Libraries["libfiles.so"] = {"access", "faccessat"};
  for (unsigned At = 0; At < 2; ++At) {
    SCOPED_TRACE(At);
    auto R = run("files_access_dynamic", {0, At});
    returned(R);
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_EQ(R.NativeCalls.back().Name, At ? "faccessat" : "access");
    EXPECT_EQ(R.NativeCalls.back().Library, "libfiles.so");
    R = run("files_access_dynamic", {1, At});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
    EXPECT_FALSE(R.NativeCalls.back().Result);
  }
}
TEST_P(AndroidFiles, ImportsRawTrapsAndOtherThreadsShareTheSameOpenFile) {
  auto R = run("files_bionic");
  returned(R);
  bool OtherThreadRead = false;
  for (const auto &Call : R.NativeCalls)
    if (Call.Name == "read" && Call.ThreadID == 1001) {
      EXPECT_EQ(Call.Arguments[0], 3u);
      EXPECT_EQ(Call.Result, 2u);
      OtherThreadRead = true;
    }
  EXPECT_TRUE(OtherThreadRead);
  ASSERT_FALSE(R.Services.empty());
  EXPECT_EQ(R.Services.front().Number, 63u);
  EXPECT_EQ(R.Services.front().Result, 1u);
}
TEST_P(AndroidFiles, ExplicitStatusPreservesEveryFieldAndDescriptorCursor) {
  Options.LinuxFiles->Metadata["/fixture/data"] = fileTestMetadata();
  returned(run("files_status"));
}
TEST_P(AndroidFiles, StatusImportsAndThreadsShareObservationsWithPrivateErrno) {
  Options.LinuxFiles->Metadata["/fixture/data"] = fileTestMetadata();
  auto R = run("files_status_bionic");
  returned(R);
  bool OtherThreadStatus = false;
  for (const auto &Call : R.NativeCalls)
    if (Call.Name == "fstat64" && Call.ThreadID == 1001) {
      EXPECT_EQ(Call.Arguments[0], 3u);
      EXPECT_EQ(Call.Result, 0u);
      OtherThreadStatus = true;
    }
  EXPECT_TRUE(OtherThreadStatus);
}
TEST_P(AndroidFiles, StatusDynamicBindingsRetainProviderIdentityAndLifetime) {
  Options.LinuxFiles->Metadata["/fixture/data"] = fileTestMetadata();
  Options.Android->Libraries["libfiles.so"] = {"fstat64"};
  auto R = run("files_status_dynamic", {0});
  returned(R);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_EQ(R.NativeCalls.back().Name, "fstat64");
  EXPECT_EQ(R.NativeCalls.back().Library, "libfiles.so");
  R = run("files_status_dynamic", {1});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
  EXPECT_FALSE(R.NativeCalls.back().Result);
}
TEST_P(AndroidFiles, UnknownStatusAndPartialCopiesHaveNoInventedResult) {
  constexpr uint64_t Buffer = 0x20000000;
  const std::vector<uint8_t> Bytes(8192, 0xa5);
  Options.Android->Memory.push_back({Buffer, 8192, Bytes, false});
  Options.Android->ReadMemory.push_back({Buffer, 8192});
  for (unsigned Mode = 0; Mode < 4; ++Mode) {
    SCOPED_TRACE(Mode);
    if (Mode)
      Options.LinuxFiles->Metadata["/fixture/data"] = fileTestMetadata();
    auto R = run("files_status_unsupported", {Mode, Buffer});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    EXPECT_FALSE(R.ReturnValue);
    ASSERT_FALSE(R.Services.empty());
    EXPECT_EQ(R.Services.back().Number, 80u);
    EXPECT_FALSE(R.Services.back().Result);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots.front().Bytes, Bytes);
  }
}
TEST_P(AndroidFiles, AbsentInputsAndUnmodeledFormsCannotFabricateAResult) {
  for (unsigned Mode = 0; Mode <= 6; ++Mode) {
    SCOPED_TRACE(Mode);
    if (!Mode)
      Options.LinuxFiles.reset();
    else if (!Options.LinuxFiles) {
      Options.LinuxFiles.emplace();
      Options.LinuxFiles->Files["/fixture/data"] = {1};
    }
    auto R = run("files_unsupported", {Mode});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    EXPECT_FALSE(R.ReturnValue);
    ASSERT_FALSE(R.Services.empty());
    EXPECT_FALSE(R.Services.back().Result);
  }
}
TEST_P(AndroidFiles, DynamicFileBindingsRetainProviderIdentityAndLifetime) {
  Options.Android->Libraries["libfiles.so"] = {"open"};
  auto R = run("files_dynamic", {0});
  returned(R, 3);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_EQ(R.NativeCalls.back().Name, "open");
  EXPECT_EQ(R.NativeCalls.back().Library, "libfiles.so");
  R = run("files_dynamic", {1});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
  EXPECT_FALSE(R.NativeCalls.back().Result);
}

TEST_P(AndroidFiles, CallableImportEvidenceBelongsToTheExactSymbol) {
  ELFLoader Loader;
  auto Image = llvm::cantFail(Loader.load(Path));
  auto Facts = llvm::cantFail(readELFProgramLinking(Image));
  auto Symbol = llvm::find_if(Facts.Symbols,
                              [](const auto &S) { return S.Name == "open"; });
  ASSERT_NE(Symbol, Facts.Symbols.end());
  const uint64_t Index = Symbol - Facts.Symbols.begin();
  EXPECT_EQ(Symbol->Info & 15, llvm::ELF::STT_NOTYPE);
  auto Offset = [&](uint64_t VA) {
    for (const auto &P : Image.ELFMetadata->ProgramHeaders)
      if (P.Type == llvm::ELF::PT_LOAD && VA >= P.VirtualAddress &&
          VA - P.VirtualAddress < P.FileSize)
        return P.FileOffset + VA - P.VirtualAddress;
    ADD_FAILURE() << "fixture address is outside file-backed PT_LOAD";
    return uint64_t(0);
  };
  uint64_t SymbolTable = 0, PLT = 0, PLTSize = 0;
  for (const auto &D : Facts.Dynamic) {
    if (D.Tag == llvm::ELF::DT_SYMTAB)
      SymbolTable = Offset(D.Value);
    if (D.Tag == llvm::ELF::DT_JMPREL)
      PLT = Offset(D.Value);
    if (D.Tag == llvm::ELF::DT_PLTRELSZ)
      PLTSize = D.Value;
  }
  ASSERT_NE(SymbolTable, 0u);
  ASSERT_NE(PLT, 0u);
  uint64_t CallRelocation = 0;
  for (uint64_t I = PLT; I < PLT + PLTSize; I += 24)
    if (llvm::support::endian::read64le(Image.Raw.data() + I + 8) ==
        (Index << 32 | llvm::ELF::R_AARCH64_JUMP_SLOT))
      CallRelocation = I;
  ASSERT_NE(CallRelocation, 0u);
  EXPECT_TRUE(llvm::any_of(Facts.Relocations, [&](const auto &R) {
    return R.Symbol == Index && R.Type == llvm::ELF::R_AARCH64_GLOB_DAT;
  }));
  Options.Android->EntrySymbol = "files_bionic";
  for (unsigned Mode = 0; Mode < 4; ++Mode) {
    SCOPED_TRACE(Mode);
    auto Modified = Image;
    if (Mode == 1 || Mode == 3)
      llvm::support::endian::write32le(Modified.Raw.data() + CallRelocation + 8,
                                       llvm::ELF::R_AARCH64_GLOB_DAT);
    if (Mode == 2 || Mode == 3)
      Modified.Raw[SymbolTable + Index * 24 + 4] =
          (Symbol->Info & 0xf0) |
          (Mode == 2 ? llvm::ELF::STT_OBJECT : llvm::ELF::STT_FUNC);
    auto RAM = llvm::cantFail(PhysicalMemory::create(Options.MemoryLimit));
    auto Space = llvm::cantFail(AddressSpace::create(RAM, Options.MemoryLimit));
    auto Linked = android_model::loadImage(*Space, Modified, Options);
    if (Mode == 0 || Mode == 3) {
      ASSERT_TRUE(bool(Linked)) << llvm::toString(Linked.takeError());
      unsigned Bindings =
          llvm::count_if(Linked->Imports, [](const auto &Entry) {
            return Entry.second == "open";
          });
      EXPECT_EQ(Bindings, 1u);
    } else {
      ASSERT_FALSE(bool(Linked));
      EXPECT_EQ(llvm::toString(Linked.takeError()),
                "Android native: unmodeled imported data symbol: open");
    }
  }
}

INSTANTIATE_TEST_SUITE_P(
    Compiled, AndroidFiles,
    testing::Combine(testing::Values("files-O0-none", "files-O0-android",
                                     "files-O0-relr", "files-O2-none",
                                     "files-O2-android", "files-O2-relr"),
                     testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::HVF,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP)));
TEST(AndroidFileMemory,
     UnterminatedPathsAndRejectedOpensDoNotConsumeDescriptors) {
  ExecutionConfiguration Configuration;
  Configuration.Backend = ExecutionBackendKind::Unicorn;
  Configuration.Architecture = GuestArchitecture::AArch64;
  Configuration.Contract = ExecutionContract::CheckedUserAArch64;
  auto Probe = probeExecutionBackend(Configuration);
  ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
  if (Probe->Availability != BackendAvailability::Available)
    GTEST_SKIP() << Probe->Reason;
  auto Backend = createExecutionBackend(Configuration, 4 * 1024 * 1024);
  ASSERT_TRUE(bool(Backend)) << llvm::toString(Backend.takeError());
  auto &CPU = *Backend->CPU;
  constexpr uint64_t Buffer = 0x20000000;
  ASSERT_EQ(llvm::toString(CPU.addressSpace()->map(
                Buffer, 4096, Read | Write | UserAccessible)),
            "");
  ASSERT_EQ(llvm::toString(CPU.write(Buffer, std::vector<uint8_t>(4096, 'x'))),
            "");
  ProcessOptions Options;
  Options.LinuxFiles.emplace();
  Options.LinuxFiles->DescriptorLimit = 4;
  Options.LinuxFiles->Files["/data"] = {};
  const linux_model::MemoryLayout Layout{linux_model::UserLimitARM64, 4096};
  ProcessResult Result{ProcessProfile::AndroidNativeAArch64,
                       GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn,
                       {}};
  linux_model::LinuxServices Kernel(CPU, Layout, Buffer + 4096, Options,
                                    Result);
  auto Open = [&] {
    return llvm::cantFail(
        Kernel.handle(linux_model::ServiceKind::OpenAt,
                      {0, 56, {uint64_t(-100), Buffer, 0, 0, 0, 0}, {}}));
  };
  EXPECT_EQ(Open(), uint64_t(-36));
  ASSERT_EQ(llvm::toString(CPU.write(Buffer, {'/', 'd', 'a', 't', 'a', 0})),
            "");
  EXPECT_EQ(Open(), 3u);
  EXPECT_EQ(Open(), uint64_t(-24));
  EXPECT_EQ(llvm::cantFail(Kernel.handle(linux_model::ServiceKind::Read,
                                         {0, 63, {3, 1, 99, 0, 0, 0}, {}})),
            0u);
}
} // namespace
} // namespace neverd::emulation
