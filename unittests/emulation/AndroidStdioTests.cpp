//===- AndroidStdioTests.cpp - Independent guest stream contracts --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check bytes, FD offsets, indicators and lifetime across actual guest calls.
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessReport.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <cstring>
#include <tuple>

namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x20000000;
class AndroidStdio : public testing::TestWithParam<
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
    Options.Android->Memory.push_back(
        {Buffer, 8192, std::vector<uint8_t>(8192, 0xa5), false});
    std::fill_n(Options.Android->Memory[0].Bytes.begin(), 512, 0);
    Options.Android->ReadMemory = {{Buffer, 1024}};
    Options.LinuxFiles.emplace();
    const std::string Contents = "alpha\nbeta\nlast";
    Options.LinuxFiles->Files["/fixture/input"] =
        std::vector<uint8_t>(Contents.begin(), Contents.end());
    LinuxFileMetadata Metadata;
    Metadata.Mode = 0100444;
    Metadata.BlockSize = 8;
    Metadata.Size = Contents.size();
    Options.LinuxFiles->Metadata["/fixture/input"] = Metadata;
    Options.InstructionQuantum = 7;
#endif
  }
  ProcessResult run(const char *Entry, std::vector<uint64_t> Arguments) {
    Options.Android->EntrySymbol = Entry;
    Options.Android->Arguments = std::move(Arguments);
    return llvm::cantFail(
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options));
  }
  void returned(const ProcessResult &R) {
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0);
    for (const auto &Call : R.NativeCalls)
      EXPECT_TRUE(Call.Result) << Call.Name;
  }
  void memory(
      const ProcessResult &R, llvm::ArrayRef<uint64_t> Words,
      std::initializer_list<std::pair<unsigned, llvm::StringRef>> Text = {}) {
    ASSERT_FALSE(R.MemorySnapshots.empty());
    std::vector<uint8_t> Expected(Options.Android->Memory[0].Bytes.begin(),
                                  Options.Android->Memory[0].Bytes.begin() +
                                      1024);
    for (size_t I = 0; I < Words.size(); ++I)
      llvm::support::endian::write64le(Expected.data() + I * 8, Words[I]);
    for (const auto &[Offset, Bytes] : Text)
      std::copy(Bytes.begin(), Bytes.end(), Expected.begin() + Offset);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  }
  void mode(llvm::StringRef Text) {
    auto &Bytes = Options.Android->Memory[0].Bytes;
    std::copy(Text.begin(), Text.end(), Bytes.begin() + 2048);
    Bytes[2048 + Text.size()] = 0;
  }
};

TEST(AndroidFileNamespaceOptions, StrictAdmissionBoundsMutableAndDynamicState) {
  for (
      const char *Text :
      {R"({"linux_files":{"files":[],"dynamic_proc_fds":1}})",
       R"({"linux_files":{"files":[],"directories":["/a","/a"]}})",
       R"({"linux_files":{"files":[],"directories":["relative"]}})",
       R"({"linux_files":{"files":[{"path":"/a","bytes_hex":"01","write_limit":0}]}})",
       R"({"linux_files":{"files":[{"path":"/a","bytes_hex":"","write_limit":16777217}]}})",
       R"({"linux_files":{"files":[{"path":"/a","bytes_hex":""}],"directories":["/a/b"]}})",
       R"({"linux_files":{"files":[{"path":"/proc/self/fd/4","bytes_hex":""}],"dynamic_proc_fds":true}})"}) {
    auto O = processOptionsFromJSON(Text);
    EXPECT_FALSE(bool(O)) << Text;
    llvm::consumeError(O.takeError());
  }
  auto O = processOptionsFromJSON(
      R"({"linux_files":{"files":[{"path":"/a/file","bytes_hex":"6162","write_limit":32}],"directories":["/empty"],"dynamic_proc_fds":true}})");
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  EXPECT_EQ(O->LinuxFiles->WriteLimits.at("/a/file"), 32);
  EXPECT_TRUE(O->LinuxFiles->DynamicProcFDs);
  EXPECT_EQ(O->LinuxFiles->Directories, (std::vector<std::string>{"/empty"}));
}

TEST_P(AndroidStdio, BufferedLinesAndBlocksShareRawDescriptorOffsets) {
  auto R = run("stdio_sequence", {Buffer});
  returned(R);
  memory(R,
         {3, 97, 8, 1, 1, 6, 3, 15, 0, UINT64_MAX, 1, 0, 0, 15, 717, 0,
          UINT64_MAX, 9},
         {{512, llvm::StringRef("lpha\n\0", 6)}, {528, "beta\nlast"}});
}

