//===- RunControlTests.cpp - Native cancellation lifetime regression -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "backends/RunDeadline.h"
#include "core/ExecutionDeadline.h"
#include "gtest/gtest.h"

#include <future>
#include <memory>

namespace neverd::emulation {
namespace {
#define NEVERD_RUN_CONTROL_TEST_VALUE(Name, Value)                             \
  constexpr unsigned Name = Value;
#include "RunControlCases.def"
#undef NEVERD_RUN_CONTROL_TEST_VALUE
using Clock = std::chrono::steady_clock;
constexpr auto Wait = std::chrono::milliseconds(WaitMilliseconds);
constexpr auto Quiet = std::chrono::milliseconds(QuietMilliseconds);

// The fake transport can hold a cancellation call in flight independently of
// the controller. This makes acknowledgement and resource retirement visible.
class InterruptTarget {
public:
  void interrupt() noexcept {
    std::unique_lock Lock(Mutex);
    ++Calls;
    Changed.notify_all();
    Changed.wait(Lock, [&] { return !Blocked; });
  }
  bool waitFor(unsigned Count, std::chrono::milliseconds Timeout = Wait) {
    std::unique_lock Lock(Mutex);
    return Changed.wait_for(Lock, Timeout, [&] { return Calls >= Count; });
  }
  void block() {
    std::lock_guard Lock(Mutex);
    Blocked = true;
  }
  void release() {
    std::lock_guard Lock(Mutex);
    Blocked = false;
    Changed.notify_all();
  }
  unsigned count() {
    std::lock_guard Lock(Mutex);
    return Calls;
  }

private:
  std::mutex Mutex;
  std::condition_variable Changed;
  unsigned Calls = 0;
  bool Blocked = false;
};
struct Interrupt {
  InterruptTarget &Target;
  void operator()() const noexcept { Target.interrupt(); }
};
using Controller = RunDeadline<Interrupt>;

TEST(RunControl, ExpiredDeadlineRetriesUntilAcknowledged) {
  InterruptTarget Target;
  Controller Control(Interrupt{Target});
  // Expiry before host entry must not lose the only cancellation request.
  Control.arm({Clock::now()});
  EXPECT_TRUE(Target.waitFor(RepeatedRequests));
  EXPECT_TRUE(Control.disarm());
  const unsigned Acknowledged = Target.count();
  EXPECT_FALSE(Target.waitFor(Acknowledged + 1, Quiet));
}

TEST(RunControl, StopInterruptsAnEntryWithADistantDeadline) {
  InterruptTarget Target;
  Controller Control(Interrupt{Target});
  std::atomic<bool> Stop{false};
  Control.arm({Clock::time_point::max(), &Stop});
  EXPECT_FALSE(Target.waitFor(1, Quiet));
  Stop = true;
  EXPECT_TRUE(Target.waitFor(1));
  EXPECT_TRUE(Control.disarm());
}

TEST(RunControl, DisarmAcknowledgesAnInFlightInterrupt) {
  InterruptTarget Target;
  Controller Control(Interrupt{Target});
  Target.block();
  Control.arm({Clock::now()});
  // Do not leave a blocked worker behind if the timeout check fails.
  EXPECT_TRUE(Target.waitFor(1));
  auto Acknowledge =
      std::async(std::launch::async, [&] { return Control.disarm(); });
  EXPECT_EQ(Acknowledge.wait_for(Quiet), std::future_status::timeout);
  Target.release();
  EXPECT_TRUE(Acknowledge.get());
  const unsigned Acknowledged = Target.count();
  EXPECT_FALSE(Target.waitFor(Acknowledged + 1, Quiet));
}

TEST(RunControl, OldStopTokenCannotInterruptTheNextGeneration) {
  InterruptTarget Target;
  Controller Control(Interrupt{Target});
  std::atomic<bool> OldStop{false}, NewStop{false};
  Control.arm({Clock::time_point::max(), &OldStop});
  EXPECT_FALSE(Control.disarm());
  Control.arm({Clock::time_point::max(), &NewStop});
  OldStop = true;
  EXPECT_FALSE(Target.waitFor(1, Quiet));
  NewStop = true;
  EXPECT_TRUE(Target.waitFor(1));
  EXPECT_TRUE(Control.disarm());
}

TEST(RunControl, AcknowledgedExpiryDoesNotCancelANewEntry) {
  InterruptTarget Target;
  Controller Control(Interrupt{Target});
  Control.arm({Clock::now()});
  EXPECT_TRUE(Target.waitFor(1));
  EXPECT_TRUE(Control.disarm());
  const unsigned Acknowledged = Target.count();
  Control.arm({Clock::time_point::max()});
  EXPECT_FALSE(Target.waitFor(Acknowledged + 1, Quiet));
  EXPECT_FALSE(Control.disarm());
}

TEST(RunControl, DisarmedDistantDeadlineDoesNotDelayEarlierRearm) {
  InterruptTarget Target;
  Controller Control(Interrupt{Target});
  Control.arm({Clock::time_point::max()});
  EXPECT_FALSE(Target.waitFor(1, Quiet));
  EXPECT_FALSE(Control.disarm());
  Control.arm({Clock::now()});
  EXPECT_TRUE(Target.waitFor(1));
  EXPECT_TRUE(Control.disarm());
  const unsigned Acknowledged = Target.count();
  EXPECT_FALSE(Target.waitFor(Acknowledged + 1, Quiet));
}

TEST(RunControl, DestructionAfterDisarmDoesNotWaitForTheOldDeadline) {
  InterruptTarget Target;
  auto Control = std::make_unique<Controller>(Interrupt{Target});
  Control->arm({Clock::time_point::max()});
  EXPECT_FALSE(Target.waitFor(1, Quiet));
  EXPECT_FALSE(Control->disarm());
  auto Retire = std::async(std::launch::async, [&] { Control.reset(); });
  EXPECT_EQ(Retire.wait_for(Wait), std::future_status::ready);
  Retire.get();
  EXPECT_EQ(Target.count(), 0u);
}

TEST(RunControl, DisarmedEntryDoesNotRetainAScopedStopToken) {
  InterruptTarget Target;
  Controller Control(Interrupt{Target});
  {
    std::atomic<bool> Stop{false};
    Control.arm({Clock::time_point::max(), &Stop});
    EXPECT_FALSE(Target.waitFor(1, Quiet));
    EXPECT_FALSE(Control.disarm());
    Stop = true;
  }
  // Remain idle beyond a polling interval after the token's lifetime ends.
  EXPECT_FALSE(Target.waitFor(1, Quiet));
}

TEST(RunControl, DestructionJoinsBeforeTheInterruptTargetCanRetire) {
  InterruptTarget Target;
  auto Control = std::make_unique<Controller>(Interrupt{Target});
  Target.block();
  Control->arm({Clock::now()});
  EXPECT_TRUE(Target.waitFor(1));
  auto Retire = std::async(std::launch::async, [&] { Control.reset(); });
  EXPECT_EQ(Retire.wait_for(Quiet), std::future_status::timeout);
  Target.release();
  Retire.get();
  const unsigned Retired = Target.count();
  EXPECT_FALSE(Target.waitFor(Retired + 1, Quiet));
}

TEST(RunControl, DestructionWakesADistantDeadlineWait) {
  InterruptTarget Target;
  auto Control = std::make_unique<Controller>(Interrupt{Target});
  Control->arm({Clock::time_point::max()});
  auto Retire = std::async(std::launch::async, [&] { Control.reset(); });
  EXPECT_EQ(Retire.wait_for(Wait), std::future_status::ready);
  Retire.get();
  EXPECT_EQ(Target.count(), 0u);
}

TEST(RunControl, NativeAllowancePreservesDeadlineAndStopIdentity) {
  std::atomic<bool> Stop{false};
  MachineRunControl Control{Clock::time_point::max(), &Stop};
  const auto Native = Control.forNativeStep();
  EXPECT_EQ(Native.Deadline, Control.Deadline);
  EXPECT_EQ(Native.Stop, &Stop);
  const auto Before = Clock::now();
  EXPECT_GE(MachineRunControl{Before}.forNativeStep().Deadline,
            Before + std::chrono::microseconds(
                         execution_limits::NativeStepGraceMicroseconds));
}

TEST(ExecutionDeadline, RejectsUnboundedAndOverflowingDurations) {
  constexpr uint64_t Invalid[] = {
#define NEVERD_CONFIGURATION_INVALID_TIMEOUT(Name, Value) Value,
#include "ExecutionConfigurationCases.def"
#undef NEVERD_CONFIGURATION_INVALID_TIMEOUT
  };
  for (uint64_t Budget : Invalid) {
    auto Deadline = makeExecutionDeadline(Budget, Clock::time_point{});
    ASSERT_FALSE(bool(Deadline));
    EXPECT_EQ(llvm::toString(Deadline.takeError()),
              diagnostic::ExecutionTimeout);
  }
}

TEST(ExecutionDeadline, ChecksAdditionBeforeTheLastRepresentableTick) {
  const auto Duration = std::chrono::duration_cast<Clock::duration>(
      std::chrono::microseconds(MinimumBudgetMicroseconds));
  const auto LastStart = Clock::time_point::max() - Duration;
  EXPECT_EQ(llvm::cantFail(
                makeExecutionDeadline(MinimumBudgetMicroseconds, LastStart)),
            Clock::time_point::max());
  auto Overflow = makeExecutionDeadline(MinimumBudgetMicroseconds,
                                        LastStart + Clock::duration(1));
  ASSERT_FALSE(bool(Overflow));
  EXPECT_EQ(llvm::toString(Overflow.takeError()), diagnostic::ExecutionTimeout);
}

TEST(ExecutionDeadline, NegativeClockEpochDoesNotOverflowRangeValidation) {
  const auto Duration = std::chrono::duration_cast<Clock::duration>(
      std::chrono::microseconds(MinimumBudgetMicroseconds));
  EXPECT_EQ(llvm::cantFail(makeExecutionDeadline(MinimumBudgetMicroseconds,
                                                 Clock::time_point::min())),
            Clock::time_point::min() + Duration);
}
} // namespace
} // namespace neverd::emulation
