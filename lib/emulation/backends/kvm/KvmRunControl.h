//===- KvmRunControl.h - Acknowledged KVM entry cancellation -----*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_KVM_RUNCONTROL_H
#define NEVERD_EMULATION_KVM_RUNCONTROL_H

#include "../../core/MachineRunControl.h"

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/Support/Error.h"

#include <memory>

namespace neverd::emulation {
/// Own the only thread entering this vCPU. Targeted cancellation is blocked
/// in host userspace and temporarily unblocked by KVM's signal mask. Neither
/// the caller's mask nor process signal dispositions are changed. The vCPU
/// descriptor and its mapped memory must outlive this controller.
class KvmRunControl final {
public:
  using StateTransfer = llvm::function_ref<llvm::Error()>;
  static llvm::Expected<std::unique_ptr<KvmRunControl>> create(int VCPU);
  ~KvmRunControl();
  llvm::Error run(MachineRunControl Control);
  /// Run transport-only state IO on the entry worker. Prepare runs once,
  /// before any EINTR retries; Capture runs only after a successful entry.
  /// Both callbacks must remain live until this call returns and must not
  /// invoke guest-memory ownership, OS policy or execution observers.
  llvm::Error run(MachineRunControl Control, StateTransfer Prepare,
                  StateTransfer Capture);
  KvmRunControl(const KvmRunControl &) = delete;
  KvmRunControl &operator=(const KvmRunControl &) = delete;

private:
  KvmRunControl();
  struct State;
  std::unique_ptr<State> Impl;
};
} // namespace neverd::emulation
#endif
