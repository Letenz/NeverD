//===- HvfIntelDeadlineTests.cpp - Finite-entry boundary contracts ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "backends/hvf/HvfIntelDeadline.h"
#include "core/ExecutionDiagnostics.h"
#include "gtest/gtest.h"

namespace neverd::emulation {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

TEST(HvfIntelDeadline, TimebaseRoundingTotalBudgetAndSentinel) {
  const auto Now = Clock::time_point{};
  EXPECT_EQ(hvf::intelSliceDeadline(100, Now, Now, 1, 1), 100u);
  EXPECT_EQ(hvf::intelSliceDeadline(100, Now, Now - 1ns, 1, 1), 100u);
  EXPECT_EQ(hvf::intelSliceDeadline(100, Now, Now + 7ns, 3, 2), 104u);
  EXPECT_EQ(hvf::intelSliceDeadline(100, Now, Now + 1ns, 3, 2), 100u);
  EXPECT_EQ(hvf::intelSliceDeadline(100, Now, Now + 9ns, 1, 3), 127u);
  EXPECT_EQ(hvf::intelSliceDeadline(100, Now, Clock::time_point::max(), 1, 1),
            1000100u);
  EXPECT_EQ(hvf::intelSliceDeadline(100, Clock::time_point::min(),
                                    Clock::time_point::max(), 1, 1),
            1000100u);
  EXPECT_EQ(hvf::intelSliceDeadline(100, Clock::time_point::max() - 7ns,
                                    Clock::time_point::max(), 1, 1),
            107u);
  EXPECT_EQ(hvf::intelSliceDeadline(UINT64_MAX - 3, Now, Now + 9ns, 1, 3),
            UINT64_MAX - 1);
  EXPECT_EQ(hvf::intelSliceDeadline(UINT64_MAX, Now, Now, 1, 1),
            UINT64_MAX - 1);
  EXPECT_EQ(
      hvf::intelSliceDeadline(0, Now, Clock::time_point::max(), 1, UINT32_MAX),
      1000000ull * UINT32_MAX);
}

TEST(HvfIntelDeadline, TimerAndIRQContinueUntilOneGuestCompletion) {
  const uint64_t Reasons[] = {52, 1, 37};
  unsigned Calls = 0, Completions = 0, GuestEffects = 0;
  const MachineRunControl Control{Clock::time_point::max()};
  auto Entry = hvf::runIntelEntry(
      Control, [] { return 123u; },
      [&](uint64_t Deadline) -> llvm::Expected<uint64_t> {
        EXPECT_EQ(Deadline, 123u);
        if (Calls >= std::size(Reasons))
          return diagnostic::error("unexpected extra native entry");
        const auto Reason = Reasons[Calls++];
        if (Reason == 37)
          ++GuestEffects;
        return Reason;
      },
      [&](bool Cancelled) {
        ++Completions;
        EXPECT_FALSE(Cancelled);
        EXPECT_EQ(GuestEffects, 1u);
        return llvm::Error::success();
      });
  EXPECT_EQ(llvm::toString(std::move(Entry.Result)), "");
  EXPECT_FALSE(Entry.Cancelled);
  EXPECT_EQ(Calls, 3u);
  EXPECT_EQ(Completions, 1u);
  EXPECT_EQ(Control.Deadline, Clock::time_point::max());
}

TEST(HvfIntelDeadline, InitialCancellationNeverReadsOrCompletesStaleState) {
  for (bool AlreadyStopped : {true, false}) {
    std::atomic<bool> Stop{AlreadyStopped};
    auto Entry = hvf::runIntelEntry(
        {Clock::time_point::max(), &Stop},
        [&] {
          Stop = true; // cancellation during deadline calculation
          return 1u;
        },
        [&](uint64_t) -> llvm::Expected<uint64_t> {
          ADD_FAILURE() << "cancelled admission entered native execution";
          return 37u;
        },
        [&](bool) {
          ADD_FAILURE() << "cancelled admission inspected stale native state";
          return llvm::Error::success();
        });
    EXPECT_EQ(llvm::toString(std::move(Entry.Result)), "");
    EXPECT_TRUE(Entry.Cancelled);
  }
}

TEST(HvfIntelDeadline, CancellationAcknowledgesLastActualTransportReturn) {
  for (uint64_t Reason : {1, 52}) {
    for (bool StopAfterReturn : {true, false}) {
      SCOPED_TRACE(Reason);
      SCOPED_TRACE(StopAfterReturn);
      std::atomic<bool> Stop{false};
      unsigned Calls = 0, Completions = 0;
      auto Entry = hvf::runIntelEntry(
          {Clock::time_point::max(), &Stop},
          [&] {
            if (Calls)
              Stop = true; // after the last return, before the next admission
            return 1u;
          },
          [&](uint64_t) -> llvm::Expected<uint64_t> {
            ++Calls;
            if (StopAfterReturn)
              Stop = true;
            return Reason;
          },
          [&](bool Cancelled) {
            ++Completions;
            EXPECT_TRUE(Cancelled);
            return llvm::Error::success();
          });
      EXPECT_EQ(llvm::toString(std::move(Entry.Result)), "");
      EXPECT_TRUE(Entry.Cancelled);
      EXPECT_EQ(Calls, 1u);
      EXPECT_EQ(Completions, 1u);
    }
  }
}

TEST(HvfIntelDeadline, GuestFaultAndEntryFailureStillCompleteWhenStopRaces) {
  for (uint64_t Reason : {0ull, (1ull << 31) | 52, (1ull << 31) | 1}) {
    std::atomic<bool> Stop{false};
    unsigned Calls = 0, Completions = 0;
    auto Entry = hvf::runIntelEntry(
        {Clock::time_point::max(), &Stop}, [] { return 1u; },
        [&](uint64_t) -> llvm::Expected<uint64_t> {
          ++Calls;
          Stop = true;
          return Reason;
        },
        [&](bool Cancelled) {
          ++Completions;
          EXPECT_TRUE(Cancelled);
          return diagnostic::error("authenticated guest/entry failure");
        });
    EXPECT_EQ(llvm::toString(std::move(Entry.Result)),
              "authenticated guest/entry failure");
    EXPECT_TRUE(Entry.Cancelled);
    EXPECT_EQ(Calls, 1u);
    EXPECT_EQ(Completions, 1u);
  }
}

TEST(HvfIntelDeadline, HostAndReadbackErrorsPreserveRacingCancellation) {
  for (const char *Failure :
       {"host entry failure", "exit reason read failure"}) {
    std::atomic<bool> Stop{false};
    auto Entry = hvf::runIntelEntry(
        {Clock::time_point::max(), &Stop}, [] { return 1u; },
        [&](uint64_t) -> llvm::Expected<uint64_t> {
          Stop = true;
          return diagnostic::error(Failure);
        },
        [&](bool) {
          ADD_FAILURE() << "native/readback failure reached completion";
          return llvm::Error::success();
        });
    EXPECT_EQ(llvm::toString(std::move(Entry.Result)), Failure);
    EXPECT_TRUE(Entry.Cancelled);
  }
}

TEST(HvfIntelDeadline, CaptureFailurePreservesStopRequestedDuringCompletion) {
  std::atomic<bool> Stop{false};
  auto Entry = hvf::runIntelEntry(
      {Clock::time_point::max(), &Stop}, [] { return 1u; },
      [](uint64_t) -> llvm::Expected<uint64_t> { return 37u; },
      [&](bool Cancelled) {
        EXPECT_FALSE(Cancelled);
        Stop = true;
        return diagnostic::error("complete-state capture failure");
      });
  EXPECT_EQ(llvm::toString(std::move(Entry.Result)),
            "complete-state capture failure");
  EXPECT_TRUE(Entry.Cancelled);
}
} // namespace
} // namespace neverd::emulation
