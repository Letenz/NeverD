//===- NativeEntryTests.cpp - Controlled host entry tests ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "backends/RunDeadline.h"
#include "gtest/gtest.h"

#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>

namespace neverd::emulation {
namespace {
#define NEVERD_RUN_CONTROL_TEST_VALUE(Name, Value)                             \
  constexpr unsigned Name = Value;
#include "RunControlCases.def"
#undef NEVERD_RUN_CONTROL_TEST_VALUE
using Clock = std::chrono::steady_clock;
constexpr auto Wait = std::chrono::milliseconds(WaitMilliseconds);
constexpr auto Quiet = std::chrono::milliseconds(QuietMilliseconds);

TEST(NativeEntry, PreStoppedEntryDoesNotCallHostAndAllowsRetry) {
  std::atomic<unsigned> Cancels{0};
  std::atomic<bool> Stop{true};
  RunDeadline Control([&]() noexcept { ++Cancels; });
  unsigned Entries = 0;
  const auto Call = [&]() noexcept {
    ++Entries;
    return HostSuccess;
  };
  auto Rejected = Control.invoke({Clock::time_point::max(), &Stop}, Call);
  EXPECT_FALSE(Rejected.Value.has_value());
  EXPECT_TRUE(Rejected.Cancelled);
  EXPECT_EQ(Entries, 0u);
  EXPECT_EQ(Cancels.load(), 0u);
  Stop = false;
  auto Retried = Control.invoke({Clock::time_point::max(), &Stop}, Call);
  ASSERT_TRUE(Retried.Value.has_value());
  EXPECT_EQ(*Retried.Value, HostSuccess);
  EXPECT_FALSE(Retried.Cancelled);
  EXPECT_EQ(Entries, 1u);
  EXPECT_EQ(Cancels.load(), 0u);
}
TEST(NativeEntry, ExpiredEntryDoesNotCallHost) {
  std::atomic<unsigned> Cancels{0};
  RunDeadline Control([&]() noexcept { ++Cancels; });
  unsigned Entries = 0;
  auto Rejected = Control.invoke({Clock::time_point::min()}, [&]() noexcept {
    ++Entries;
    return HostSuccess;
  });
  EXPECT_FALSE(Rejected.Value.has_value());
  EXPECT_TRUE(Rejected.Cancelled);
  EXPECT_EQ(Entries, 0u);
  EXPECT_EQ(Cancels.load(), 0u);
}
TEST(NativeEntry, StopAtHostReturnCannotPublishAnUncancelledResult) {
  std::atomic<unsigned> Cancels{0};
  std::atomic<bool> Stop{false};
  RunDeadline Control([&]() noexcept { ++Cancels; });
  auto Result =
      Control.invoke({Clock::time_point::max(), &Stop}, [&]() noexcept {
        Stop = true;
        return HostSuccess;
      });
  ASSERT_TRUE(Result.Value.has_value());
  EXPECT_EQ(*Result.Value, HostSuccess);
  EXPECT_TRUE(Result.Cancelled);
}
TEST(NativeEntry, ActualHostFailureSurvivesASimultaneousStop) {
  std::atomic<unsigned> Cancels{0};
  std::atomic<bool> Stop{false};
  RunDeadline Control([&]() noexcept { ++Cancels; });
  auto Result =
      Control.invoke({Clock::time_point::max(), &Stop}, [&]() noexcept {
        Stop = true;
        return HostFailure;
      });
  ASSERT_TRUE(Result.Value.has_value());
  EXPECT_EQ(*Result.Value, HostFailure);
  EXPECT_TRUE(Result.Cancelled);
}
TEST(NativeEntry, CompletedEntryReleasesItsStopTokenBeforeReuse) {
  std::atomic<unsigned> Cancels{0};
  RunDeadline Control([&]() noexcept { ++Cancels; });
  std::atomic<bool> NextStop{false};
  {
    std::atomic<bool> PreviousStop{false};
    auto First = Control.invoke({Clock::time_point::max(), &PreviousStop},
                                []() noexcept { return HostSuccess; });
    ASSERT_TRUE(First.Value.has_value());
    EXPECT_FALSE(First.Cancelled);
    PreviousStop = true;
  }
  auto Next = Control.invoke({Clock::time_point::max(), &NextStop},
                             []() noexcept { return HostSuccess; });
  ASSERT_TRUE(Next.Value.has_value());
  EXPECT_FALSE(Next.Cancelled);
  EXPECT_EQ(Cancels.load(), 0u);
}
TEST(NativeEntry, InvocationWaitsForInFlightCancellationAcknowledgement) {
  std::atomic<bool> Stop{false};
  std::mutex Mutex;
  std::condition_variable Changed;
  bool Entered = false, Blocked = true;
  RunDeadline Control([&]() noexcept {
    std::unique_lock Lock(Mutex);
    Entered = true;
    Changed.notify_all();
    Changed.wait(Lock, [&] { return !Blocked; });
  });
  auto Result = std::async(std::launch::async, [&] {
    return Control.invoke({Clock::time_point::max(), &Stop}, [&]() noexcept {
      Stop = true;
      std::unique_lock Lock(Mutex);
      Changed.wait_for(Lock, Wait, [&] { return Entered; });
      return HostSuccess;
    });
  });
  {
    std::unique_lock Lock(Mutex);
    EXPECT_TRUE(Changed.wait_for(Lock, Wait, [&] { return Entered; }));
  }
  EXPECT_EQ(Result.wait_for(Quiet), std::future_status::timeout);
  {
    std::lock_guard Lock(Mutex);
    Blocked = false;
    Changed.notify_all();
  }
  const auto Acknowledged = Result.get();
  ASSERT_TRUE(Acknowledged.Value.has_value());
  EXPECT_EQ(*Acknowledged.Value, HostSuccess);
  EXPECT_TRUE(Acknowledged.Cancelled);
}
} // namespace
} // namespace neverd::emulation
