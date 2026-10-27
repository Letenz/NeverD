//===- Budget.h - Shared bounded mobile input resources ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared bounded mobile input resources.
//===----------------------------------------------------------------------===//
#pragma once

#include <chrono>
#include <cstdint>
#include <stdexcept>

namespace neverd::mobile {
struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct Limits {
  uint64_t timeout = 300;
  uint64_t max_files = 20000;
  uint64_t max_bytes = uint64_t(2) * 1024 * 1024 * 1024;
  void validate() const;
};

struct Budget {
  Limits limits;
  std::chrono::steady_clock::time_point deadline;
  uint64_t remaining;
  uint64_t output_bytes = 0;
  explicit Budget(Limits limits = {});
  void tick(uint64_t count = 1);
  // For bounded inner loops that check the deadline separately. Work is
  // debited immediately so nested readers share the same remaining limit.
  void consumeWork(uint64_t count) {
    if (count > remaining)
      throw Error("mobile analysis exceeded its work budget");
    remaining -= count;
  }
  void output(uint64_t count);
  void check() const;
};

} // namespace neverd::mobile
