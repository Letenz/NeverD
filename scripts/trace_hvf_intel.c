// Temporary diagnostic: observe public HVF calls in an independent executable.
#include <Hypervisor/Hypervisor.h>
#include <Hypervisor/hv_vmx.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>

static hv_return_t trace_vmcs(hv_vcpuid_t CPU, uint32_t Field, uint64_t Value) {
  hv_return_t (*Original)(hv_vcpuid_t, uint32_t, uint64_t) =
      dlsym(RTLD_NEXT, "hv_vmx_vcpu_write_vmcs");
  const hv_return_t Status = Original(CPU, Field, Value);
  fprintf(stderr, "TRACE write 0x%x = 0x%llx status=0x%x\n", Field,
          (unsigned long long)Value, (uint32_t)Status);
  return Status;
}

static void snapshot(hv_vcpuid_t CPU, const char *Phase) {
  const uint32_t Fields[] = {VMCS_CTRL_PIN_BASED,
                             VMCS_CTRL_CPU_BASED,
                             VMCS_CTRL_CPU_BASED2,
                             VMCS_CTRL_VMENTRY_CONTROLS,
                             VMCS_CTRL_VMEXIT_CONTROLS,
                             VMCS_GUEST_CR0,
                             VMCS_GUEST_CR3,
                             VMCS_GUEST_CR4,
                             VMCS_GUEST_IA32_EFER,
                             VMCS_CTRL_CR0_MASK,
                             VMCS_CTRL_CR0_SHADOW,
                             VMCS_CTRL_CR4_MASK,
                             VMCS_CTRL_CR4_SHADOW,
                             VMCS_GUEST_RIP,
                             VMCS_GUEST_RSP,
                             VMCS_GUEST_RFLAGS,
                             VMCS_GUEST_CS_AR,
                             VMCS_GUEST_SS_AR,
                             VMCS_GUEST_TR_AR,
                             VMCS_GUEST_LDTR_AR,
                             VMCS_RO_INSTR_ERROR,
                             VMCS_RO_EXIT_REASON};
  fprintf(stderr, "TRACE %s\n", Phase);
  for (unsigned I = 0; I < sizeof(Fields) / sizeof(Fields[0]); ++I) {
    uint64_t Value = 0;
    const hv_return_t Status = hv_vmx_vcpu_read_vmcs(CPU, Fields[I], &Value);
    fprintf(stderr, "TRACE read 0x%x = 0x%llx status=0x%x\n", Fields[I],
            (unsigned long long)Value, (uint32_t)Status);
  }
}

static hv_return_t trace_run(hv_vcpuid_t CPU) {
  hv_return_t (*Original)(hv_vcpuid_t) = dlsym(RTLD_NEXT, "hv_vcpu_run");
  snapshot(CPU, "before run");
  const hv_return_t Status = Original(CPU);
  fprintf(stderr, "TRACE run status=0x%x\n", (uint32_t)Status);
  snapshot(CPU, "after run");
  return Status;
}

static hv_return_t trace_until(hv_vcpuid_t CPU, uint64_t Deadline) {
  hv_return_t (*Original)(hv_vcpuid_t, uint64_t) =
      dlsym(RTLD_NEXT, "hv_vcpu_run_until");
  snapshot(CPU, "before until");
  const hv_return_t Status = Original(CPU, Deadline);
  fprintf(stderr, "TRACE until status=0x%x\n", (uint32_t)Status);
  snapshot(CPU, "after until");
  return Status;
}

// dyld's __interpose section is a sequence of replacement/original pairs.
__attribute__((used, section("__DATA,__interpose"))) static const struct {
  const void *Replacement;
  const void *Original;
} Interpositions[] = {
    {(const void *)trace_vmcs, (const void *)hv_vmx_vcpu_write_vmcs},
    {(const void *)trace_run, (const void *)hv_vcpu_run},
    {(const void *)trace_until, (const void *)hv_vcpu_run_until}};
