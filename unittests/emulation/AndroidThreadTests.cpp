//===- AndroidThreadTests.cpp - Independent cooperative pthread checks ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessReport.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/Support/Endian.h"

#include <set>

namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x20000000;
class AndroidThread : public testing::TestWithParam<const char *> {
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
    Options.InstructionQuantum = 7;
    Options.Limits.Instructions = 100000;
    Options.Android.emplace();
    Options.Android->Initialize = false;
    Options.Android->ThreadLimit = 8;
    Options.Android->TraceLimit = 16000;
    Options.Android->Memory.push_back({Buffer, 4096, {}, false});
    Options.Android->ReadMemory.push_back({Buffer, 1024});
    ExecutionConfiguration Configuration;
    Configuration.Backend = Options.Backend;
    Configuration.Architecture = GuestArchitecture::AArch64;
    Configuration.Contract = ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(Configuration);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available)
      GTEST_SKIP() << Probe->Reason;
#endif
  }
  ProcessResult run(const char *Entry, uint64_t Mode = 0) {
    Options.Android->EntrySymbol = Entry;
    Options.Android->Arguments = {Buffer, Mode};
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    if (!R) {
      ADD_FAILURE() << llvm::toString(R.takeError());
      return {ProcessProfile::AndroidNativeAArch64,
              GuestArchitecture::AArch64,
              Options.Backend,
              {}};
    }
    return std::move(*R);
  }
  uint64_t word(const ProcessResult &R, size_t Index) {
    if (R.MemorySnapshots.size() != 1 ||
        R.MemorySnapshots[0].Bytes.size() < (Index + 1) * 8) {
      ADD_FAILURE() << "missing memory snapshot";
      return UINT64_MAX;
    }
    return llvm::support::endian::read64le(R.MemorySnapshots[0].Bytes.data() +
                                           Index * 8);
  }
  void returned(const ProcessResult &R, uint64_t Value) {
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, Value);
  }
};
TEST_P(AndroidThread,
       IndependentContextsTLSAndNestedCallbacksShareOnlyProcessState) {
  auto R = run("threads_identity");
  returned(R, 0);
  ASSERT_EQ(R.NativeThreads.size(), 3u);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(word(R, 3), 77u);
  EXPECT_EQ(word(R, 5), 1000u);
  EXPECT_EQ(word(R, 10), 0u);
  EXPECT_EQ(word(R, 12), 1001u);
  EXPECT_EQ(word(R, 13), 1u);
  EXPECT_EQ(word(R, 1), word(R, 2) + 16);
  EXPECT_EQ(word(R, 4), word(R, 2) + 0x200);
  EXPECT_EQ(word(R, 11), R.NativeThreads[0].Handle);
  EXPECT_FALSE(R.NativeThreads[0].Retired);
  std::set<uint64_t> TLS{word(R, 2)};
  for (size_t I = 1; I <= 2; ++I) {
    const auto &T = R.NativeThreads[I];
    const size_t Row = I * 32;
    EXPECT_EQ(T.ID, 1000 + I);
    EXPECT_TRUE(T.Finished);
    EXPECT_TRUE(T.Retired);
    EXPECT_FALSE(T.Detached);
    EXPECT_EQ(word(R, Row), T.Handle);
    EXPECT_EQ(word(R, Row + 10), T.Handle);
    for (unsigned Offset : {1u, 2u, 3u, 13u, 15u})
      EXPECT_EQ(word(R, Row + Offset), T.ID);
    EXPECT_EQ(word(R, Row + 4), 1000u);
    EXPECT_TRUE(TLS.insert(word(R, Row + 5)).second);
    EXPECT_EQ(word(R, Row + 5), T.TLS);
    EXPECT_EQ(word(R, Row + 6), T.TLS + 16);
    EXPECT_EQ(word(R, Row + 7), 0u);
    EXPECT_EQ(word(R, Row + 8), 100 + I);
    EXPECT_EQ(word(R, Row + 11), T.TLS + 0x200);
    EXPECT_EQ(word(R, Row + 12), 0u);
    EXPECT_EQ(word(R, Row + 16), 0u);
    EXPECT_EQ(word(R, Row + 14), 2u);
    EXPECT_EQ(word(R, Row + 20), T.StackBase);
    EXPECT_EQ(word(R, Row + 21), T.StackSize);
    EXPECT_EQ(word(R, Row + 22), T.GuardSize);
    EXPECT_GE(word(R, Row + 9), T.StackBase + T.GuardSize);
    EXPECT_LT(word(R, Row + 9), T.StackBase + T.StackSize);
    EXPECT_EQ(word(R, Row + 23), 0xfeed0000 + I);
    EXPECT_EQ(word(R, Row + 24), 0x0123456789abcdefUL + I);
    EXPECT_EQ(word(R, Row + 25), 0x0123456789abcdefUL + I);
    EXPECT_EQ(word(R, Row + 26), I << 22);
    EXPECT_EQ(word(R, Row + 27), I);
    EXPECT_EQ(word(R, Row + 28), I == 1 ? 1u : 0u);
    EXPECT_EQ(word(R, 7 + I), 0xfedc000000000000UL + I);
  }
  ASSERT_GT(R.TraceThreads.size(), 8u);
  EXPECT_EQ(R.TraceThreads.front().Index, 0u);
  for (size_t I = 1; I < R.TraceThreads.size(); ++I) {
    EXPECT_GT(R.TraceThreads[I].Index, R.TraceThreads[I - 1].Index);
    EXPECT_NE(R.TraceThreads[I].ID, R.TraceThreads[I - 1].ID);
    EXPECT_LT(R.TraceThreads[I].Index, R.Trace.size());
  }
  for (const auto &C : R.NativeCalls) {
    ASSERT_TRUE(C.ThreadID);
    EXPECT_GE(*C.ThreadID, 1000u);
    EXPECT_LE(*C.ThreadID, 1002u);
    EXPECT_TRUE(C.Result) << C.Name;
  }
  for (const auto &S : R.Services) {
    ASSERT_TRUE(S.ThreadID);
    EXPECT_EQ(S.Result, S.ThreadID);
  }
  auto Again = run("threads_identity");
  EXPECT_EQ(processResultJSON(R), processResultJSON(Again));
}
TEST_P(AndroidThread, EntryReturnAndDetachedCompletionAreSeparateBoundaries) {
  Options.InstructionQuantum = 100000;
  for (uint64_t Mode : {0u, 1u}) {
    Options.Android->DrainThreads = false;
    auto R = run("threads_detached", Mode);
    returned(R, 73);
    ASSERT_EQ(R.NativeThreads.size(), 2u);
    EXPECT_TRUE(R.NativeThreads[0].Finished);
    EXPECT_FALSE(R.NativeThreads[1].Finished);
    EXPECT_TRUE(R.NativeThreads[1].Detached);
    EXPECT_EQ(word(R, 2), 0u);
    EXPECT_EQ(word(R, 4), 22u);
    EXPECT_EQ(word(R, 5), 22u);
    Options.Android->DrainThreads = true;
    R = run("threads_detached", Mode);
    returned(R, 73);
    ASSERT_EQ(R.NativeThreads.size(), 2u);
    EXPECT_TRUE(R.NativeThreads[1].Finished);
    EXPECT_TRUE(R.NativeThreads[1].Retired);
    EXPECT_EQ(word(R, 2), 1001u);
  }
}
TEST_P(AndroidThread, ThreadAndProcessExitKeepDifferentOwnership) {
  for (uint64_t Mode : {0u, 1u}) {
    auto R = run("threads_last_exit", Mode);
    EXPECT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
    EXPECT_EQ(R.ExitStatus, Mode ? 7u : 8u);
    EXPECT_FALSE(R.ReturnValue);
    ASSERT_EQ(R.NativeThreads.size(), 1u);
    EXPECT_TRUE(R.NativeThreads[0].Finished);
  }
  for (uint64_t Mode : {0u, 1u, 2u, 3u, 4u}) {
    auto R = run("threads_exit", Mode);
    ASSERT_EQ(R.NativeThreads.size(), 2u);
    if (Mode < 3) {
      returned(R, 74);
      EXPECT_EQ(word(R, 3), Mode ? 0u : 0xfedcba9876543210UL);
      EXPECT_EQ(word(R, 4), 1u);
      EXPECT_TRUE(R.NativeThreads[1].Retired);
    } else {
      EXPECT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
      EXPECT_EQ(R.ExitStatus, Mode == 3 ? 37u : 38u);
      EXPECT_FALSE(R.ReturnValue);
      EXPECT_EQ(word(R, 4), 0u);
      EXPECT_FALSE(R.NativeThreads[0].Finished);
    }
  }
}
TEST_P(AndroidThread,
       CreationRefusalPreservesErrnoAndDoesNotPublishAnIdentity) {
  Options.Android->ThreadLimit = 2;
  auto R = run("threads_limits");
  returned(R, 0);
  EXPECT_EQ(word(R, 4), 11u);
  EXPECT_EQ(word(R, 5), 0u);
  EXPECT_EQ(word(R, 6), 91u);
  ASSERT_EQ(R.NativeThreads.size(), 2u);
  for (uint64_t Mode : {1u, 4u}) {
    R = run("threads_limits", Mode);
    returned(R, 0);
    EXPECT_EQ(word(R, 0), 11u);
    EXPECT_EQ(word(R, 1), 0x12345678u);
    EXPECT_EQ(word(R, 6), 91u);
    EXPECT_EQ(R.NativeThreads.size(), 1u);
  }
  for (uint64_t Mode : {2u, 3u, 5u}) {
    R = run("threads_limits", Mode);
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    EXPECT_EQ(word(R, 1), 0x12345678u);
    EXPECT_EQ(R.NativeThreads.size(), 1u);
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_FALSE(R.NativeCalls.back().Result);
  }
}
TEST_P(AndroidThread, BadHandlesAndEntryPointersStopBeforeChildPublication) {
  returned(run("threads_invalid", 0), 35);
  returned(run("threads_invalid", 1), 3);
  for (uint64_t Mode : {2u, 3u, 4u, 5u, 6u}) {
    auto R = run("threads_invalid", Mode);
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
    EXPECT_EQ(R.NativeThreads.size(), 1u);
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_FALSE(R.NativeCalls.back().Result);
  }
}
TEST_P(AndroidThread,
       JoinCyclesStopAndCompletedUnjoinedThreadsRemainQueryable) {
  auto R = run("threads_join_cycle");
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
  ASSERT_EQ(R.NativeThreads.size(), 2u);
  EXPECT_TRUE(R.NativeThreads[0].Waiting);
  EXPECT_TRUE(R.NativeThreads[1].Waiting);
  EXPECT_FALSE(R.ReturnValue);
  for (uint64_t Mode : {0u, 1u}) {
    R = run("threads_join_finished", Mode);
    returned(R, 0);
    EXPECT_EQ(word(R, 3), 0u);
    EXPECT_TRUE(R.NativeThreads[1].Retired);
    if (!Mode)
      EXPECT_EQ(word(R, 4), 0x1234567800000042UL);
  }
}
TEST_P(AndroidThread, DynamicNamesAndProviderLifetimeUseTheSameThreadModel) {
  Options.Android->Libraries["libthread-model.so"] = {"pthread_create",
                                                      "pthread_join"};
  auto R = run("threads_dynamic");
  returned(R, 75);
  EXPECT_EQ(word(R, 4), 0x1234567800000042UL);
  for (const auto &Call : R.NativeCalls)
    if (Call.Name == "pthread_create" || Call.Name == "pthread_join") {
      EXPECT_EQ(Call.Library, "libthread-model.so");
      EXPECT_EQ(Call.ThreadID, 1000u);
      EXPECT_EQ(Call.Result, 0u);
    }
  R = run("threads_dynamic", 1);
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(R.NativeThreads.size(), 1u);
}
TEST_P(AndroidThread, LimitsRemainSharedAcrossSuspensionsAndTraceAttribution) {
  Options.Limits.Instructions = 300;
  auto R = run("threads_identity");
  EXPECT_EQ(R.Stop, ProcessStopReason::InstructionLimit) << R.Diagnostic;
  EXPECT_EQ(R.Instructions, 300u);
  EXPECT_FALSE(R.ReturnValue);
  Options.Limits.Instructions = 100000;
  Options.OutputLimit = Options.Android->TraceLimit * 8 +
                        Options.Android->ThreadLimit * 128 + 1024 + 16;
  R = run("threads_identity");
  returned(R, 0);
  EXPECT_TRUE(R.TraceTruncated);
  EXPECT_EQ(R.TraceThreads.size(), 1u);
  EXPECT_LT(R.Trace.size(), Options.Android->TraceLimit);
}
TEST_P(AndroidThread, GuardPagesCallbackExitAndCorruptTLSRemainExplicitStops) {
  auto R = run("threads_boundary", 0);
  EXPECT_EQ(R.Stop, ProcessStopReason::CPUFailure) << R.Diagnostic;
  ASSERT_EQ(R.NativeThreads.size(), 2u);
  ASSERT_TRUE(R.LastCPUExit && R.LastCPUExit->Fault);
  EXPECT_EQ(R.LastCPUExit->Fault->Address, R.NativeThreads[1].StackBase);
  EXPECT_FALSE(R.NativeThreads[1].Finished);
  R = run("threads_boundary", 1);
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
  EXPECT_FALSE(R.NativeThreads[1].Finished);
  EXPECT_FALSE(R.NativeCalls.back().Result);
  R = run("threads_boundary", 2);
  EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
  EXPECT_NE(R.Diagnostic.find("TLS identity"), std::string::npos);
  EXPECT_FALSE(R.NativeThreads[1].Finished);
}
TEST_P(AndroidThread, OnceWaitersObserveOneInitializerAndPreserveContexts) {
  for (uint64_t Mode : {0u, 1u, 2u}) {
    SCOPED_TRACE(Mode);
    if (Mode == 2)
      Options.Android->Libraries["libc.so"] = {"pthread_once"};
    auto R = run("threads_once_wait", Mode);
    returned(R, 0);
    ASSERT_EQ(R.NativeThreads.size(), 4u);
    EXPECT_EQ(word(R, 1), 1u);
    EXPECT_EQ(word(R, 3), 1u);
    EXPECT_EQ(word(R, 4), 0x123456789abcdef0ULL);
    EXPECT_EQ(word(R, 5), 0xfedcba9876543210ULL);
    EXPECT_EQ(word(R, 6), 0u);
    EXPECT_EQ(word(R, 7), 0u);
    EXPECT_EQ(word(R, 8), 2u);
    EXPECT_EQ(word(R, 9), 2u);
    std::set<uint64_t> Callers;
    unsigned OnceCalls = 0;
    bool Named = false;
    for (const auto &Call : R.NativeCalls) {
      if (Call.Name == "pthread_once") {
        ++OnceCalls;
        EXPECT_EQ(Call.Result, 0u);
        ASSERT_TRUE(Call.ThreadID);
        Callers.insert(*Call.ThreadID);
      }
      Named |= Call.Name == "dlsym" && Call.Symbol == "pthread_once" &&
               Call.Library == "libc.so";
    }
    EXPECT_EQ(OnceCalls, 6u);
    EXPECT_EQ(Callers, (std::set<uint64_t>{1000, 1001, 1002, 1003}));
    EXPECT_EQ(Named, Mode == 2);
    for (size_t I = 1; I <= 3; ++I) {
      const size_t Row = I * 32;
      const uint64_t Cookie = 0xfeedface0000ULL + I;
      EXPECT_EQ(word(R, Row + 1), 0u);
      EXPECT_EQ(word(R, Row + 2), Mode == 1 && I == 2 ? 0x123456789abcdef0ULL
                                                      : 0xfedcba9876543210ULL);
      for (size_t Offset : {3u, 8u, 9u})
        EXPECT_EQ(word(R, Row + Offset), Cookie);
      EXPECT_EQ(word(R, Row + 4), 70u + I);
      EXPECT_EQ(word(R, Row + 5), 1000u + I);
      EXPECT_EQ(word(R, Row + 6), R.NativeThreads[I].TLS);
      EXPECT_EQ(word(R, Row + 7), 1u);
      EXPECT_TRUE(R.NativeThreads[I].Retired);
      EXPECT_FALSE(R.NativeThreads[I].Waiting);
    }
  }
}
TEST_P(AndroidThread, OnceWaitsRejectUnknownRecursiveAndReinitializedControls) {
  Options.Android->Memory.push_back({Buffer + 4096, 4096, {}, false});
  for (uint64_t Mode : {0u, 1u, 8u}) {
    SCOPED_TRACE(Mode);
    auto R = run("threads_once_failure", Mode);
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    EXPECT_EQ(R.NativeThreads.size(), 1u);
    EXPECT_NE(R.Diagnostic.find("pthread_once"), std::string::npos);
    EXPECT_EQ(word(R, 7), 0u);
    for (const auto &Call : R.NativeCalls)
      if (Call.Name == "pthread_once")
        EXPECT_FALSE(Call.Result);
  }
}
TEST_P(AndroidThread, OnceAndJoinCyclesKeepAllWaitsIncomplete) {
  Options.Android->Memory.push_back({Buffer + 4096, 4096, {}, false});
  for (uint64_t Mode : {2u, 3u}) {
    SCOPED_TRACE(Mode);
    auto R = run("threads_once_failure", Mode);
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    EXPECT_NE(R.Diagnostic.find("pending waits"), std::string::npos);
    ASSERT_EQ(R.NativeThreads.size(), Mode == 2 ? 2u : 3u);
    for (const auto &Thread : R.NativeThreads) {
      EXPECT_TRUE(Thread.Waiting);
      EXPECT_FALSE(Thread.Finished);
    }
    for (const auto &Call : R.NativeCalls)
      if (Call.Name == "pthread_once" || Call.Name == "pthread_join")
        EXPECT_FALSE(Call.Result);
  }
}
TEST_P(AndroidThread, OnceWakeupsRecheckGuestMemoryBeforeCompletingWaiters) {
  Options.Android->Memory.push_back({Buffer + 4096, 4096, {}, false});
  // Service boundaries order the release, callback return and third-thread
  // mutation without preempting the few instructions before each import.
  Options.InstructionQuantum = 1000;
  for (uint64_t Mode : {4u, 5u, 6u}) {
    SCOPED_TRACE(Mode);
    auto R = run("threads_once_failure", Mode);
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
    unsigned Completed = 0, Pending = 0;
    for (const auto &Call : R.NativeCalls)
      if (Call.Name == "pthread_once") {
        Completed += Call.Result.has_value();
        Pending += !Call.Result.has_value();
      }
    EXPECT_EQ(Completed, Mode == 4 ? 0u : 1u);
    EXPECT_EQ(Pending, Mode == 4 ? 2u : 1u);
    if (Mode == 5)
      EXPECT_NE(R.Diagnostic.find("completion changed"), std::string::npos);
  }
}
TEST_P(AndroidThread, OnceWaitingDoesNotCompleteCallsOrResetTheBudget) {
  Options.Android->Memory.push_back({Buffer + 4096, 4096, {}, false});
  Options.Limits.Instructions = 2000;
  auto R = run("threads_once_failure", 7);
  EXPECT_EQ(R.Stop, ProcessStopReason::InstructionLimit) << R.Diagnostic;
  EXPECT_EQ(R.Instructions, 2000u);
  ASSERT_EQ(R.NativeThreads.size(), 2u);
  EXPECT_TRUE(R.NativeThreads[0].Waiting);
  EXPECT_FALSE(R.ReturnValue);
  unsigned Pending = 0;
  for (const auto &Call : R.NativeCalls)
    if (Call.Name == "pthread_once") {
      EXPECT_FALSE(Call.Result);
      ++Pending;
    }
  EXPECT_EQ(Pending, 2u);
}
TEST_P(AndroidThread, OnceWaitsAgreeWithNativeTransport) {
  ExecutionConfiguration C;
  C.Backend = ExecutionBackendKind::HVF;
  C.Architecture = GuestArchitecture::AArch64;
  C.Contract = ExecutionContract::CheckedUserAArch64;
  auto Probe = probeExecutionBackend(C);
  ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
  if (Probe->Availability != BackendAvailability::Available)
    GTEST_SKIP() << Probe->Reason;
  auto Software = run("threads_once_wait", 1);
  returned(Software, 0);
  Options.Backend = ExecutionBackendKind::HVF;
  auto Native = run("threads_once_wait", 1);
  returned(Native, 0);
  EXPECT_EQ(Native.SelectedBackend, ExecutionBackendKind::HVF);
  Native.SelectedBackend = Software.SelectedBackend;
  Native.BackendSelectionReason = Software.BackendSelectionReason;
  EXPECT_EQ(processResultJSON(Native), processResultJSON(Software));
}
TEST_P(AndroidThread, AvailableNativeTransportAgreesWithSoftwareContexts) {
  ExecutionConfiguration C;
  C.Backend = ExecutionBackendKind::HVF;
  C.Architecture = GuestArchitecture::AArch64;
  C.Contract = ExecutionContract::CheckedUserAArch64;
  auto Probe = probeExecutionBackend(C);
  ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
  if (Probe->Availability != BackendAvailability::Available)
    GTEST_SKIP() << Probe->Reason;
  auto Software = run("threads_identity");
  returned(Software, 0);
  Options.Backend = ExecutionBackendKind::HVF;
  auto Native = run("threads_identity");
  returned(Native, 0);
  EXPECT_EQ(Native.SelectedBackend, ExecutionBackendKind::HVF);
  Native.SelectedBackend = Software.SelectedBackend;
  Native.BackendSelectionReason = Software.BackendSelectionReason;
  EXPECT_EQ(processResultJSON(Native), processResultJSON(Software));
}
INSTANTIATE_TEST_SUITE_P(OptimizationAndPacking, AndroidThread,
                         testing::Values("threads-O0-none",
                                         "threads-O0-android",
                                         "threads-O0-relr", "threads-O2-none",
                                         "threads-O2-android",
                                         "threads-O2-relr"));
TEST(AndroidThreadOptions, RejectsWrongTypesAndInvalidSchedulingLimits) {
  for (const char *JSON : {R"({"android":{"thread_limit":true}})",
                           R"({"android":{"thread_limit":-1}})",
                           R"({"android":{"drain_threads":1}})"}) {
    auto O = processOptionsFromJSON(JSON);
    ASSERT_FALSE(bool(O));
    llvm::consumeError(O.takeError());
  }
  for (auto [Limit, Drain] :
       {std::pair<uint64_t, bool>{0, false}, {257, false}, {1, true}}) {
    ProcessOptions O;
    O.Android.emplace();
    O.Android->EntrySymbol = "entry";
    O.Android->ThreadLimit = Limit;
    O.Android->DrainThreads = Drain;
    auto R = emulateProcess("absent-thread-input.so",
                            ProcessProfile::AndroidNativeAArch64, O);
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find("thread_limit"),
              std::string::npos);
  }
}
} // namespace
} // namespace neverd::emulation
