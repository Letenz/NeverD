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
#include <map>
#include <set>

namespace neverd::emulation::linux_model {
struct LinuxDeviceState {
  const LinuxDeviceOptions *Options;
  size_t NextIoctl = 0;
  std::set<uint32_t> Contexts;
  uint32_t OpenFlags = 0;
  std::map<uint32_t, uint32_t> BinderLoopers;
  int32_t BinderMaxThreads = 0;
  uint32_t BinderRequestedThreads = 0;
  uint32_t BinderStartedThreads = 0;
};
} // namespace neverd::emulation::linux_model
#endif
