//===- KvmVM.h - KVM resource ownership ----------------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_KVM_VM_H
#define NEVERD_EMULATION_KVM_VM_H
#include "../../core/ExecutionDiagnostics.h"
#include "../../core/MachineRunControl.h"
#include "../../core/MemoryProjection.h"
#include "KvmRunControl.h"

#include <fcntl.h>
#include <linux/kvm.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
namespace neverd::emulation {
class KvmVM {
public:
  int System = -1, VM = -1, CPU = -1;
  kvm_run *Run = nullptr;
  size_t RunSize = 0;
  llvm::Error initializeRunControl() {
    auto Created = KvmRunControl::create(CPU);
    if (!Created)
      return Created.takeError();
    Control = std::move(*Created);
    return llvm::Error::success();
  }
  llvm::Error runUntilExit(MachineRunControl Control) {
    if (!this->Control)
      return diagnostic::error(diagnostic::KvmRunControl);
    return this->Control->run(Control);
  }
  virtual ~KvmVM() {
    // Stop the entry worker before unmapping its run state or closing the vCPU.
    Control.reset();
    if (Run)
      munmap(Run, RunSize);
    if (CPU >= 0)
      close(CPU);
    if (VM >= 0)
      close(VM);
    if (System >= 0)
      close(System);
  }
  llvm::Error initialize(llvm::ArrayRef<MemoryRegistration> Mappings) {
#define NEVERD_KVM_STRING(Name, Value) constexpr char Name[] = Value;
#include "KvmProtocol.def"
#undef NEVERD_KVM_STRING
    System = open(Device, O_RDWR | O_CLOEXEC);
    if (System < 0)
      return diagnostic::unavailable(diagnostic::KvmOpen,
                                     BackendAvailability::DeviceAccess);
    if (ioctl(System, KVM_GET_API_VERSION, 0) != KVM_API_VERSION)
      return diagnostic::unavailable(diagnostic::KvmCapabilities,
                                     BackendAvailability::HostAPI);
    if (ioctl(System, KVM_CHECK_EXTENSION, KVM_CAP_SET_GUEST_DEBUG) <= 0)
      return diagnostic::unavailable(diagnostic::KvmCapabilities,
                                     BackendAvailability::MissingCapability);
    VM = ioctl(System, KVM_CREATE_VM, 0);
    if (VM < 0)
      return diagnostic::error(diagnostic::KvmCreate);
    uint32_t Slot = 0;
    for (const auto &Mapping : Mappings) {
      kvm_userspace_memory_region Region{};
      Region.slot = Slot++;
      Region.guest_phys_addr = Mapping.Physical;
      Region.memory_size = Mapping.Size;
      Region.userspace_addr = reinterpret_cast<uintptr_t>(Mapping.Backing);
      if (ioctl(VM, KVM_SET_USER_MEMORY_REGION, &Region) < 0)
        return diagnostic::error(diagnostic::KvmMap);
    }
    CPU = ioctl(VM, KVM_CREATE_VCPU, 0);
    if (CPU < 0)
      return diagnostic::error(diagnostic::KvmCreate);
    int MappingSize = ioctl(System, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (MappingSize < int(sizeof(kvm_run)))
      return diagnostic::error(diagnostic::KvmMap);
    RunSize = MappingSize;
    void *Mapping =
        mmap(nullptr, RunSize, PROT_READ | PROT_WRITE, MAP_SHARED, CPU, 0);
    if (Mapping == MAP_FAILED)
      return diagnostic::error(diagnostic::KvmMap);
    Run = static_cast<kvm_run *>(Mapping);
    return initializeRunControl();
  }

private:
  std::unique_ptr<KvmRunControl> Control;
};
} // namespace neverd::emulation
#endif
