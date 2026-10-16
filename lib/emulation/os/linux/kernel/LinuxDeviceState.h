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
#include "neverd/emulation/MemoryView.h"

#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <set>

namespace neverd::emulation::linux_model {
struct LinuxDeviceState;
struct BinderCall {
  std::weak_ptr<LinuxDeviceState> Sender;
  uint32_t Thread = 0;
};
struct BinderBuffer {
  uint64_t Size = 0;
  bool Delivered = false;
};
struct BinderWork {
  uint32_t Command = 0, Code = 0, Flags = 0, SenderPID = 0;
  uint64_t Offset = 0;
  std::shared_ptr<BinderCall> Call;
};
struct GPUAllocation {
  MemoryView Bytes;
  uint64_t Flags;
  uint32_t ID = 0; // Zero identifies a legacy user-address import.
  uint64_t UserAddress = 0;
  bool PendingFree = false;
};
struct KGSLProcess {
  std::map<uint64_t, GPUAllocation> Allocations;
  uint64_t NextAddress = 0x10000000;
};
struct LinuxDeviceState {
  const LinuxDeviceOptions *Options;
  size_t NextIoctl = 0;
  std::set<uint32_t> Contexts;
  std::map<uint32_t, uint32_t> QueuedTimestamps;
  std::shared_ptr<KGSLProcess> GPU;
  uint32_t OpenFlags = 0;
  std::map<uint32_t, uint32_t> BinderLoopers;
  int32_t BinderMaxThreads = 0;
  uint32_t BinderRequestedThreads = 0;
  uint32_t BinderStartedThreads = 0;
  bool BinderManager = false;
  LinuxDeviceMapping BinderMapping;
  std::map<uint64_t, BinderBuffer> BinderBuffers;
  std::deque<BinderWork> BinderProcessWork;
  std::map<uint32_t, std::deque<BinderWork>> BinderThreadWork;
  std::map<uint32_t, std::shared_ptr<BinderCall>> BinderIncoming,
      BinderOutgoing;

  bool hasPendingBinderWork() const {
    if (!BinderIncoming.empty() || !BinderOutgoing.empty() ||
        !BinderProcessWork.empty())
      return true;
    for (const auto &[Thread, Work] : BinderThreadWork)
      if (!Work.empty())
        return true;
    return false;
  }
};
} // namespace neverd::emulation::linux_model
#endif