TEST_P(AndroidStdio, SeekAndPositionAPIsDiscardReadAheadAndClearOnlyEOF) {
  auto R = run("stdio_seek", {Buffer});
  returned(R);
  memory(R, {97, 0, 3,          104, 0,  0, 11, 1, 1, 0, 0, 1,
             1,  0, UINT64_MAX, 22,  11, 0, 0,  0, 0, 0, 97},
         {{512, "last"}, {528, llvm::StringRef("last\0", 5)}});
}

TEST_P(AndroidStdio, FDOpenAdoptsWithoutDuplicatingOrResettingTheSharedCursor) {
  auto R = run("stdio_adopt", {Buffer});
  returned(R);
  memory(R, {2, 4, 2, 112, 10, 3, 0, 3, 0, 2, UINT64_MAX, 9, 97, 0},
         {{512, "al"}, {528, "ha"}});
}

TEST_P(AndroidStdio, API28RetriesAfterEOFAndReadOnlyFlushSetsErrno) {
  auto R = run("stdio_eof_retry", {Buffer});
  returned(R);
  memory(R, {0, UINT64_MAX, 1, 0, 97, 1, 1, 0, UINT64_MAX, 9, 1, 0, UINT64_MAX,
             0, 108});
}

TEST_P(AndroidStdio, ClosedFDKeepsBufferedBytesButRefillSetsPersistentError) {
  for (uint64_t Prefetch : {0, 1}) {
    auto R = run("stdio_closed_fd", {Buffer, Prefetch});
    returned(R);
    memory(R,
           {Prefetch ? 97u : 0u, 0, Prefetch ? 7u : 0u, UINT64_MAX, 1, 0, 9,
            UINT64_MAX, 3, 0, UINT64_MAX, 9},
           Prefetch
               ? std::initializer_list<
                     std::pair<unsigned, llvm::StringRef>>{{512, "lpha\nbe"}}
               : std::initializer_list<std::pair<unsigned, llvm::StringRef>>{});
  }
}

TEST_P(AndroidStdio, BlockReadsCountWholeObjectsAndKeepPartialObjectBytes) {
  auto R = run("stdio_transfer", {Buffer, 2, 10, Buffer + 512});
  returned(R);
  memory(R, {7, 15, 1, 0, 717}, {{512, "alpha\nbeta\nlast"}});
  R = run("stdio_transfer", {Buffer, 3, 5, Buffer + 512});
  returned(R);
  memory(R, {5, 15, 0, 0, 717}, {{512, "alpha\nbeta\nlast"}});
  R = run("stdio_transfer", {Buffer, 3, 3, Buffer + 512});
  returned(R);
  memory(R, {3, 9, 0, 0, 717}, {{512, "alpha\nbet"}});
}

TEST_P(AndroidStdio, ZeroAndOverflowDoNotInspectDestinationOrAllocateBuffer) {
  Options.LinuxFiles->Metadata.clear();
  for (auto [Size, Count] : {std::pair{uint64_t(0), UINT64_MAX},
                             std::pair{UINT64_MAX, uint64_t(0)}}) {
    auto R = run("stdio_transfer", {Buffer, Size, Count, UINT64_MAX});
    returned(R);
    memory(R, {0, 0, 0, 0, 717});
  }
  auto R = run("stdio_transfer", {Buffer, UINT64_MAX, 2, UINT64_MAX});
  returned(R);
  memory(R, {0, 0, 0, 1, 75});
}

TEST_P(AndroidStdio, LineBoundsPreserveErrnoAndOnlyWriteConsumedBytes) {
  auto R = run("stdio_line", {Buffer, 1, Buffer + 512});
  returned(R);
  memory(R, {1, 0, 0, 0, 717}, {{512, llvm::StringRef("\0", 1)}});
  for (uint64_t Size : {uint64_t(0), UINT64_MAX}) {
    R = run("stdio_line", {Buffer, Size, Buffer + 512});
    returned(R);
    memory(R, {0, 0, 0, 0, 22});
  }
  R = run("stdio_line", {Buffer, 3, Buffer + 512});
  returned(R);
  memory(R, {1, 2, 0, 0, 717}, {{512, llvm::StringRef("al\0", 3)}});
  R = run("stdio_line", {Buffer, 0x100000010, Buffer + 512});
  returned(R);
  memory(R, {1, 6, 0, 0, 717}, {{512, llvm::StringRef("alpha\n\0", 7)}});
}

