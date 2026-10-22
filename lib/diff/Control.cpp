//===- Control.cpp - Cooperative comparison operation stops -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared cancellation and deadline precedence for every comparison phase.
//===----------------------------------------------------------------------===//

#include "neverd/diff/Control.h"

namespace neverd::diff {

std::optional<ComparisonState> ComparisonControl::stop() const {
  if (Cancel && Cancel->load(std::memory_order_relaxed))
    return ComparisonState::Cancelled;
  if (Deadline && std::chrono::steady_clock::now() >= *Deadline)
    return ComparisonState::TimedOut;
  return std::nullopt;
}

} // namespace neverd::diff
