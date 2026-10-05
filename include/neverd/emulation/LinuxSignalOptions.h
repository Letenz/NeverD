//===- LinuxSignalOptions.h - Explicit signal dispositions ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_LINUXSIGNALOPTIONS_H
#define NEVERD_EMULATION_LINUXSIGNALOPTIONS_H

#include <cstdint>
#include <map>

namespace neverd::emulation {
struct LinuxSignalAction {
  uint64_t Handler = 0;
  uint64_t Flags = 0;
  uint64_t Restorer = 0;
  uint64_t Mask = 0;
};
/// Initial, process-wide kernel dispositions. Missing signals have unknown
/// actions; an all-zero action must be supplied explicitly. Handler/restorer
/// addresses are guest values. This does not enable signal delivery.
struct LinuxSignalOptions {
  std::map<int32_t, LinuxSignalAction> Actions;
};
} // namespace neverd::emulation
#endif
