//===- AndroidSleepTests.cpp - Original calls and suspended guest state --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessReport.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x20000000;
class AndroidSleep : public testing::TestWithParam<const char *> {
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
    Options.Android->Memory.push_back(
        {Buffer, 8192, std::vector<uint8_t>(8192, 0xa5), false});
    Options.Android->ReadMemory.push_back({Buffer, 8192});
    Options.InstructionQuantum = 7;
    Options.Limits.Instructions = 50000;
    Options.LinuxTime.emplace();
    Options.LinuxTime->AdvanceOnIdle = true;
    Options.LinuxTime->Clocks = {{0, {4294967297, 999999998}}, {1, {12, 0}}};
#endif
  }
  ProcessResult run(const char *Entry, std::vector<uint64_t> Arguments) {
    Options.Android->EntrySymbol = Entry;
    Options.Android->Arguments = std::move(Arguments);
    return llvm::cantFail(
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options));
  }
  void request(int64_t Seconds, int64_t Nanoseconds, size_t Offset = 4095) {
    auto *Bytes = Options.Android->Memory[0].Bytes.data() + Offset;
    llvm::support::endian::write64le(Bytes, uint64_t(Seconds));
    llvm::support::endian::write64le(Bytes + 8, uint64_t(Nanoseconds));
  }
  uint64_t word(const ProcessResult &R, size_t Index) {
    return llvm::support::endian::read64le(R.MemorySnapshots[0].Bytes.data() +
                                           8 * Index);
  }
};

TEST_P(AndroidSleep, EveryABIUsesCopiedTimespecAndRetainsRemainingAndErrno) {
  for (unsigned ThreadLimit : {1u, 2u}) {
    Options.Android->ThreadLimit = ThreadLimit;
    for (uint64_t Route : {0u, 1u, 2u}) {
      for (int64_t Nanoseconds : {0, 5}) {
        request(0, Nanoseconds);
        for (uint64_t Remaining : {uint64_t(1), Buffer + 4095, UINT64_MAX}) {
          auto R = run("sleep_call", {Route, Buffer + 4095, Remaining, Buffer});
          ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
          EXPECT_EQ(R.ReturnValue, 0u);
          ASSERT_EQ(R.MemorySnapshots.size(), 1u);
          auto Expected = Options.Android->Memory[0].Bytes;
          const uint64_t Values[] = {73,
                                     Nanoseconds ? 4294967298u : 4294967297u,
                                     Nanoseconds ? 3u : 999999998u,
                                     Nanoseconds ? 4294967298u : 4294967297u};
          for (size_t I = 0; I < std::size(Values); ++I)
            llvm::support::endian::write64le(Expected.data() + I * 8,
                                             Values[I]);
          EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
          for (const auto &Call : R.NativeCalls)
            EXPECT_TRUE(Call.Result) << Call.Name;
          for (const auto &Call : R.Services)
            EXPECT_EQ(Call.Result, 0u);
        }
      }
    }
  }
}

TEST_P(AndroidSleep,
       CompleteInputReadPrecedesValueChecksAndNeverAdvancesOnError) {
  for (uint64_t Route : {0u, 1u, 2u}) {
    for (const auto &Value : {LinuxTimespec{-1, 0}, LinuxTimespec{0, -1},
                              LinuxTimespec{0, 1000000000}}) {
      request(Value.Seconds, Value.Nanoseconds);
      auto R = run("sleep_call", {Route, Buffer + 4095, 1, Buffer});
      ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
      EXPECT_EQ(R.ReturnValue, Route == 2 ? uint64_t(0) - 22 : UINT64_MAX);
      EXPECT_EQ(word(R, 0), Route == 2 ? 73u : 22u);
      EXPECT_EQ(word(R, 1), 4294967297u);
      EXPECT_EQ(word(R, 2), 999999998u);
    }
    for (uint64_t Address : {uint64_t(1), Buffer + 8192 - 8, UINT64_MAX}) {
      auto R = run("sleep_call", {Route, Address, Buffer, Buffer});
      ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
      EXPECT_EQ(R.ReturnValue, Route == 2 ? uint64_t(0) - 14 : UINT64_MAX);
      EXPECT_EQ(word(R, 0), Route == 2 ? 73u : 14u);
      EXPECT_EQ(word(R, 1), 4294967297u);
      EXPECT_EQ(word(R, 2), 999999998u);
    }
  }
}

TEST_P(AndroidSleep,
       MultipleDeadlinesPreserveContextsAndCompleteJoinMutexAndOnce) {
  Options.Android->ThreadLimit = 4;
  Options.Android->Memory[0].Bytes.assign(8192, 0);
  for (uint64_t Interleave : {0u, 1u}) {
    auto R = run("sleep_workers", {Buffer, Interleave});
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    ASSERT_EQ(R.NativeThreads.size(), 4u);
    EXPECT_EQ(word(R, 0), 3u);
    if (!Interleave) {
      EXPECT_EQ(word(R, 1), 2u);
      EXPECT_EQ(word(R, 2), 3u);
      EXPECT_EQ(word(R, 3), 1u);
    } else {
      EXPECT_EQ(word(R, 5), 1u);
      EXPECT_EQ(word(R, 6), 1u);
    }
    for (size_t ID = 1; ID <= 3; ++ID) {
      const size_t Row = ID * 32;
      EXPECT_EQ(word(R, Row), 0u);
      EXPECT_EQ(word(R, Row + 1), 70 + ID);
      for (size_t Field : {2u, 3u, 4u})
        EXPECT_EQ(word(R, Row + Field), 0xfeedface00000000ULL + ID);
      EXPECT_EQ(word(R, Row + 5), 12u);
      EXPECT_EQ(word(R, Row + 6), ID == 1 ? 3u : ID == 2 ? 1u : 2u);
      EXPECT_EQ(word(R, Row + 7), 1000 + ID);
      EXPECT_EQ(word(R, Row + 8), R.NativeThreads[ID].TLS);
      EXPECT_TRUE(R.NativeThreads[ID].Finished);
      EXPECT_TRUE(R.NativeThreads[ID].Retired);
      if (Interleave)
        EXPECT_EQ(word(R, Row + 9), 1u);
    }
    for (const auto &Call : R.NativeCalls)
      EXPECT_TRUE(Call.Result) << Call.Name;
    ASSERT_EQ(R.Services.size(), 1u);
    EXPECT_EQ(R.Services[0].Number, 101u);
    EXPECT_EQ(R.Services[0].Result, 0u);
    EXPECT_EQ(processResultJSON(R),
              processResultJSON(run("sleep_workers", {Buffer, Interleave})));
  }
}