TEST_P(AndroidStdio, UserBuffersAndDisabledBufferingHaveObservableReadAhead) {
  auto R = run("stdio_buffering", {Buffer, 0, Buffer + 512, 3});
  returned(R);
  memory(R, {0, 97, 3, 1, 717, 0}, {{512, "alp"}});
  R = run("stdio_buffering", {Buffer, 1, 0, 5});
  returned(R);
  memory(R, {0, 97, 5, 1, 717, 0});
  Options.LinuxFiles->Metadata.clear();
  R = run("stdio_buffering", {Buffer, 0, Buffer + 512, 3});
  returned(R);
  memory(R, {0, 97, 3, 1, 717, 0}, {{512, "alp"}});
  R = run("stdio_buffering", {Buffer, 2, UINT64_MAX, UINT64_MAX});
  returned(R);
  memory(R, {0, 97, 1, 1, 717, 0});
}

TEST_P(AndroidStdio, InvalidBufferModesPreserveDefaultAndErrno) {
  for (auto [Mode, Size] : {std::pair{uint64_t(3), uint64_t(5)},
                            std::pair{uint64_t(0), UINT64_MAX}}) {
    auto R = run("stdio_buffering", {Buffer, Mode, UINT64_MAX, Size});
    returned(R);
    memory(R, {UINT64_MAX, 97, 8, 1, 717, 0});
  }
}

TEST_P(AndroidStdio, RebufferingDiscardsUnreadBytesAtTheActualFDPosition) {
  for (uint64_t Kind : {0, 1, 2}) {
    auto R = run("stdio_rebuffer", {Buffer, Kind});
    returned(R);
    memory(R,
           {97, 0, 8, 116,
            Kind == 0   ? 9u
            : Kind == 1 ? 11u
                        : 15u,
            9},
           Kind == 1
               ? std::initializer_list<
                     std::pair<unsigned, llvm::StringRef>>{{512, "ta\n"}}
               : std::initializer_list<std::pair<unsigned, llvm::StringRef>>{});
  }
}

TEST_P(AndroidStdio, ZeroBlockSizeUsesBUFSIZAndAllocationFailureUsesOneByte) {
  Options.LinuxFiles->Metadata["/fixture/input"].BlockSize = 0;
  auto R = run("stdio_buffering", {Buffer, 0, 0, 0});
  returned(R);
  memory(R, {0, 97, 15, 1, 717, 0});
  Options.LinuxFiles->Metadata["/fixture/input"].BlockSize = INT32_MAX;
  R = run("stdio_line", {Buffer, 3, Buffer + 512});
  returned(R);
  memory(R, {1, 2, 0, 0, 12}, {{512, llvm::StringRef("al\0", 3)}});
  R = run("stdio_buffering", {Buffer, 0, 0, 0});
  returned(R);
  memory(R, {UINT64_MAX, 97, 1, 1, 12, 0});
}

TEST_P(AndroidStdio, MissingMetadataIsUnknownRatherThanAnInventedFstatFailure) {
  Options.LinuxFiles->Metadata.clear();
  auto R = run("stdio_sequence", {Buffer});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("explicit file metadata"), std::string::npos);
  EXPECT_FALSE(R.NativeCalls.back().Result);
}

TEST_P(AndroidStdio, OpenModesAndDescriptorFailuresHaveSeparateContracts) {
  for (const char *Text : {"", "q", "+r"}) {
    mode(Text);
    auto R = run("stdio_open_case", {Buffer, Buffer + 2048, 0, 0});
    returned(R);
    memory(R, {0, 22});
  }
  mode("rbtx");
  auto R = run("stdio_open_case", {Buffer, Buffer + 2048, 0, 0});
  returned(R);
  memory(R, {1, 717, 3, 0});
  for (auto [FD, Error] :
       {std::pair{UINT64_MAX, 9u}, std::pair{uint64_t(1), 22u}}) {
    R = run("stdio_open_case", {Buffer, Buffer + 2048, 1, FD});
    returned(R);
    memory(R, {0, Error});
  }
  for (const char *Text : {"w", "a", "r+", "rb+e"}) {
    mode(Text);
    R = run("stdio_open_case", {Buffer, Buffer + 2048, 0, 0});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_FALSE(R.NativeCalls.back().Result);
  }
  mode("re");
  R = run("stdio_open_case", {Buffer, Buffer + 2048, 1, 0});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
}

