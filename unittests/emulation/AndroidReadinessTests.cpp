//===- AndroidReadinessTests.cpp - Scheduled readiness across guest threads
//===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original SVC/libc callers, copied wait state and deterministic virtual time.

#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/Support/Endian.h"

#include <tuple>

namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x20000000;
class AndroidReadiness : public testing::TestWithParam<
                             std::tuple<const char *, ExecutionBackendKind>> {
protected:
  ProcessOptions Options;
  std::filesystem::path Path;
  void SetUp() override {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
    GTEST_SKIP() << "Android fixtures unavailable";
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
    Options.Android->ThreadLimit = 3;
    Options.Android->TraceLimit = 50000;
    Options.Android->Memory.push_back(
        {Buffer, 8192, std::vector<uint8_t>(8192), false});
    Options.Android->ReadMemory.push_back({Buffer, 8192});
    Options.InstructionQuantum = 7;
    Options.Limits.Instructions = 50000;
    Options.LinuxTime.emplace().AdvanceOnIdle = true;
    Options.LinuxTime->Clocks[1] = {100, 0};
    Options.LinuxFiles.emplace();
    LinuxStreamConnection C0;
    C0.Peer = {2, 0, 0x23, 0x29, 127, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    C0.ReceiveEvents = {{{0x5a}, 0, 0, 7}};
    Options.LinuxNetwork.emplace().Connections = {C0};
#endif
  }
  ProcessResult run(uint64_t Route, uint64_t Action) {
    Options.Android->EntrySymbol = "readiness_workers";
    Options.Android->Arguments = {Buffer, Route, Action};
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    if (!R)
      llvm::report_fatal_error(R.takeError());
    return std::move(*R);
  }
  uint64_t word(const ProcessResult &R, unsigned I) {
    return llvm::support::endian::read64le(
        R.MemorySnapshots.at(0).Bytes.data() + 8 * I);
  }
  void completed(const ProcessResult &R, uint64_t Route, uint64_t Value) {
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
    EXPECT_FALSE(R.TraceTruncated);
    EXPECT_EQ(R.Trace.size(), R.Instructions);
    unsigned Selects = 0;
    if (Route) {
      for (const auto &C : R.NativeCalls)
        if (C.Name == "syscall" && C.Arguments[0] == 72) {
          ++Selects;
          EXPECT_EQ(C.Result, Value);
          EXPECT_EQ(C.ThreadID, 1001u);
        }
    } else {
      for (const auto &C : R.Services)
        if (C.Number == 72) {
          ++Selects;
          EXPECT_EQ(C.Result, Value);
          EXPECT_EQ(C.ThreadID, 1001u);
        }
    }
    EXPECT_EQ(Selects, 1u); // Resume completes the original event exactly once.
    EXPECT_EQ(word(R, 8), 100u);
    EXPECT_EQ(word(R, 9), 3u); // Other runnable thread sleeps before I/O wakes.
    EXPECT_EQ(word(R, 10), 456u);
    EXPECT_EQ(word(R, 12), 42u);
    EXPECT_EQ(word(R, 13), 43u);
    for (const auto &T : R.NativeThreads) {
      EXPECT_TRUE(T.Finished);
      EXPECT_FALSE(T.Waiting);
    }
  }
};

TEST_P(AndroidReadiness,
       CopiedInputsSurviveOtherThreadAndNativeErrnoStaysLocal) {
  for (uint64_t Route : {0u, 1u}) {
    auto R = run(Route, 1);
    completed(R, Route, 1);
    EXPECT_EQ(word(R, 0), 1u);
    EXPECT_EQ(word(R, 1), 73u);
    EXPECT_EQ(word(R, 2), 100u);
    EXPECT_EQ(word(R, 3), 7u);
    EXPECT_EQ(word(R, 4), 1u);
    EXPECT_EQ(word(R, 5), 0x5au);
    EXPECT_EQ(word(R, 6), 123u);
    EXPECT_EQ(word(R, 16), 0u);
    EXPECT_EQ(word(R, 17), 3u);
    EXPECT_EQ(word(R, 512), 8u);
  }
}
TEST_P(AndroidReadiness, TimeoutAndSameInstantArrivalHaveDistinctOutcomes) {
  for (uint64_t Arrival : {10u, 11u}) {
    Options.LinuxNetwork->Connections[0].ReceiveEvents[0].ReadyAtNS = Arrival;
    for (uint64_t Route : {0u, 1u}) {
      auto R = run(Route, 0);
      completed(R, Route, Arrival == 10);
      EXPECT_EQ(word(R, 0), Arrival == 10);
      EXPECT_EQ(word(R, 1), 73u);
      EXPECT_EQ(word(R, 3), 10u);
      EXPECT_EQ(word(R, 17), 0u);
      EXPECT_EQ(word(R, 512), Arrival == 10 ? 8u : 0u);
      EXPECT_EQ(word(R, 5), Arrival == 10 ? 0x5au : 0u);
    }
  }
}
TEST_P(AndroidReadiness, OutputFaultOnWakeUsesOriginalCallErrnoConvention) {
  for (uint64_t Route : {0u, 1u}) {
    auto R = run(Route, 2);
    const auto Value = Route ? UINT64_MAX : uint64_t(-14);
    completed(R, Route, Value);
    EXPECT_EQ(word(R, 0), Value);
    EXPECT_EQ(word(R, 1), Route ? 14u : 73u);
    EXPECT_EQ(word(R, 3), 7u);
    EXPECT_EQ(word(R, 17), 3u);
    EXPECT_EQ(word(R, 512), 8u);
    EXPECT_EQ(word(R, 5), 0u);
  }
}
TEST_P(AndroidReadiness, ClosedDescriptorStopsWithoutCompletingOrTimingOut) {
  for (uint64_t Route : {0u, 1u}) {
    auto R = run(Route, 3);
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(R.Diagnostic.find("descriptor changed"), std::string::npos);
    EXPECT_EQ(word(R, 6), 0u);
    EXPECT_EQ(word(R, 9), 3u);
    EXPECT_EQ(word(R, 17), 10u);
    EXPECT_EQ(word(R, 512), 8u);
    unsigned Pending = 0;
    for (const auto &C : R.NativeCalls)
      if (C.Name == "syscall" && C.Arguments[0] == 72 && !C.Result)
        ++Pending;
    for (const auto &C : R.Services)
      if (C.Number == 72 && !C.Result)
        ++Pending;
    EXPECT_EQ(Pending, 1u);
  }
}
TEST_P(AndroidReadiness, TimedInputsRequireTheExplicitClockPolicy) {
  Options.Android->EntrySymbol = "readiness_workers";
  Options.LinuxTime->AdvanceOnIdle = false;
  auto R = emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("linux_time.advance_on_idle"),
            std::string::npos);
}
INSTANTIATE_TEST_SUITE_P(
    Backends, AndroidReadiness,
    testing::Combine(
        testing::Values("readiness-O0-none", "readiness-O0-android",
                        "readiness-O0-relr", "readiness-O2-none",
                        "readiness-O2-android", "readiness-O2-relr"),
        testing::Values(ExecutionBackendKind::Unicorn,
                        ExecutionBackendKind::HVF, ExecutionBackendKind::KVM,
                        ExecutionBackendKind::WHP)));
} // namespace
} // namespace neverd::emulation
