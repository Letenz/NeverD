//===- Control.h - Shared comparison operation state and stop --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Common completion and cooperative-stop contract for snapshot extraction,
/// correspondence, regions, dependency analysis and bounded validation.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DIFF_CONTROL_H
#define NEVERD_DIFF_CONTROL_H

#include "llvm/ADT/StringRef.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

namespace neverd::diff {

enum class ComparisonState : uint8_t {
  Complete,
  Cancelled,
  TimedOut,
  ResourceLimited,
};
llvm::StringRef stateName(ComparisonState State);
std::optional<ComparisonState> stateNamed(llvm::StringRef Name);

struct ComparisonControl {
  /// Set from any thread to stop at the next cooperative check.
  const std::atomic<bool> *Cancel = nullptr;
  std::optional<std::chrono::steady_clock::time_point> Deadline;
  /// Called from the operation's coordinator as phases advance.
  std::function<void(llvm::StringRef Phase, uint64_t Done, uint64_t Total)>
      Progress;
  std::optional<ComparisonState> stop() const;
};

} // namespace neverd::diff

#endif // NEVERD_DIFF_CONTROL_H
