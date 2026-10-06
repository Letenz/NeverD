//===- AndroidFileStatusAtTests.cpp - Path and descriptor observations ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxFileTestMetadata.h"
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessReport.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x20000000;
constexpr uint64_t Name = Buffer + 512;
constexpr uint64_t Output = Buffer + 1024;

class AndroidFileStatusAt : public testing::TestWithParam<const char *> {
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
    setPath("/fixture/data");
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
  ProcessResult call(uint64_t Route, uint64_t Directory = uint64_t(-100),
                     uint64_t PathAddress = Name, uint64_t Destination = Output,
                     uint64_t Flags = 0) {
    return run("files_status_at_call",
               {Route, Directory, PathAddress, Destination, Flags, Buffer});
  }
};

TEST_P(AndroidFileStatusAt, AbsoluteAndEmptyPathsPreserveStatusAndOpenCursors) {
  Options.LinuxFiles->DescriptorLimit = 4;
  for (const char *Entry : {"files_status_at", "files_status_at_bionic"}) {
    auto R = run(Entry);
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Options.Android->Memory[0].Bytes);
  }
}

TEST_P(AndroidFileStatusAt,
       AllABIsPreserveUntouchedBytesAndTranslateOnlyErrno) {
  struct Case {
    uint64_t PathAddress, Destination, Flags, Error;
    const char *Text;
  };
  const Case Cases[] = {{1, Output, 0x40000000, 22, "/fixture/data"},
                        {1, Output, 0, 14, "/fixture/data"},
                        {Name, 1, 0, 2, "/absent"},
                        {Name, 1, 0, 20, "/fixture/data/child"},
                        {Name, Output, 0, 20, "/fixture/data/"},
                        {Name, Output, 0, 20, "/fixture/data////"},
                        {Name, Output, 0, 2, "/missing/"},
                        {Name, Output, 0, 2, "/missing/child///"},
                        {Name, 1, 0, 2, ""},
                        {Name, 1, 0x1000, 9, ""},
                        {Name, 1, 0, 14, "/fixture/data"},
                        {Name, UINT64_MAX, 0, 14, "/fixture/data"}};
  for (uint64_t Route : {0u, 1u, 2u, 3u}) {
    for (const auto &C : Cases) {
      SCOPED_TRACE(testing::Message()
                   << Route << ':' << C.Error << ':' << C.Text);
      setPath(C.Text);
      auto Expected = Options.Android->Memory[0].Bytes;
      llvm::support::endian::write64le(Expected.data(),
                                       Route == 3 ? 83 : C.Error);
      auto R = call(Route, UINT64_MAX, C.PathAddress, C.Destination, C.Flags);
      ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
      EXPECT_EQ(R.ReturnValue, uint64_t(0) - (Route == 3 ? C.Error : 1));
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
    }
  }
}

TEST_P(AndroidFileStatusAt, CompleteStatusCopyMayOverwriteItsPathname) {
  for (uint64_t Route : {0u, 1u, 2u, 3u}) {
    auto Expected = Options.Android->Memory[0].Bytes;
    llvm::support::endian::write64le(Expected.data(), 83);
    std::fill(Expected.begin() + 512, Expected.begin() + 640, 0);
    const std::pair<unsigned, uint64_t> Words[] = {{0, 0xfe12cd34},
                                                   {8, 0xfedcba9876543210ULL},
                                                   {16, 0x89abcdef000081a4ULL},
                                                   {24, 0xfedcba9887654321ULL},
                                                   {56, 16384},
                                                   {64, 0x1234567890ULL},
                                                   {72, 0x8000000000000001ULL},
                                                   {80, 123456789},
                                                   {88, 4294967297},
                                                   {96, 987654321},
                                                   {104, 0x7fffffffffffffffULL},
                                                   {112, 999999999}};
    for (auto [Offset, Value] : Words)
      llvm::support::endian::write64le(Expected.data() + 512 + Offset, Value);
    auto R = call(Route, UINT64_MAX, Name, Name, 0xabcdef0000000900ULL);
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  }
}

