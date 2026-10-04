//===- HvfIntelDeadline.h - Owner-thread finite Intel entries -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_HVFINTELDEADLINE_H
#define NEVERD_EMULATION_HVFINTELDEADLINE_H

#include "../../core/MachineRunControl.h"

#include "llvm/Support/Error.h"

#include <cassert>
#include <limits>

namespace neverd::emulation::hvf {
inline constexpr uint64_t IntelInterruptExit = 1;
inline constexpr uint64_t IntelTimerExit = 52;
inline bool isIntelTransportExit(uint64_t Reason) {
  // Compare the complete exit reason: an entry-failure bit is not an IRQ.
  return Reason == IntelInterruptExit || Reason == IntelTimerExit;
}

/// MachNow must be sampled before Now. Rounding down keeps the native deadline
/// within the borrowed total budget, even if the thread is descheduled before
/// entry. A past deadline is valid; it cannot certify instruction completion.
inline uint64_t intelSliceDeadline(uint64_t MachNow,
                                   std::chrono::steady_clock::time_point Now,
                                   std::chrono::steady_clock::time_point End,
                                   uint32_t Numer, uint32_t Denom) {
  using Clock = std::chrono::steady_clock;
  constexpr auto Poll =
      std::chrono::microseconds(execution_limits::CancelRetryMicroseconds);
  constexpr uint64_t MaxNanos =
      std::chrono::duration_cast<std::chrono::nanoseconds>(Poll).count();
  static_assert(MaxNanos <= UINT64_MAX / UINT32_MAX);
  assert(Numer && Denom);
  constexpr uint64_t LastFinite = UINT64_MAX - 1;
  MachNow = std::min(MachNow, LastFinite);
  if (End <= Now)
    return MachNow;
  // Clamp before subtraction; even opposite extreme clock epochs are safe.
  const auto SliceEnd =
      Now > Clock::time_point::max() - Poll ? End : std::min(End, Now + Poll);
  const auto Nanos = uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(SliceEnd - Now)
          .count());
  const uint64_t Ticks = (Nanos * Denom) / Numer;
  return MachNow + std::min(Ticks, LastFinite - MachNow);
}

struct IntelEntryResult {
  llvm::Error Result;
  bool Cancelled;
};

/// Enter performs one host entry and reads its full exit reason on the owner.
/// No state is prepared again between transport exits. Intel SDM 26.2/26.5.2:
/// MTF has priority over debug traps, the preemption timer and external IRQs.
/// With no injected events and intercepted exceptions, these transport returns
/// cannot stand in for the checked instruction's MTF/exception completion.
template <typename Deadline, typename Entry, typename Completion>
IntelEntryResult runIntelEntry(MachineRunControl Control, Deadline NextDeadline,
                               Entry Enter, Completion Complete) {
  bool Entered = false;
  while (!Control.interrupted()) {
    const auto Until = NextDeadline();
    if (Control.interrupted())
      break;
    auto Reason = Enter(Until);
    if (!Reason)
      return {Reason.takeError(), Control.interrupted()};
    Entered = true;
    const bool Cancelled = Control.interrupted();
    if (!isIntelTransportExit(*Reason) || Cancelled) {
      // Classify real guest exits even if stop raced with the return. Native
      // errors and completion/capture errors must take priority over stop.
      auto Result = Complete(Cancelled);
      return {std::move(Result), Cancelled || Control.interrupted()};
    }
  }
  // A stop between transport return and re-entry acknowledges the last real
  // return. Rejected initial admission must not inspect stale native state.
  return {Entered ? Complete(true) : llvm::Error::success(), true};
}
} // namespace neverd::emulation::hvf
#endif