TEST_P(AndroidStdio, AbsentFileAndDescriptorLimitReturnKnownErrno) {
  mode("r");
  Options.LinuxFiles->Files.clear();
  Options.LinuxFiles->Metadata.clear();
  auto R = run("stdio_open_case", {Buffer, Buffer + 2048, 0, 0});
  returned(R);
  memory(R, {0, 2});
  Options.LinuxFiles->DescriptorLimit = 3;
  R = run("stdio_open_case", {Buffer, Buffer + 2048, 0, 0});
  returned(R);
  memory(R, {0, 24});
}

TEST_P(AndroidStdio, ForgedRetiredAndExposedFILEStorageCannotSucceed) {
  for (uint64_t Kind : {0, 1, 2, 3, 4}) {
    auto R = run("stdio_bad_handle", {Kind});
    EXPECT_NE(R.Stop, ProcessStopReason::Returned) << Kind;
    EXPECT_FALSE(R.ReturnValue);
  }
}

TEST_P(AndroidStdio, DirectReadReportsPartialKernelFaultButBufferedCopyFails) {
  Options.LinuxFiles->Files["/fixture/input"] = std::vector<uint8_t>(32, 'z');
  Options.Android->ReadMemory.push_back({Buffer + 8188, 4});
  auto R = run("stdio_transfer", {Buffer, 1, 32, Buffer + 8188});
  returned(R);
  memory(R, {4, 4, 0, 1, 14});
  ASSERT_EQ(R.MemorySnapshots.size(), 2u);
  EXPECT_EQ(R.MemorySnapshots[1].Bytes, std::vector<uint8_t>(4, 'z'));
  R = run("stdio_transfer", {Buffer, 1, 8, Buffer + 8188});
  EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure);
  EXPECT_FALSE(R.NativeCalls.back().Result);
}

TEST_P(AndroidStdio, SharedFILEKeepsThreadLocalErrnoAndStreamCursor) {
  Options.Android->ThreadLimit = 2;
  auto R = run("stdio_threads", {Buffer});
  returned(R);
  memory(R, {97, 0, 108, 2, UINT64_MAX, 9, 0, 0, 99, 112, 3, 717});
}

TEST_P(AndroidStdio, ExplicitDynamicProvidersShareOrdinaryStreamState) {
  Options.Android->Libraries["libstreams.so"] = {"fopen", "fgetc", "fclose"};
  auto R = run("stdio_dynamic", {Buffer, 0});
  returned(R);
  memory(R, {97, 1, 0});
  R = run("stdio_dynamic", {Buffer, 1});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_FALSE(R.NativeCalls.back().Result);
  EXPECT_EQ(R.NativeCalls.back().Library, "libstreams.so");
}

TEST_P(AndroidStdio,
       AllocationFailureUnwindsOnlyOwnedFDsAndCloseReclaimsBuffers) {
  Options.MemoryLimit = 512 * 1024;
  Options.StackSize = 64 * 1024;
  Options.OutputLimit = 4096;
  auto R = run("stdio_allocation_lifetime", {Buffer});
  returned(R);
  memory(R, {1, 12, 3, 1, 12, 0, 0, 100});
}

TEST_P(AndroidStdio, SeekPreservesReadErrorWhereRewindClearsBothIndicators) {
  auto R = run("stdio_error_indicators", {Buffer});
  returned(R);
  memory(R, {0, 1, 0, UINT64_MAX, 1, 1, 0, 1, 0, 97, 1, 0, 0});
}

