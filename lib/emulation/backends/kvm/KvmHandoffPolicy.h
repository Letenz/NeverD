//===- KvmHandoffPolicy.h - Adaptive vCPU handoff waiting -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_KVM_HANDOFFPOLICY_H
#define NEVERD_EMULATION_KVM_HANDOFFPOLICY_H

namespace neverd::emulation {
namespace kvm::handoff {
#define NEVERD_KVM_HANDOFF_VALUE(Name, Value)                                  \
  inline constexpr unsigned Name = Value;
#include "KvmHandoffPolicy.def"
#undef NEVERD_KVM_HANDOFF_VALUE
} // namespace kvm::handoff

/// One instance per waiting direction, owned by that direction's thread.
/// Failed short waits suppress polling for a bounded number of handoffs.
/// Periodic retries allow a descheduled or previously slow peer to recover.
/// This policy grants no access to a request or captured state: the transport
/// mutex still owns publication, acknowledgement and callback lifetimes.
class KvmHandoffPolicy {
public:
  bool begin() {
    if (!Remaining)
      return true;
    --Remaining;
    return false;
  }
  void finish(bool Ready) {
    if (Ready) {
      Misses = 0;
      return;
    }
    if (++Misses == kvm::handoff::MissLimit) {
      Remaining = kvm::handoff::RetryEntries;
      Misses = 0;
    }
  }

private:
  unsigned Misses = 0, Remaining = 0;
};
} // namespace neverd::emulation
#endif
