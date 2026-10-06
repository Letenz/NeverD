//===- LinuxClockTests.cpp - Independent virtual time invariants ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#include "os/linux/kernel/LinuxTime.h"

namespace neverd::emulation {
namespace {
TEST(LinuxClock, IdleAdvancementSharesElapsedTimeWithoutOverflowingEpochs) {
  std::optional<LinuxTimeOptions> Options{std::in_place};
  Options->AdvanceOnIdle = true;
  Options->Clocks = {
      {0, {4294967297, 999999998}}, {1, {12, 3}}, {7, {INT64_MIN, 999999999}}};
  linux_model::LinuxClock Clock(Options);
  ProcessResult R{ProcessProfile::LinuxELF64,
                  GuestArchitecture::X64,
                  ExecutionBackendKind::Unicorn,
                  {}};
  ASSERT_FALSE(bool(linux_model::validateTimeOptions(*Options)));
  ASSERT_EQ(Clock.deadline({1, 4}, R), 1000000004u);
  ASSERT_EQ(Clock.elapsed(), 0u);
  ASSERT_TRUE(Clock.advanceTo(1000000004, R));
  EXPECT_EQ(Clock.read(0, R)->Seconds, 4294967299);
  EXPECT_EQ(Clock.read(0, R)->Nanoseconds, 2);
  EXPECT_EQ(Clock.read(1, R)->Seconds, 13);
  EXPECT_EQ(Clock.read(1, R)->Nanoseconds, 7);
  EXPECT_EQ(Clock.read(7, R)->Seconds, INT64_MIN + 2);
  EXPECT_EQ(Clock.read(7, R)->Nanoseconds, 3);
  EXPECT_EQ(Clock.elapsed(), 1000000004u);
  EXPECT_FALSE(Clock.read(2, R));
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
}

TEST(LinuxClock, OverflowRefusesAllClocksAndSaturatedSleepIsNotEINVAL) {
  std::optional<LinuxTimeOptions> Options{std::in_place};
  Options->AdvanceOnIdle = true;
  Options->Clocks = {{0, {INT64_MAX, 999999998}}, {1, {3, 4}}};
  linux_model::LinuxClock Clock(Options);
  ProcessResult R{ProcessProfile::LinuxELF64,
                  GuestArchitecture::AArch64,
                  ExecutionBackendKind::Unicorn,
                  {}};
  EXPECT_FALSE(Clock.deadline({INT64_MAX, 0}, R));
  EXPECT_EQ(R.Diagnostic, linux_model::SleepRange);
  ASSERT_TRUE(Clock.advanceTo(1, R));
  EXPECT_FALSE(Clock.advanceTo(2, R));
  EXPECT_EQ(R.Diagnostic, linux_model::TimeAdvanceOverflow);
  EXPECT_EQ(Clock.elapsed(), 1u);
  EXPECT_EQ(Clock.read(0, R)->Seconds, INT64_MAX);
  EXPECT_EQ(Clock.read(0, R)->Nanoseconds, 999999999);
  EXPECT_EQ(Clock.read(1, R)->Nanoseconds, 5);

  Options->Clocks.clear();
  ASSERT_TRUE(Clock.advanceTo(INT64_MAX - 1, R));
  EXPECT_FALSE(Clock.deadline({0, 2}, R));
  EXPECT_EQ(R.Diagnostic, linux_model::SleepRange);
  EXPECT_EQ(Clock.elapsed(), uint64_t(INT64_MAX) - 1);
}
} // namespace
} // namespace neverd::emulation
