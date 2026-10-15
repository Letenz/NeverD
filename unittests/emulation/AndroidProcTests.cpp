//===- AndroidProcTests.cpp - Live proc through Android native programs ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Real guest thread, libc buffering and raw syscall paths share OS owners.
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"

#include <tuple>

namespace neverd::emulation {
namespace {
class AndroidProc : public testing::TestWithParam<
                        std::tuple<const char *, ExecutionBackendKind>> {
protected:
  std::filesystem::path Path;
  ProcessOptions Options;
  void SetUp() override {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
    GTEST_SKIP() << "Android fixture compiler unavailable";
#else
    Path = std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
           (std::string(std::get<0>(GetParam())) + ".so");
    Options.Backend = std::get<1>(GetParam());
    ExecutionConfiguration C;
    C.Backend = Options.Backend;
    C.Architecture = GuestArchitecture::AArch64;
    C.Contract = ExecutionContract::CheckedUserAArch64;
    auto P = probeExecutionBackend(C);
    ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
    if (P->Availability != BackendAvailability::Available)
      GTEST_SKIP() << P->Reason;
    Options.Android.emplace();
    Options.Android->Initialize = false;
    Options.Android->ThreadLimit = 2;
    Options.InstructionQuantum = 101;
    Options.Limits.Instructions = 1000000;
    Options.Limits.Events = 20000;
    Options.Limits.TimeoutMicroseconds = 30000000;
    Options.LinuxFiles.emplace();
    Options.LinuxFiles->DynamicProc = LinuxProcOptions{"fixture", 1};
    Options.LinuxFiles->Files["/fixture/value"] = {'a', 'b', 'c'};
    Options.LinuxFiles->WriteLimits["/fixture/value"] = 16384;
    Options.LinuxFiles->TrackWriteMetadata.insert("/fixture/value");
    LinuxFileMetadata M;
    M.Mode = 0100644;
    M.Size = 3;
    M.Inode = 42;
    M.LinkCount = 1;
    M.BlockSize = 4096;
    M.Blocks = 8;
    Options.LinuxFiles->Metadata["/fixture/value"] = M;
    Options.LinuxTime.emplace().Clocks[0] = {100, 7};
#endif
  }
  ProcessResult run(const char *Entry) {
    Options.Android->EntrySymbol = Entry;
    auto R = llvm::cantFail(
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options));
    EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
    return R;
  }
};
TEST_P(AndroidProc, RealThreadCreationAndExitReachDIRAndBufferedStatus) {
  auto R = run("proc_thread_lifecycle");
  ASSERT_EQ(R.NativeThreads.size(), 2u);
  EXPECT_TRUE(R.NativeThreads[1].Finished);
  EXPECT_TRUE(R.NativeThreads[1].Retired);
}
TEST_P(AndroidProc, RawAndBionicReadsObserveLiveMappingChanges) {
  run("proc_live_maps");
}
TEST_P(AndroidProc, NativeWritesPublishCompleteStatAndLiveFDInfo) {
  run("proc_write_stat");
}

INSTANTIATE_TEST_SUITE_P(
    Backends, AndroidProc,
    testing::Combine(testing::Values("proc-O0-none", "proc-O2-none",
                                     "proc-O0-android", "proc-O2-android",
                                     "proc-O0-relr", "proc-O2-relr"),
                     testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::HVF,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP)));
} // namespace
} // namespace neverd::emulation