TEST_P(AndroidStdio, WriteBuffersAndUpdateModesPreserveKernelCursorAndBytes) {
  Options.LinuxFiles->WriteLimits["/fixture/input"] = 64;
  auto R = run("stdio_write_sequence", {Buffer});
  returned(R);
  memory(R, {0, 3, 0, 0, 3, 5, 8, 0, 2, 4, 1, 0, 0, 8, 0, 8, 0},
         {{512, "ab"}, {528, "abcDXFGH"}, {544, "abcDXFGH"}, {768, "XbcD"}});
}
TEST_P(AndroidStdio,
       LineBufferAndBufferReplacementFlushAtTheDeclaredBoundaries) {
  Options.LinuxFiles->WriteLimits["/fixture/input"] = 64;
  auto R = run("stdio_line_write", {Buffer});
  returned(R);
  memory(R, {0, 0, 3, 0, 5, 0, 0, 7, 0}, {{512, "ab\ncdEF"}, {768, "cd\n"}});
}
TEST_P(AndroidStdio, AppendWritesUseCurrentObjectEndDespiteSeekAndOtherOpens) {
  Options.LinuxFiles->WriteLimits["/fixture/input"] = 64;
  auto R = run("stdio_append_write", {Buffer});
  returned(R);
  memory(R, {15, 0, 0, 1, 0, 1, 0, 16, 0, 0}, {{512, "Qlpha\nbeta\nlastZ"}});
}
TEST_P(AndroidStdio, FlushFailureRetiresPendingBytesAndCloseStillReleasesFD) {
  Options.LinuxFiles->WriteLimits["/fixture/input"] = 64;
  for (unsigned Kind : {0, 1, 2}) {
    auto R = run("stdio_flush_failure", {Buffer, Kind});
    returned(R);
    if (Kind == 1)
      memory(R, {0, 3, 0, UINT64_MAX, 9}, {{768, "abc"}});
    else
      memory(R, {0, 3, 0, Kind == 2 ? 0 : UINT64_MAX, 9, 1, 0, UINT64_MAX},
             {{768, Kind == 2 ? "abc12345" : "abc"}});
  }
}
TEST_P(AndroidStdio, MutableObjectBytesAreSharedButOnlyDupSharesTheCursor) {
  Options.LinuxFiles->WriteLimits["/fixture/input"] = 64;
  auto R = run("filesystem_shared", {Buffer});
  returned(R);
  memory(R, {2, 2, 4, 18, 1, 12, 7, 0, 19, 0, 0},
         {{512, "XYph"}, {528, llvm::StringRef("ast\0\0\0Z", 7)}});
}
TEST_P(AndroidStdio,
       DirectoryBuffersPreserveCookiesAndUseTheLiveDescriptorOwner) {
  Options.LinuxFiles->Directories = {"/fixture/sub"};
  auto R = run("filesystem_directory", {Buffer});
  returned(R);
  memory(R, {4, 717, 4, 0, 2, 5, 0, 0, 4, 4, 8, 4},
         {{512, "."},
          {528, ".."},
          {544, "input"},
          {560, "sub"},
          {576, ".."},
          {592, "."},
          {608, "alpha"}});
}
TEST_P(AndroidStdio, DirectoryErrorsDoNotConsumeRecordsOrAdoptRegularFiles) {
  auto R = run("filesystem_dir_errors", {Buffer});
  returned(R);
  memory(R, {UINT64_MAX, 22, 0, UINT64_MAX, 14, 0, UINT64_MAX, 20, UINT64_MAX,
             21, 1, 20, 1, 0, UINT64_MAX, 9, 0, UINT64_MAX, 22});
}
TEST_P(AndroidStdio, DynamicProcLinksTrackCloseDupAndDescriptorReuse) {
  Options.LinuxFiles->Directories = {"/fixture/sub"};
  Options.LinuxFiles->DynamicProcFDs = true;
  auto R = run("filesystem_proc", {Buffer});
  returned(R);
  ASSERT_FALSE(R.MemorySnapshots.empty());
  const auto &Bytes = R.MemorySnapshots[0].Bytes;
  for (auto [Index, Value] : {std::pair{0, 14ULL},
                              {1, UINT64_MAX},
                              {2, 2ULL},
                              {3, 4ULL},
                              {4, 12ULL},
                              {5, 7ULL},
                              {6, 0ULL},
                              {7, 0ULL}})
    EXPECT_EQ(llvm::support::endian::read64le(Bytes.data() + Index * 8), Value);
  for (auto [Offset, Name] : {std::pair{512, "/fixture/input"},
                              {544, "/fixture/sub"},
                              {576, "."},
                              {592, ".."},
                              {608, "0"},
                              {624, "1"},
                              {640, "2"},
                              {656, "3"},
                              {672, "4"}})
    EXPECT_EQ(std::string(Bytes.begin() + Offset,
                          Bytes.begin() + Offset + std::strlen(Name)),
              Name);
}
TEST_P(AndroidStdio, WriteZeroOverflowAndReadonlyPreserveTheirErrorContracts) {
  Options.LinuxFiles->WriteLimits["/fixture/input"] = 64;
  auto R = run("stdio_write_transfer", {Buffer, UINT64_MAX, 0, 9, 2});
  returned(R);
  memory(R, {0, 0, 717, 0, 0});
  R = run("stdio_write_transfer", {Buffer, UINT64_MAX, UINT64_MAX, 2, 2});
  returned(R);
  memory(R, {0, 0, 75, 1, 0});
  R = run("stdio_write_transfer", {Buffer, UINT64_MAX, 1, 1, 3});
  returned(R);
  memory(R, {0, 0, 9, 0, 0});
}
TEST_P(AndroidStdio, DirectWriteFaultsDifferFromBufferedCopies) {
  Options.LinuxFiles->WriteLimits["/fixture/input"] = 64;
  auto R = run("stdio_write_transfer", {Buffer, UINT64_MAX, 1, 8, 2});
  returned(R);
  memory(R, {0, 0, 14, 1, 0});
  R = run("stdio_write_transfer", {Buffer, UINT64_MAX, 1, 1, 0});
  EXPECT_NE(R.Stop, ProcessStopReason::Returned);
  EXPECT_FALSE(R.NativeCalls.back().Result);
}

