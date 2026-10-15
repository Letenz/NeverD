//===- LinuxDeviceState.h - Shared device open lifetime --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Descriptor aliases and VMAs retain the same device open description.
#ifndef NEVERD_EMULATION_OS_LINUX_LINUXDEVICESTATE_H
#define NEVERD_EMULATION_OS_LINUX_LINUXDEVICESTATE_H
#include "neverd/emulation/LinuxDeviceOptions.h"

#include <cstddef>
#include <set>

namespace neverd::emulation::linux_model {
struct LinuxDeviceState {
  const LinuxDeviceOptions *Options;
  size_t NextIoctl = 0;
  std::set<uint32_t> Contexts;
};
} // namespace neverd::emulation::linux_model
#endif
