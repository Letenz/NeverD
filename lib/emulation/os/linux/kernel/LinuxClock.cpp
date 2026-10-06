//===- LinuxClock.cpp - Explicit workload time and idle advancement ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxTime.h"

#include "llvm/Support/FormatVariadic.h"

namespace neverd::emulation::linux_model {
namespace {
constexpr uint64_t NanosecondsPerSecond = 1000000000;

bool advancingClock(int32_t ID) {
  return ID == ClockRealtime || ID == ClockMonotonic || ID == ClockBootTime;
}

std::optional<LinuxTimespec> shifted(LinuxTimespec Value, uint64_t Elapsed) {
  const uint64_t Nanoseconds =
      Value.Nanoseconds + Elapsed % NanosecondsPerSecond;
  const int64_t Seconds =
      Elapsed / NanosecondsPerSecond + Nanoseconds / NanosecondsPerSecond;
  if (Value.Seconds > INT64_MAX - Seconds)
    return std::nullopt;
  return LinuxTimespec{Value.Seconds + Seconds,
                       int64_t(Nanoseconds % NanosecondsPerSecond)};
}

void unsupported(ProcessResult &Result, llvm::StringRef Reason) {
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = Reason.str();
}
} // namespace

bool isKnownClock(int32_t ID) {
  switch (ID) {
#define NEVERD_LINUX_CLOCK(Name, Number)                                       \
  case Number:                                                                 \
    return true;
#include "../LinuxValues.def"
#undef NEVERD_LINUX_CLOCK
  default:
    return false;
  }
}

bool isNormalizedTimespec(const LinuxTimespec &Value) {
  return Value.Nanoseconds >= 0 &&
         uint64_t(Value.Nanoseconds) < NanosecondsPerSecond;
}

llvm::Error validateTimeOptions(const LinuxTimeOptions &Options) {
  for (const auto &[ID, Value] : Options.Clocks) {
    if (!isKnownClock(ID))
      return failure(TimeClockOption);
    if (!isNormalizedTimespec(Value))
      return failure(TimeNanoseconds);
    if (Options.AdvanceOnIdle && !advancingClock(ID))
      return failure(TimeAdvanceClock);
  }
  return llvm::Error::success();
}

std::optional<LinuxTimespec> LinuxClock::read(int32_t ID,
                                              ProcessResult &Result) const {
  if (Options) {
    auto I = Options->Clocks.find(ID);
    if (I != Options->Clocks.end()) {
      auto Value = shifted(I->second, Elapsed);
      if (!Value)
        unsupported(Result, TimeAdvanceOverflow);
      return Value;
    }
  }
  unsupported(Result, llvm::formatv(TimeClockMissing, ID).str());
  return std::nullopt;
}

std::optional<uint64_t> LinuxClock::deadline(const LinuxTimespec &Duration,
                                             ProcessResult &Result) const {
  assert(advancesOnIdle() && Duration.Seconds >= 0 &&
         isNormalizedTimespec(Duration));
  // Saturated Linux ktime deadlines are outside this finite model. They do
  // not become EINVAL, an immediate success or a wrapped earlier deadline.
  if (uint64_t(Duration.Seconds) >=
      uint64_t(INT64_MAX) / NanosecondsPerSecond) {
    unsupported(Result, SleepRange);
    return std::nullopt;
  }
  const uint64_t DurationNS =
      uint64_t(Duration.Seconds) * NanosecondsPerSecond + Duration.Nanoseconds;
  if (DurationNS > uint64_t(INT64_MAX) - Elapsed) {
    unsupported(Result, SleepRange);
    return std::nullopt;
  }
  return Elapsed + DurationNS;
}

bool LinuxClock::advanceTo(uint64_t Deadline, ProcessResult &Result) {
  assert(advancesOnIdle() && Deadline >= Elapsed && Deadline <= INT64_MAX);
  for (const auto &[ID, Value] : Options->Clocks) {
    if (!shifted(Value, Deadline)) {
      unsupported(Result, TimeAdvanceOverflow);
      return false;
    }
  }
  Elapsed = Deadline;
  return true;
}
} // namespace neverd::emulation::linux_model
