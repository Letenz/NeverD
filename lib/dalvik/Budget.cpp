//===- Budget.cpp - Shared bounded mobile input resources ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared bounded mobile input resources.
//===----------------------------------------------------------------------===//
#include "neverd/dalvik/Budget.h"

#include "neverd/dalvik/Limits.h"

#include <algorithm>
#include <limits>

namespace neverd::mobile {
void Limits::validate() const {
  if (!timeout || !max_files || !max_bytes ||
      timeout > uint64_t(std::numeric_limits<int64_t>::max() / 1000000000))
    throw Error("timeout and input limits must be positive and representable");
}

Budget::Budget(Limits configured) : limits(configured) {
  limits.validate();
  auto now = std::chrono::steady_clock::now();
  auto available = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::time_point::max() - now);
  if (limits.timeout > static_cast<uint64_t>(available.count()))
    throw Error("mobile analysis deadline is not representable");
  deadline = now + std::chrono::seconds(limits.timeout);
  remaining = std::min<uint64_t>(
      ::neverd::limits::dalvik::kMaxInputWork,
      std::max<uint64_t>(
          ::neverd::limits::dalvik::kMinInputWork,
          limits.max_bytes > ::neverd::limits::dalvik::kInputWorkSaturationBytes
              ? ::neverd::limits::dalvik::kMaxInputWork
              : limits.max_bytes *
                    ::neverd::limits::dalvik::kInputWorkPerByte));
}

void Budget::check() const {
  if (std::chrono::steady_clock::now() > deadline)
    throw Error("mobile analysis exceeded its time budget");
}

void Budget::tick(uint64_t count) {
  check();
  consumeWork(count);
}

void Budget::output(uint64_t count) {
  check();
  if (count > limits.max_bytes - output_bytes)
    throw Error("mobile generated source exceeded its byte budget");
  output_bytes += count;
}

} // namespace neverd::mobile
