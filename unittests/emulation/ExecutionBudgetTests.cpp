//===- ExecutionBudgetTests.cpp - Shared continuation accounting ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionBudget.h"

#include <type_traits>

namespace neverd::emulation {
namespace {
#define NEVERD_BUDGET_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "ExecutionBudgetCases.def"
#undef NEVERD_BUDGET_VALUE
using Clock = ExecutionBudget::Clock;
using Microseconds = std::chrono::microseconds;
constexpr Clock::time_point Epoch{};
constexpr ExecutionLimits Limits{InstructionLimit, EventLimit, Timeout};

TEST(ExecutionBudget, ContinuationsShareCreditsWithoutResetOrOverflow) {
  static_assert(!std::is_copy_constructible_v<ExecutionBudget>);
  static_assert(!std::is_move_constructible_v<ExecutionBudget>);
  auto Budget = llvm::cantFail(ExecutionBudget::create(Limits, Epoch));
  auto &First = *Budget;
  auto &Second = *Budget;
  EXPECT_TRUE(First.consumeInstructions());
  EXPECT_TRUE(Second.consumeInstructions(InstructionLimit - 1));
  EXPECT_FALSE(First.consumeInstructions());
  EXPECT_FALSE(Second.consumeInstructions(UINT64_MAX));
  EXPECT_EQ(Budget->instructions(), InstructionLimit);
  EXPECT_TRUE(First.consumeEvents());
  EXPECT_TRUE(Second.consumeEvents());
  EXPECT_FALSE(First.consumeEvents());
  EXPECT_FALSE(Second.consumeEvents(UINT64_MAX));
  EXPECT_EQ(Budget->events(), EventLimit);
}

TEST(ExecutionBudget, FailedReservationsDoNotSpendCreditOrWrapAtMaximum) {
  auto Budget = llvm::cantFail(
      ExecutionBudget::create({UINT64_MAX, UINT64_MAX, Timeout}, Epoch));
  EXPECT_TRUE(Budget->consumeInstructions(UINT64_MAX - 1));
  EXPECT_FALSE(Budget->consumeInstructions(UINT64_MAX));
  EXPECT_TRUE(Budget->consumeInstructions());
  EXPECT_FALSE(Budget->consumeInstructions());
  EXPECT_EQ(Budget->instructions(), UINT64_MAX);
  EXPECT_TRUE(Budget->consumeEvents(UINT64_MAX - 1));
  EXPECT_FALSE(Budget->consumeEvents(UINT64_MAX));
  EXPECT_TRUE(Budget->consumeEvents());
  EXPECT_FALSE(Budget->consumeEvents());
  EXPECT_EQ(Budget->events(), UINT64_MAX);
}

TEST(ExecutionBudget,
     OneAbsoluteDeadlineSurvivesResumptionsAndOldClockSamples) {
  auto Budget = llvm::cantFail(ExecutionBudget::create(Limits, Epoch));
  EXPECT_EQ(Budget->remainingMicroseconds(Epoch), Timeout);
  EXPECT_EQ(Budget->remainingMicroseconds(Clock::time_point::min()), Timeout);
  EXPECT_EQ(Budget->remainingMicroseconds(Epoch + Microseconds(Timeout - 1)),
            1u);
  EXPECT_EQ(Budget->remainingMicroseconds(Epoch + Microseconds(Timeout)), 0u);
  EXPECT_EQ(Budget->remainingMicroseconds(Clock::time_point::max()), 0u);
  // Consuming counters changes neither the start nor the absolute deadline.
  EXPECT_TRUE(Budget->consumeInstructions());
  EXPECT_EQ(Budget->remainingMicroseconds(Epoch + Microseconds(Timeout - 1)),
            1u);
  const auto NearEnd = Clock::time_point::max() - Microseconds(Timeout);
  auto Last = llvm::cantFail(ExecutionBudget::create(Limits, NearEnd));
  EXPECT_EQ(Last->remainingMicroseconds(NearEnd), Timeout);
  EXPECT_EQ(Last->remainingMicroseconds(Clock::time_point::max()), 0u);
}

TEST(ExecutionBudget, ZeroUnboundedAndUnrepresentableRequestsAreRejected) {
  constexpr ExecutionLimits Invalid[] = {
      {0, EventLimit, Timeout},
      {InstructionLimit, 0, Timeout},
      {InstructionLimit, EventLimit, 0},
      {InstructionLimit, EventLimit, UINT64_MAX}};
  for (auto Limits : Invalid) {
    auto Budget = ExecutionBudget::create(Limits, Epoch);
    EXPECT_FALSE(bool(Budget));
    llvm::consumeError(Budget.takeError());
  }
  auto Overflow = ExecutionBudget::create(Limits, Clock::time_point::max());
  EXPECT_FALSE(bool(Overflow));
  llvm::consumeError(Overflow.takeError());
}
} // namespace
} // namespace neverd::emulation
