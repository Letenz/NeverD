//===- WhpResourceTests.cpp - Native resource ownership and cancellation --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "backends/whp/WhpResourceCache.h"
#include "gtest/gtest.h"

#include <array>
#include <atomic>
#include <future>

namespace neverd::emulation {
namespace {
#define NEVERD_WHP_RESOURCE_TEXT(Name, Text) constexpr char Name[] = Text;
#define NEVERD_WHP_RESOURCE_VALUE(Name, Value) constexpr unsigned Name = Value;
#include "WhpResourceCases.def"
#undef NEVERD_WHP_RESOURCE_VALUE
#undef NEVERD_WHP_RESOURCE_TEXT
using Clock = std::chrono::steady_clock;
enum Owner { First, Second, OwnerCount };
struct ResourceStats {
  unsigned Live = 0, Created = 0, Destroyed = 0;
  std::array<unsigned, OwnerCount> Attempts{};
  std::array<bool, OwnerCount> Fail{};
  std::atomic<bool> *StopAtCreate = nullptr;
};
struct FakeResource {
  ResourceStats &Stats;
  Owner Identity;
  FakeResource(ResourceStats &Stats, Owner Identity)
      : Stats(Stats), Identity(Identity) {
    ++Stats.Live;
    ++Stats.Created;
  }
  ~FakeResource() {
    --Stats.Live;
    ++Stats.Destroyed;
  }
};
class WhpResources : public testing::Test {
protected:
  using Binding = WhpResourceBinding<FakeResource>;
  using Lease = WhpResourceCache<FakeResource>::Lease;
  ResourceStats Stats;
  std::unique_ptr<Binding> binding(Owner Identity) {
    return std::make_unique<Binding>(
        [&, Identity]() -> llvm::Expected<std::unique_ptr<FakeResource>> {
          EXPECT_EQ(Stats.Live, 0u);
          ++Stats.Attempts[Identity];
          if (Stats.StopAtCreate)
            *Stats.StopAtCreate = true;
          if (Stats.Fail[Identity])
            return diagnostic::error(CreateFailed);
          return std::make_unique<FakeResource>(Stats, Identity);
        });
  }
  void interrupted(llvm::Error E, bool Stopped, bool Expired,
                   llvm::StringRef Text = {}) {
    bool Seen = false;
    auto Remaining = llvm::handleErrors(
        std::move(E), [&](const MachineInterruptedError &Interrupted) {
          Seen = true;
          EXPECT_EQ(Interrupted.stopRequested(), Stopped);
          EXPECT_EQ(Interrupted.deadlineReached(), Expired);
          if (!Text.empty()) {
            std::string Detail;
            llvm::raw_string_ostream OS(Detail);
            Interrupted.log(OS);
            EXPECT_EQ(OS.str(), Text);
          }
        });
    EXPECT_EQ(llvm::toString(std::move(Remaining)), "");
    EXPECT_TRUE(Seen);
  }
  void touch(Binding &Value, Owner Expected) {
    auto Active = Value.acquire({Clock::time_point::max()});
    ASSERT_TRUE(bool(Active)) << llvm::toString(Active.takeError());
    EXPECT_EQ((**Active).Identity, Expected);
  }
};
TEST_F(WhpResources, ReusesHotOwnerAndRetiresBeforeEveryOwnerSwitch) {
  auto A = binding(First), B = binding(Second);
  ASSERT_NO_FATAL_FAILURE(touch(*A, First));
  ASSERT_NO_FATAL_FAILURE(touch(*A, First));
  EXPECT_EQ(Stats.Attempts[First], 1u);
  ASSERT_NO_FATAL_FAILURE(touch(*B, Second));
  EXPECT_EQ(Stats.Destroyed, 1u);
  ASSERT_NO_FATAL_FAILURE(touch(*A, First));
  EXPECT_EQ(Stats.Attempts[First], 2u);
  EXPECT_EQ(Stats.Created, 3u);
  EXPECT_EQ(Stats.Destroyed, 2u);
  EXPECT_EQ(Stats.Live, 1u);
}
TEST_F(WhpResources, RetiringInactiveOwnerCannotDestroyTheCurrentResource) {
  auto A = binding(First), B = binding(Second);
  ASSERT_NO_FATAL_FAILURE(touch(*A, First));
  ASSERT_NO_FATAL_FAILURE(touch(*B, Second));
  A.reset();
  ASSERT_NO_FATAL_FAILURE(touch(*B, Second));
  EXPECT_EQ(Stats.Attempts[Second], 1u);
  EXPECT_EQ(Stats.Live, 1u);
  B.reset();
  EXPECT_EQ(Stats.Live, 0u);
  EXPECT_EQ(Stats.Created, Stats.Destroyed);
}
TEST_F(WhpResources, FailedReplacementLeavesOtherLogicalOwnersUsable) {
  auto A = binding(First), B = binding(Second);
  ASSERT_NO_FATAL_FAILURE(touch(*A, First));
  Stats.Fail[Second] = true;
  auto Failed = B->acquire({Clock::time_point::max()});
  ASSERT_FALSE(bool(Failed));
  EXPECT_EQ(llvm::toString(Failed.takeError()), CreateFailed);
  EXPECT_EQ(Stats.Live, 0u);
  ASSERT_NO_FATAL_FAILURE(touch(*A, First));
  EXPECT_EQ(Stats.Attempts[First], 2u);
  B.reset();
  EXPECT_EQ(Stats.Live, 1u);
}
TEST_F(WhpResources, LateStopDiscardsTheNewResourceBeforePublication) {
  auto A = binding(First), B = binding(Second);
  ASSERT_NO_FATAL_FAILURE(touch(*A, First));
  std::atomic<bool> Stop{false};
  Stats.StopAtCreate = &Stop;
  auto Cancelled = B->acquire({Clock::time_point::max(), &Stop});
  ASSERT_FALSE(bool(Cancelled));
  interrupted(Cancelled.takeError(), true, false);
  EXPECT_EQ(Stats.Live, 0u);
  EXPECT_EQ(Stats.Created, Stats.Destroyed);
  Stats.StopAtCreate = nullptr;
  Stop = false;
  ASSERT_NO_FATAL_FAILURE(touch(*A, First));
  EXPECT_EQ(Stats.Attempts[First], 2u);
}
TEST_F(WhpResources, FailedConstructionOutranksASimultaneousStop) {
  auto A = binding(First), B = binding(Second);
  ASSERT_NO_FATAL_FAILURE(touch(*A, First));
  std::atomic<bool> Stop{false};
  Stats.StopAtCreate = &Stop;
  Stats.Fail[Second] = true;
  auto Failed = B->acquire({Clock::time_point::max(), &Stop});
  ASSERT_FALSE(bool(Failed));
  auto E = Failed.takeError();
  EXPECT_FALSE(E.isA<MachineInterruptedError>());
  EXPECT_EQ(llvm::toString(std::move(E)), CreateFailed);
  EXPECT_EQ(Stats.Live, 0u);
}
TEST_F(WhpResources, RejectedEntryCannotRetireTheHotResource) {
  auto A = binding(First), B = binding(Second);
  ASSERT_NO_FATAL_FAILURE(touch(*A, First));
  std::atomic<bool> Stop{true};
  auto Stopped = B->acquire({Clock::time_point::max(), &Stop});
  ASSERT_FALSE(bool(Stopped));
  interrupted(Stopped.takeError(), true, false);
  auto Expired = B->acquire({Clock::time_point::min()});
  ASSERT_FALSE(bool(Expired));
  interrupted(Expired.takeError(), false, true);
  EXPECT_EQ(Stats.Attempts[Second], 0u);
  ASSERT_NO_FATAL_FAILURE(touch(*A, First));
  EXPECT_EQ(Stats.Attempts[First], 1u);
}
TEST_F(WhpResources, InitializationRetainsPhaseCauseAndOrdinaryDeadlines) {
  auto A = binding(First), B = binding(Second);
  EXPECT_EQ(llvm::toString(A->initialize()), "");
  std::atomic<bool> Stop{true};
  interrupted(B->initialize({Clock::time_point::max(), &Stop}), true, false,
              InitializationStopped);
  interrupted(B->initialize({Clock::time_point::min()}), false, true,
              InitializationExpired);
  EXPECT_EQ(Stats.Attempts[Second], 0u);
  EXPECT_EQ(Stats.Live, 1u);
  auto Expired = A->acquire({Clock::time_point::min()});
  ASSERT_FALSE(bool(Expired));
  interrupted(Expired.takeError(), false, true, diagnostic::WhpRun);
  ASSERT_NO_FATAL_FAILURE(touch(*A, First));
  EXPECT_EQ(Stats.Attempts[First], 1u);
}
TEST_F(WhpResources, CancelledInitializationDiscardsTheCreatedResource) {
  auto A = binding(First);
  std::atomic<bool> Stop{false};
  Stats.StopAtCreate = &Stop;
  interrupted(A->initialize({Clock::time_point::max(), &Stop}), true, false,
              InitializationStopped);
  EXPECT_EQ(Stats.Created, 1u);
  EXPECT_EQ(Stats.Created, Stats.Destroyed);
  EXPECT_EQ(Stats.Live, 0u);
  Stats.StopAtCreate = nullptr;
  Stop = false;
  EXPECT_EQ(llvm::toString(A->initialize()), "");
  EXPECT_EQ(Stats.Attempts[First], 2u);
  EXPECT_EQ(Stats.Live, 1u);
}
TEST_F(WhpResources, InitializationHostFailureOutranksConcurrentStop) {
  auto A = binding(First);
  std::atomic<bool> Stop{false};
  Stats.StopAtCreate = &Stop;
  Stats.Fail[First] = true;
  auto E = A->initialize({Clock::time_point::max(), &Stop});
  EXPECT_FALSE(E.isA<MachineInterruptedError>());
  EXPECT_EQ(llvm::toString(std::move(E)), CreateFailed);
  EXPECT_EQ(Stats.Attempts[First], 1u);
  EXPECT_EQ(Stats.Created, 0u);
}
TEST_F(WhpResources, WaitingForAnotherOwnerHonorsTheOriginalDeadline) {
  auto A = binding(First), B = binding(Second);
  auto Active = std::make_unique<Lease>(
      llvm::cantFail(A->acquire({Clock::time_point::max()})));
  auto Waiting = std::async(std::launch::async, [&]() -> llvm::Error {
    auto Result = B->acquire(
        {Clock::now() + std::chrono::milliseconds(DeadlineMilliseconds)});
    return Result ? llvm::Error::success() : Result.takeError();
  });
  const auto Ready =
      Waiting.wait_for(std::chrono::milliseconds(WaitMilliseconds));
  if (Ready != std::future_status::ready)
    Active.reset();
  EXPECT_EQ(Ready, std::future_status::ready);
  interrupted(Waiting.get(), false, true);
  EXPECT_EQ(Stats.Attempts[Second], 0u);
}
TEST_F(WhpResources, WaitingForAnotherOwnerObservesExternalStop) {
  auto A = binding(First), B = binding(Second);
  auto Active = std::make_unique<Lease>(
      llvm::cantFail(A->acquire({Clock::time_point::max()})));
  std::atomic<bool> Stop{false};
  std::promise<void> Entered;
  auto Started = Entered.get_future();
  auto Waiting = std::async(std::launch::async, [&]() -> llvm::Error {
    Entered.set_value();
    auto Result = B->acquire({Clock::time_point::max(), &Stop});
    return Result ? llvm::Error::success() : Result.takeError();
  });
  Started.wait();
  Stop = true;
  const auto Ready =
      Waiting.wait_for(std::chrono::milliseconds(WaitMilliseconds));
  if (Ready != std::future_status::ready)
    Active.reset();
  EXPECT_EQ(Ready, std::future_status::ready);
  interrupted(Waiting.get(), true, false);
  EXPECT_EQ(Stats.Attempts[Second], 0u);
}
TEST_F(WhpResources, IndependentBindingsRetainSimultaneousResourceLeases) {
  // Hold the first lease while another host thread acquires the second.
  // The shared-cache path is separately required to time out in this order.
  auto Create = [&]() -> llvm::Expected<std::unique_ptr<FakeResource>> {
    return std::make_unique<FakeResource>(Stats, First);
  };
  Binding A(Create, true), B(Create, true);
  auto FirstLease = llvm::cantFail(A.acquire({Clock::time_point::max()}));
  std::promise<void> Acquired, Release;
  auto Ready = Acquired.get_future();
  auto Finish = Release.get_future();
  auto Other = std::async(std::launch::async, [&]() -> llvm::Error {
    auto SecondLease =
        B.acquire({Clock::now() + std::chrono::milliseconds(WaitMilliseconds)});
    Acquired.set_value();
    if (!SecondLease)
      return SecondLease.takeError();
    Finish.wait();
    return llvm::Error::success();
  });
  const auto Status =
      Ready.wait_for(std::chrono::milliseconds(WaitMilliseconds));
  if (Status == std::future_status::ready)
    EXPECT_EQ(Stats.Live, unsigned(OwnerCount));
  Release.set_value();
  EXPECT_EQ(Status, std::future_status::ready);
  EXPECT_EQ(llvm::toString(Other.get()), "");
}
} // namespace
} // namespace neverd::emulation
