//===- AndroidFileSystemStatusTests.cpp - Filesystem query boundaries ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxFileTestMetadata.h"
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessReport.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Endian.h"

namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x20000000;
constexpr uint64_t Name = Buffer + 512;
constexpr uint64_t Output = Buffer + 1024;

class AndroidFileSystemStatus : public testing::TestWithParam<const char *> {
protected:
  std::filesystem::path Path;
  ProcessOptions Options;

  void SetUp() override {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
    GTEST_SKIP() << "Clang and ld.lld Android fixtures unavailable";
#else
    Path = std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
           (std::string(GetParam()) + ".so");
    Options.Backend = ExecutionBackendKind::Unicorn;
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
    Options.Android->TraceLimit = 100000;
    Options.Android->Memory.push_back(
        {Buffer, 8192, std::vector<uint8_t>(8192, 0xa5), false});
    Options.Android->ReadMemory.push_back({Buffer, 8192});
    Options.LinuxFiles.emplace();
    Options.LinuxFiles->Files["/fixture/data"] = {0,    0xff, 0x41,
                                                  0x0a, 0x80, 0x5a};
    Options.LinuxFiles->Metadata["/fixture/data"] = fileTestMetadata();
    Options.Limits.Instructions = 100000;
#endif
  }

  void setPath(llvm::StringRef Text) {
    auto &Bytes = Options.Android->Memory[0].Bytes;
    std::copy(Text.begin(), Text.end(), Bytes.begin() + 512);
    Bytes[512 + Text.size()] = 0;
  }

  ProcessResult run(const char *Entry, std::vector<uint64_t> Args = {}) {
    Options.Android->EntrySymbol = Entry;
    Options.Android->Arguments = std::move(Args);
    return llvm::cantFail(
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options));
  }

  ProcessResult call(uint64_t Route, bool Descriptor, uint64_t Input,
                     uint64_t Destination) {
    return run("files_filesystem_status_call",
               {Route, uint64_t(Descriptor), Input, Destination, Buffer});
  }
};

TEST_P(AndroidFileSystemStatus, LookupErrorsPrecedeOutputAndPreserveAllBytes) {
  struct Case {
    bool Descriptor;
    uint64_t Input, Error;
    const char *Text;
  };
  const Case Cases[] = {{false, Name, 2, "/absent"},
                        {false, Name, 2, "/absent/child///"},
                        {false, Name, 2, ""},
                        {false, Name, 20, "/fixture/data/"},
                        {false, Name, 20, "/fixture/data////"},
                        {false, Name, 20, "/fixture/data/child"},
                        {false, 0, 14, ""},
                        {false, UINT64_MAX, 14, ""},
                        {true, 0xabcdef12ffffffffULL, 9, ""},
                        {true, 0xabcdef1200000003ULL, 9, ""}};
  for (uint64_t Route : {0u, 1u, 2u, 3u}) {
    for (const auto &C : Cases) {
      SCOPED_TRACE(testing::Message()
                   << Route << ':' << C.Input << ':' << C.Text);
      setPath(C.Text);
      auto Expected = Options.Android->Memory[0].Bytes;
      llvm::support::endian::write64le(Expected.data(),
                                       Route == 3 ? 83 : C.Error);
      for (uint64_t Destination : {Output, Name, uint64_t(1), UINT64_MAX}) {
        auto R = call(Route, C.Descriptor, C.Input, Destination);
        ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
        EXPECT_EQ(R.ReturnValue, uint64_t(0) - (Route == 3 ? C.Error : 1));
        ASSERT_EQ(R.MemorySnapshots.size(), 1u);
        EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
      }
    }
  }
}

TEST_P(AndroidFileSystemStatus, PathLengthLimitPrecedesOutputAndPathParsing) {
  setPath(std::string(4096, 'a'));
  for (uint64_t Route : {0u, 1u, 2u, 3u}) {
    auto Expected = Options.Android->Memory[0].Bytes;
    llvm::support::endian::write64le(Expected.data(), Route == 3 ? 83 : 36);
    auto R = call(Route, false, Name, 1);
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, uint64_t(0) - (Route == 3 ? 36 : 1));
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  }
}

