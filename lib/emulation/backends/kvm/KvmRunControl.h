//===- KvmRunControl.h - Acknowledged KVM entry cancellation -----*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_KVM_RUNCONTROL_H
#define NEVERD_EMULATION_KVM_RUNCONTROL_H

#include "../../core/MachineRunControl.h"

#include "llvm/Support/Error.h"

#include <memory>

namespace neverd::emulation {
/// Own the only thread entering this vCPU. Targeted cancellation is blocked
/// in host userspace and temporarily unblocked by KVM's signal mask. Neither
/// the caller's mask nor process signal dispositions are changed. The vCPU
/// descriptor and its mapped memory must outlive this controller.
class KvmRunControl final {
public:
  static llvm::Expected<std::unique_ptr<KvmRunControl>> create(int VCPU);
  ~KvmRunControl();
  llvm::Error run(MachineRunControl Control);
  KvmRunControl(const KvmRunControl &) = delete;
  KvmRunControl &operator=(const KvmRunControl &) = delete;

private:
  KvmRunControl();
  struct State;
  std::unique_ptr<State> Impl;
};
} // namespace neverd::emulation
#endif
