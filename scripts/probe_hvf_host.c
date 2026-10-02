//===- probe_hvf_host.c - Require a usable native macOS hypervisor --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include <Hypervisor/Hypervisor.h>
#include <stdint.h>
#include <stdio.h>

static int report(const char *Operation, hv_return_t Status) {
  printf("%s: 0x%08x\n", Operation, (uint32_t)Status);
  return Status != HV_SUCCESS;
}

int main(void) {
#if defined(__arm64__)
  hv_return_t Status = hv_vm_create(NULL);
  hv_vcpu_t CPU;
  hv_vcpu_exit_t *Exit;
#elif defined(__x86_64__)
  hv_return_t Status = hv_vm_create(HV_VM_DEFAULT);
  hv_vcpuid_t CPU;
#else
#error "HVF host probe requires a native ARM64 or x64 executable"
#endif
  if (report("hv_vm_create", Status))
    return 1;
#if defined(__arm64__)
  Status = hv_vcpu_create(&CPU, &Exit, NULL);
#else
  Status = hv_vcpu_create(&CPU, HV_VCPU_DEFAULT);
#endif
  int Failed = report("hv_vcpu_create", Status);
  if (!Failed)
    Failed |= report("hv_vcpu_destroy", hv_vcpu_destroy(CPU));
  Failed |= report("hv_vm_destroy", hv_vm_destroy());
  return Failed;
}