TEST_P(AndroidStdio, AdoptedOutputFDUsesCaptureAndClosesTheOriginalDescriptor) {
  auto R = run("stdio_output_fd", {Buffer});
  returned(R);
  EXPECT_EQ(R.StandardOutput, "stream\n");
  memory(R, {1, 0, 0, 0, UINT64_MAX, 9});
}
TEST_P(AndroidStdio, FlushAllCompletesBufferedWritersAndAppendAtTheCurrentEnd) {
  Options.LinuxFiles->WriteLimits["/fixture/input"] = 64;
  auto R = run("stdio_flush_all", {Buffer});
  returned(R);
  memory(R, {0, 0, 3, 3, 0, 0, 6, 0, 0},
         {{512, "onetwo"}, {768, "onetwo"}, {800, "two"}});
}
TEST_P(AndroidStdio, DirectoryCacheSurvivesFDClosureUntilTheNextRefill) {
  auto R = run("filesystem_closed_dir", {Buffer});
  returned(R);
  memory(R, {0, 1, 9, UINT64_MAX, 9},
         {{512, "."}, {528, ".."}, {544, "input"}});
}

TEST_P(AndroidStdio, OnlyRefillsFlushOtherLineBufferedWriters) {
  Options.LinuxFiles->Files["/fixture/output"] = {};
  Options.LinuxFiles->WriteLimits["/fixture/output"] = 64;
  auto Metadata = Options.LinuxFiles->Metadata.at("/fixture/input");
  Metadata.Size = 0;
  Options.LinuxFiles->Metadata["/fixture/output"] = Metadata;
  for (uint64_t Direct : {0, 1}) {
    for (uint64_t Closed : {0, 1}) {
      SCOPED_TRACE(testing::Message()
                   << "direct=" << Direct << ", closed=" << Closed);
      auto R = run("stdio_cross_stream_read", {Buffer, Direct, Closed});
      returned(R);
      memory(R,
             {0, 0, 2, 0, Direct ? 9u : 1u, Closed && !Direct ? 9u : 717u,
              Closed && !Direct ? 1u : 0u, 0,
              Closed   ? UINT64_MAX
              : Direct ? 0u
                       : 2u,
              0, Closed ? UINT64_MAX : 0u},
             {{512, Direct ? "alpha\nbet" : "a"},
              {768, "xy"},
              {800, Direct ? "" : "alpha\nbe"}});
    }
  }
}

TEST_P(AndroidStdio, DirectReadPreservesPendingOutputUntilAnActualRefill) {
  Options.LinuxFiles->WriteLimits["/fixture/input"] = 64;
  auto R = run("stdio_direct_update", {Buffer});
  returned(R);
  memory(
      R, {0, 3, 5, 5, 0, 1, 12, 9, 0, 15, 0},
      {{512, "alpha"}, {528, "t"}, {544, "alphaabcta\nlast"}, {768, "ta\nl"}});
}

INSTANTIATE_TEST_SUITE_P(
    PackedAndOptimization, AndroidStdio,
    testing::Combine(testing::Values("stdio-O0-none", "stdio-O0-android",
                                     "stdio-O0-relr", "stdio-O2-none",
                                     "stdio-O2-android", "stdio-O2-relr"),
                     testing::Values(ExecutionBackendKind::Unicorn)));
INSTANTIATE_TEST_SUITE_P(
    Hypervisor, AndroidStdio,
    testing::Combine(testing::Values("stdio-O0-relr", "stdio-O2-relr"),
                     testing::Values(ExecutionBackendKind::HVF)));
} // namespace
} // namespace neverd::emulation
