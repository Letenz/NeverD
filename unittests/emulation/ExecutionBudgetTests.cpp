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
TEST(ExecutionBudget, ActiveWorkExcludesWaitingAndPreservesAllCredits) {
  auto Result = ExecutionBudget::createWithTimePolicy(
      Limits, ExecutionTimePolicy::ActiveWork, Epoch);
  ASSERT_TRUE(bool(Result));
  auto &B = **Result;
  EXPECT_EQ(B.timePolicy(), ExecutionTimePolicy::ActiveWork);
  EXPECT_EQ(B.remainingMicroseconds(Epoch + Microseconds(10000)), Timeout);
  auto Work = B.enterWork(Epoch + Microseconds(10000));
  ASSERT_TRUE(bool(Work));
  EXPECT_TRUE(B.consumeInstructions());
  EXPECT_TRUE(B.consumeEvents());
  EXPECT_EQ(B.remainingMicroseconds(Epoch + Microseconds(10400)), 600u);
  ASSERT_EQ(llvm::toString((*Work)->close(Epoch + Microseconds(10400))), "");
  EXPECT_EQ(B.remainingMicroseconds(Clock::time_point::max()), 600u);
  auto Next = B.enterWork(Epoch + Microseconds(100000));
  ASSERT_TRUE(bool(Next));
  EXPECT_EQ(B.remainingMicroseconds(Epoch + Microseconds(100500)), 100u);
  ASSERT_EQ(llvm::toString((*Next)->close(Epoch + Microseconds(100500))), "");
  EXPECT_EQ(B.instructions(), 1u);
  EXPECT_EQ(B.events(), 1u);
  EXPECT_EQ(B.remainingMicroseconds(Clock::time_point::max()), 100u);
}
TEST(ExecutionBudget, NestedActiveScopesChargeTheirUnionExactlyOnce) {
  auto B = llvm::cantFail(ExecutionBudget::createWithTimePolicy(
      Limits, ExecutionTimePolicy::ActiveWork, Epoch));
  auto Outer = B->enterWork(Epoch);
  ASSERT_TRUE(bool(Outer));
  auto Inner = B->enterWork(Epoch + Microseconds(100));
  ASSERT_TRUE(bool(Inner));
  ASSERT_EQ(llvm::toString((*Inner)->close(Epoch + Microseconds(200))), "");
  EXPECT_EQ(B->remainingMicroseconds(Epoch + Microseconds(300)), 700u);
  ASSERT_EQ(llvm::toString((*Outer)->close(Epoch + Microseconds(400))), "");
  EXPECT_EQ(B->remainingMicroseconds(Epoch + Microseconds(10000)), 600u);
  EXPECT_NE(llvm::toString((*Outer)->close(Epoch + Microseconds(10000))), "");
  EXPECT_EQ(B->remainingMicroseconds(Epoch + Microseconds(20000)), 600u);
}
TEST(ExecutionBudget, ActiveClockSamplesCannotReplenishAnObservedInterval) {
  auto B = llvm::cantFail(ExecutionBudget::createWithTimePolicy(
      Limits, ExecutionTimePolicy::ActiveWork, Epoch));
  auto Work = B->enterWork(Epoch);
  ASSERT_TRUE(bool(Work));
  EXPECT_EQ(B->remainingMicroseconds(Epoch + Microseconds(700)), 300u);
  EXPECT_EQ(B->remainingMicroseconds(Epoch + Microseconds(10)), 300u);
  ASSERT_EQ(llvm::toString((*Work)->close(Epoch + Microseconds(20))), "");
  EXPECT_EQ(B->remainingMicroseconds(Epoch + Microseconds(10000)), 300u);
  auto Next = B->enterWork(Epoch + Microseconds(900));
  ASSERT_TRUE(bool(Next));
  EXPECT_EQ(B->remainingMicroseconds(Clock::time_point::max()), 0u);
  EXPECT_EQ(B->remainingMicroseconds(Epoch + Microseconds(950)), 0u);
  ASSERT_EQ(llvm::toString((*Next)->close(Epoch + Microseconds(950))), "");
  EXPECT_EQ(B->remainingMicroseconds(Epoch + Microseconds(20000)), 0u);
}
TEST(ExecutionBudget, FractionalActiveTimeSurvivesPauseAndResume) {
  auto B = llvm::cantFail(ExecutionBudget::createWithTimePolicy(
      {100, 100, 2}, ExecutionTimePolicy::ActiveWork, Epoch));
  const auto OneMicrosecond =
      std::chrono::duration_cast<Clock::duration>(Microseconds(1));
  const auto Slice = Clock::duration(OneMicrosecond.count() / 2 + 1);
  auto First = B->enterWork(Epoch);
  ASSERT_TRUE(bool(First));
  ASSERT_EQ(llvm::toString((*First)->close(Epoch + Slice)), "");
  EXPECT_EQ(B->remainingMicroseconds(Epoch + Microseconds(100)), 1u);
  auto Second = B->enterWork(Epoch + Microseconds(100));
  ASSERT_TRUE(bool(Second));
  ASSERT_EQ(llvm::toString((*Second)->close(Epoch + Microseconds(100) + Slice)),
            "");
  EXPECT_EQ(B->remainingMicroseconds(Epoch + Microseconds(1000)), 0u);
}
TEST(ExecutionBudget, AbsoluteDeadlineKeepsItsOriginalPolicyInsideWorkScopes) {
  auto B = llvm::cantFail(ExecutionBudget::create(Limits, Epoch));
  EXPECT_EQ(B->timePolicy(), ExecutionTimePolicy::AbsoluteDeadline);
  auto Work = B->enterWork(Epoch + Microseconds(100));
  ASSERT_TRUE(bool(Work));
  ASSERT_EQ(llvm::toString((*Work)->close(Epoch + Microseconds(200))), "");
  EXPECT_EQ(B->remainingMicroseconds(Epoch + Microseconds(999)), 1u);
  EXPECT_EQ(B->remainingMicroseconds(Epoch + Microseconds(1000)), 0u);
}
TEST(ExecutionBudget, ActiveScopeClockOverflowFailsWithoutChangingBudget) {
  auto B = llvm::cantFail(ExecutionBudget::createWithTimePolicy(
      Limits, ExecutionTimePolicy::ActiveWork, Epoch));
  auto Invalid = B->enterWork(Clock::time_point::max());
  ASSERT_FALSE(bool(Invalid));
  llvm::consumeError(Invalid.takeError());
  EXPECT_EQ(B->remainingMicroseconds(Epoch), Timeout);
  auto Work = B->enterWork(Epoch);
  ASSERT_TRUE(bool(Work));
  ASSERT_EQ(llvm::toString((*Work)->close(Epoch + Microseconds(1))), "");
  EXPECT_EQ(B->remainingMicroseconds(Epoch), Timeout - 1);
  auto BadPolicy = ExecutionBudget::createWithTimePolicy(
      Limits, static_cast<ExecutionTimePolicy>(99), Epoch);
  ASSERT_FALSE(bool(BadPolicy));
  llvm::consumeError(BadPolicy.takeError());
}
} // namespace
} // namespace neverd::emulation