TEST_P(AndroidFileSystemStatus, MissingObservationsDoNotInventFilesystemData) {
  auto Files = Options.LinuxFiles;
  for (uint64_t Route : {0u, 1u, 2u, 3u}) {
    for (const char *Text :
         {"/fixture/data", "/fixture", "/", "////", "/fixture///", "relative",
          "/fixture/../data", "/fixture//data"}) {
      SCOPED_TRACE(testing::Message() << Route << ':' << Text);
      setPath(Text);
      auto R = call(Route, false, Name, 1);
      EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Options.Android->Memory[0].Bytes);
    }
    for (uint64_t FD : {0u, 1u, 2u}) {
      auto R = call(Route, true, FD | 0xabcdef1200000000ULL, 1);
      EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
      EXPECT_EQ(R.Diagnostic,
                "Linux filesystem status requires explicit observations");
    }
    auto R = run("files_filesystem_status_live", {Route, 1, Buffer});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(R.Diagnostic,
              "Linux filesystem status requires explicit observations");
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Options.Android->Memory[0].Bytes);
    Options.LinuxFiles.reset();
    for (bool Descriptor : {false, true}) {
      auto R = call(Route, Descriptor, 1, 1);
      EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
      EXPECT_EQ(R.Diagnostic,
                "Linux file services require explicit linux_files input");
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Options.Android->Memory[0].Bytes);
    }
    Options.LinuxFiles = Files;
  }
}

TEST_P(AndroidFileSystemStatus, DynamicAliasesPreserveProviderLifetime) {
  Options.Android->Libraries["libfiles.so"] = {"statfs", "statfs64", "fstatfs",
                                               "fstatfs64"};
  for (uint64_t Descriptor : {0u, 1u}) {
    for (uint64_t Alias : {0u, 1u}) {
      const char *Name = Descriptor ? (Alias ? "fstatfs64" : "fstatfs")
                                    : (Alias ? "statfs64" : "statfs");
      auto R = run("files_filesystem_status_dynamic", {0, Alias, Descriptor});
      ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
      EXPECT_EQ(R.ReturnValue, 0u);
      auto Call = llvm::find_if(R.NativeCalls,
                                [&](const auto &C) { return C.Name == Name; });
      ASSERT_NE(Call, R.NativeCalls.end());
      EXPECT_EQ(Call->Library, "libfiles.so");
      R = run("files_filesystem_status_dynamic", {1, Alias, Descriptor});
      EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
      EXPECT_NE(R.Diagnostic.find("inactive dynamic library"),
                std::string::npos);
      EXPECT_FALSE(R.NativeCalls.back().Result);
    }
  }
}

TEST_P(AndroidFileSystemStatus, NativeTransportMatchesCompleteReports) {
  bool Compared = false;
  for (auto Backend : {ExecutionBackendKind::KVM, ExecutionBackendKind::WHP,
                       ExecutionBackendKind::HVF}) {
    ExecutionConfiguration C;
    C.Backend = Backend;
    C.Architecture = GuestArchitecture::AArch64;
    C.Contract = ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(C);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available)
      continue;
    for (const char *Entry :
         {"files_filesystem_status", "files_filesystem_status_bionic"}) {
      Options.Backend = ExecutionBackendKind::Unicorn;
      auto Software = run(Entry);
      ASSERT_EQ(Software.Stop, ProcessStopReason::Returned)
          << Software.Diagnostic;
      ASSERT_EQ(Software.ReturnValue, 0u);
      ASSERT_FALSE(Software.TraceTruncated);
      Options.Backend = Backend;
      auto Native = run(Entry);
      ASSERT_EQ(Native.Stop, ProcessStopReason::Returned) << Native.Diagnostic;
      Native.SelectedBackend = Software.SelectedBackend;
      Native.BackendSelectionReason = Software.BackendSelectionReason;
      EXPECT_EQ(processResultJSON(Native), processResultJSON(Software));
      Compared = true;
    }
  }
  if (!Compared)
    GTEST_SKIP() << "native AArch64 transports unavailable";
}

INSTANTIATE_TEST_SUITE_P(OptimizationAndPacking, AndroidFileSystemStatus,
                         testing::Values("files-O0-none", "files-O0-android",
                                         "files-O0-relr", "files-O2-none",
                                         "files-O2-android", "files-O2-relr"));
} // namespace
} // namespace neverd::emulation
