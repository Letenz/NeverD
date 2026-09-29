//===- MachineRunControl.h - Shared machine deadline and stop token ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_MACHINERUNCONTROL_H
#define NEVERD_EMULATION_MACHINERUNCONTROL_H

#include "ExecutionLimits.h"

#include <algorithm>
#include <atomic>
#include <chrono>

namespace neverd::emulation {
/// The checked CPU owns the token; a machine may borrow it only until step()
/// returns. Transport cancellation must be acknowledged before that return.
struct MachineRunControl {
  std::chrono::steady_clock::time_point Deadline;
  const std::atomic<bool> *Stop = nullptr;

  bool stopRequested() const { return Stop && Stop->load(); }

  MachineRunControl forNativeStep() const {
    // Normal run budgets are checked at instruction boundaries. Give an
    // instruction already entering the host a bounded transport allowance.
    // Taking the maximum also avoids adding to a possibly maximal deadline.
    return {std::max(Deadline,
                     std::chrono::steady_clock::now() +
                         std::chrono::microseconds(
                             execution_limits::NativeStepGraceMicroseconds)),
            Stop};
  }
};
} // namespace neverd::emulation
#endif
