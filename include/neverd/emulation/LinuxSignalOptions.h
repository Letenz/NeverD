//===- LinuxSignalOptions.h - Explicit signal dispositions ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_LINUXSIGNALOPTIONS_H
#define NEVERD_EMULATION_LINUXSIGNALOPTIONS_H

#include <cstdint>
#include <map>
#include <optional>

namespace neverd::emulation {
struct LinuxSignalAction {
  uint64_t Handler = 0;
  uint64_t Flags = 0;
  uint64_t Restorer = 0;
  uint64_t Mask = 0;
};
/// Initial, process-wide kernel dispositions. Missing signals have unknown
/// actions; an all-zero action must be supplied explicitly. Handler/restorer
/// addresses are guest values. Missing initial thread state remains unknown.
struct LinuxSignalOptions {
  std::map<int32_t, LinuxSignalAction> Actions;
  /// Leader's blocked set; newly created threads inherit their creator's set.
  std::optional<uint64_t> InitialMask;
  /// Explicitly absent alternate signal stack. Enabled stacks are not modeled.
  bool AlternateStackDisabled = false;
};
} // namespace neverd::emulation
#endif