TEST_P(AndroidFileStatusAt,
       UnmodeledObjectsAndVersionSpecificInputsStayPending) {
  struct Case {
    const char *Text;
    uint64_t Directory, PathAddress, Destination, Flags;
    bool Catalogue, Metadata;
  };
  const Case Cases[] = {
      {"/fixture/data", uint64_t(-100), Name, Output, 0, false, false},
      {"/fixture/data", uint64_t(-100), Name, Output, 0, true, false},
      {"/fixture", uint64_t(-100), Name, Output, 0, true, true},
      {"/", uint64_t(-100), Name, Output, 0, true, true},
      {"////", uint64_t(-100), Name, Output, 0, true, true},
      {"/fixture///", uint64_t(-100), Name, Output, 0, true, true},
      {"fixture/data", uint64_t(-100), Name, Output, 0, true, true},
      {"/fixture/../data", uint64_t(-100), Name, Output, 0, true, true},
      {"", uint64_t(-100), Name, Output, 0x1000, true, true},
      {"", 1, Name, Output, 0x1000, true, true},
      {"", 1, 0, Output, 0x1000, true, true},
      {"/fixture/data", uint64_t(-100), Name, Output, 0x2000, true, true},
      {"/fixture/data", uint64_t(-100), Name, Output, 0x4000, true, true},
      {"/fixture/data", uint64_t(-100), Name, Buffer + 8192 - 64, 0, true,
       true}};
  auto Files = Options.LinuxFiles;
  for (uint64_t Route : {0u, 1u, 2u, 3u}) {
    for (const auto &C : Cases) {
      SCOPED_TRACE(testing::Message()
                   << Route << ':' << C.Text << ':' << C.Flags);
      Options.LinuxFiles = Files;
      if (!C.Catalogue)
        Options.LinuxFiles.reset();
      else if (!C.Metadata)
        Options.LinuxFiles->Metadata.clear();
      setPath(C.Text);
      auto R = call(Route, C.Directory, C.PathAddress, C.Destination, C.Flags);
      EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
      EXPECT_FALSE(R.ReturnValue);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Options.Android->Memory[0].Bytes);
      if (Route == 3) {
        ASSERT_FALSE(R.Services.empty());
        EXPECT_FALSE(R.Services.back().Result);
      } else {
        ASSERT_FALSE(R.NativeCalls.empty());
        EXPECT_FALSE(R.NativeCalls.back().Result);
      }
    }
  }
}

TEST_P(AndroidFileStatusAt, DynamicAliasesRetainProviderIdentityAndLifetime) {
  Options.Android->Libraries["libfiles.so"] = {"fstatat", "fstatat64"};
  for (uint64_t Alias : {0u, 1u}) {
    auto R = run("files_status_at_dynamic", {0, Alias});
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_EQ(R.NativeCalls.back().Name, Alias ? "fstatat64" : "fstatat");
    EXPECT_EQ(R.NativeCalls.back().Library, "libfiles.so");
    R = run("files_status_at_dynamic", {1, Alias});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
    EXPECT_FALSE(R.NativeCalls.back().Result);
  }
}

TEST_P(AndroidFileStatusAt, NativeTransportMatchesCompleteReports) {
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
    Options.LinuxFiles->DescriptorLimit = 4;
    for (const char *Entry :
         {"files_status_at", "files_status_at_bionic", "files_trailing_paths",
          "files_trailing_paths_bionic"}) {
      Options.Backend = ExecutionBackendKind::Unicorn;
      auto Software = run(Entry);
      ASSERT_EQ(Software.Stop, ProcessStopReason::Returned)
          << Software.Diagnostic;
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

INSTANTIATE_TEST_SUITE_P(OptimizationAndPacking, AndroidFileStatusAt,
                         testing::Values("files-O0-none", "files-O0-android",
                                         "files-O0-relr", "files-O2-none",
                                         "files-O2-android", "files-O2-relr"));
} // namespace
} // namespace neverd::emulation