TEST_P(AndroidSleep,
       RunnableThreadKeepsSleeperAndOriginalEventIncompleteAtBudget) {
  Options.Android->ThreadLimit = 2;
  Options.Limits.Instructions = 5000;
  Options.Android->Memory[0].Bytes.assign(8192, 0);
  for (uint64_t Route : {0u, 1u, 2u}) {
    auto R = run("sleep_busy", {Buffer, Route});
    ASSERT_EQ(R.Stop, ProcessStopReason::InstructionLimit) << R.Diagnostic;
    EXPECT_EQ(R.Instructions, 5000u);
    ASSERT_EQ(R.NativeThreads.size(), 2u);
    EXPECT_TRUE(R.NativeThreads[0].Waiting);
    EXPECT_FALSE(R.NativeThreads[1].Waiting);
    EXPECT_EQ(word(R, 0), 0u);
    unsigned Pending = 0;
    for (const auto &Call : R.NativeCalls)
      if (Call.Name == "nanosleep" || Call.Name == "syscall") {
        EXPECT_FALSE(Call.Result);
        ++Pending;
      }
    for (const auto &Call : R.Services) {
      EXPECT_FALSE(Call.Result);
      ++Pending;
    }
    EXPECT_EQ(Pending, 1u);
  }
}

TEST_P(AndroidSleep, NativeTransportPreservesCompleteSleepAndWaitReports) {
  std::vector<ExecutionBackendKind> NativeBackends;
  for (auto Backend : {ExecutionBackendKind::KVM, ExecutionBackendKind::WHP,
                       ExecutionBackendKind::HVF}) {
    ExecutionConfiguration C;
    C.Backend = Backend;
    C.Architecture = GuestArchitecture::AArch64;
    C.Contract = ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(C);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability == BackendAvailability::Available)
      NativeBackends.push_back(Backend);
  }
  if (NativeBackends.empty())
    GTEST_SKIP() << "native AArch64 transports unavailable";
  Options.Android->ThreadLimit = 4;
  Options.Android->TraceLimit = 50000;
  Options.Android->Memory[0].Bytes.assign(8192, 0);
  for (uint64_t Interleave : {0u, 1u}) {
    Options.Backend = ExecutionBackendKind::Unicorn;
    auto Software = run("sleep_workers", {Buffer, Interleave});
    ASSERT_EQ(Software.Stop, ProcessStopReason::Returned)
        << Software.Diagnostic;
    ASSERT_FALSE(Software.TraceTruncated);
    for (auto Backend : NativeBackends) {
      Options.Backend = Backend;
      auto Native = run("sleep_workers", {Buffer, Interleave});
      ASSERT_EQ(Native.Stop, ProcessStopReason::Returned) << Native.Diagnostic;
      EXPECT_EQ(Native.SelectedBackend, Backend);
      Native.SelectedBackend = Software.SelectedBackend;
      Native.BackendSelectionReason = Software.BackendSelectionReason;
      EXPECT_EQ(processResultJSON(Native), processResultJSON(Software));
    }
  }
}

TEST_P(AndroidSleep, PolicyOverflowAndProviderLifetimeRefuseBeforeCompletion) {
  request(0, 1);
  Options.LinuxTime->AdvanceOnIdle = false;
  auto R = run("sleep_call", {0, Buffer + 4095, 0, Buffer});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("advance_on_idle"), std::string::npos);
  Options.LinuxTime->AdvanceOnIdle = true;
  Options.Android->ThreadLimit = 2;
  Options.LinuxTime->Clocks[0] = {INT64_MAX, 999999999};
  R = run("sleep_call", {2, Buffer + 4095, 0, Buffer});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  ASSERT_EQ(R.NativeThreads.size(), 1u);
  EXPECT_TRUE(R.NativeThreads[0].Waiting);
  ASSERT_EQ(R.Services.size(), 1u);
  EXPECT_FALSE(R.Services[0].Result);
  EXPECT_NE(R.Diagnostic.find("virtual clock"), std::string::npos);
  Options.LinuxTime->Clocks[0] = {1, 0};
  request(INT64_MAX, 0);
  R = run("sleep_call", {1, Buffer + 4095, 0, Buffer});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("deadline"), std::string::npos);
  EXPECT_FALSE(R.NativeCalls.back().Result);
  Options.Android->Libraries["libsleep-model.so"] = {"nanosleep"};
  R = run("sleep_dynamic", {0});
  ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
  R = run("sleep_dynamic", {1});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
}

INSTANTIATE_TEST_SUITE_P(OptimizationAndPacking, AndroidSleep,
                         testing::Values("sleep-O0-none", "sleep-O0-android",
                                         "sleep-O0-relr", "sleep-O2-none",
                                         "sleep-O2-android", "sleep-O2-relr"));
} // namespace
} // namespace neverd::emulation
