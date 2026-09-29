//===- KvmVM.h - KVM resource ownership ----------------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_KVM_VM_H
#define NEVERD_EMULATION_KVM_VM_H
#include "../../core/ExecutionDiagnostics.h"

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
  virtual ~KvmVM() {
    if (Run)
      munmap(Run, RunSize);
    if (CPU >= 0)
      close(CPU);
    if (VM >= 0)
      close(VM);
    if (System >= 0)
      close(System);
  }
  llvm::Error initialize(uint8_t *Backing, uint64_t Size) {
#define NEVERD_KVM_STRING(Name, Value) constexpr char Name[] = Value;
#include "KvmProtocol.def"
#undef NEVERD_KVM_STRING
    System = open(Device, O_RDWR | O_CLOEXEC);
    if (System < 0)
      return diagnostic::unavailable(diagnostic::KvmOpen);
    if (ioctl(System, KVM_GET_API_VERSION, 0) != KVM_API_VERSION ||
        ioctl(System, KVM_CHECK_EXTENSION, KVM_CAP_SET_GUEST_DEBUG) <= 0)
      return diagnostic::unavailable(diagnostic::KvmCapabilities);
    VM = ioctl(System, KVM_CREATE_VM, 0);
    if (VM < 0)
      return diagnostic::error(diagnostic::KvmCreate);
    kvm_userspace_memory_region Region{};
    Region.memory_size = Size;
    Region.userspace_addr = reinterpret_cast<uintptr_t>(Backing);
    if (ioctl(VM, KVM_SET_USER_MEMORY_REGION, &Region) < 0)
      return diagnostic::error(diagnostic::KvmMap);
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
    return llvm::Error::success();
  }
};
} // namespace neverd::emulation
#endif
