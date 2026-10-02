//===- probe_hvf_intel.h - Independent native Intel instruction probe ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_SCRIPTS_PROBE_HVF_INTEL_H
#define NEVERD_SCRIPTS_PROBE_HVF_INTEL_H
#include <Hypervisor/hv_vmx.h>
#include <cpuid.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int probe_variant(const char *Name) {
  const char *Variant = getenv("NEVERD_HVF_PROBE_VARIANT");
  return Variant &&
         (strcmp(Variant, Name) == 0 || strcmp(Variant, "combined") == 0);
}

static int probe_control(hv_vcpuid_t CPU, uint32_t Field, uint64_t Required) {
  uint64_t Must = 0, May = 0;
  if (report("probe VMCS capabilities",
             hv_vmx_vcpu_get_cap_write_vmcs(CPU, Field, &Must, &May)))
    return 1;
  printf("probe control 0x%x: must=0x%llx may=0x%llx required=0x%llx\n", Field,
         (unsigned long long)Must, (unsigned long long)May,
         (unsigned long long)Required);
  if ((Required & May) != Required) {
    printf("probe control 0x%x cannot supply 0x%llx\n", Field,
           (unsigned long long)Required);
    return 1;
  }
  return report("probe VMCS control",
                hv_vmx_vcpu_write_vmcs(CPU, Field, Must | Required));
}

// Original MOV/HALT program, identity mapped through three independently
// authored page-table pages. This deliberately has no NeverD, decoder, FP
// packet or guest OS dependency. Backing lives until the caller destroys VM.
static int probe_intel_execution(hv_vcpuid_t CPU, void **Backing, int Legacy) {
  printf("Intel entry API: %s\n", Legacy ? "hv_vcpu_run" : "hv_vcpu_run_until");
  const char *MSRMaskText = getenv("NEVERD_HVF_PROBE_MSRS");
  const unsigned MSRMask = MSRMaskText ? (unsigned)strtoul(MSRMaskText, NULL, 0)
                           : probe_variant("msrs") ? 0xfff
                                                   : 0;
  if (MSRMask) {
    const uint32_t MSRs[] = {0xc0000081, 0xc0000082, 0xc0000083, 0xc0000084,
                             0xc0000100, 0xc0000101, 0xc0000102, 0xc0000103,
                             0x174,      0x175,      0x176,      0x10};
    for (unsigned I = 0; I < sizeof(MSRs) / sizeof(MSRs[0]); ++I)
      if ((MSRMask & (1u << I)) &&
          report("probe native MSR",
                 hv_vcpu_enable_native_msr(CPU, MSRs[I], 1)))
        return 1;
  }
  for (unsigned I = HV_VMX_CAP_CR0_FIXED0; I <= HV_VMX_CAP_CR4_FIXED1; ++I) {
    uint64_t Value = 0;
    const hv_return_t S =
        hv_vmx_read_capability((hv_vmx_capability_t)I, &Value);
    printf("hardware capability %u: status=0x%x value=0x%llx\n", I, (uint32_t)S,
           (unsigned long long)Value);
  }
  const size_t Bytes = 65536;
  if (posix_memalign(Backing, (size_t)sysconf(_SC_PAGESIZE), Bytes))
    return 1;
  memset(*Backing, 0, Bytes);
  unsigned char *RAM = *Backing;
  ((uint64_t *)(RAM + 0x1000))[0] = 0x2003; // PML4 -> PDPT
  ((uint64_t *)(RAM + 0x2000))[0] = 0x3003; // PDPT -> PD
  ((uint64_t *)(RAM + 0x3000))[0] = 0x0083; // identity 2 MiB page
  const unsigned char Program[] = {0xb8, 37, 0, 0, 0, 0xf4}; // mov eax,37; hlt
  memcpy(RAM + 0x4000, Program, sizeof(Program));
  if (probe_variant("tables")) {
    uint64_t *GDT = (uint64_t *)(RAM + 0x5000);
    GDT[1] = 0x00af9b000000ffffULL;
    GDT[2] = 0x00cf93000000ffffULL;
    GDT[5] = 0x00008b0060000067ULL;
  }
  if (report("probe hv_vm_map",
             hv_vm_map(RAM, 0, Bytes,
                       HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC)))
    return 1;
  if (probe_control(CPU, VMCS_CTRL_PIN_BASED, PIN_BASED_INTR | PIN_BASED_NMI) ||
      probe_control(CPU, VMCS_CTRL_CPU_BASED,
                    CPU_BASED_SECONDARY_CTLS | CPU_BASED_HLT |
                        CPU_BASED_TPR_SHADOW) ||
      probe_control(CPU, VMCS_CTRL_CPU_BASED2, CPU_BASED2_EPT) ||
      probe_control(CPU, VMCS_CTRL_VMENTRY_CONTROLS, VMENTRY_GUEST_IA32E) ||
      (!probe_variant("order") &&
       (probe_control(CPU, VMCS_GUEST_CR0, 0x80010033) ||
        probe_control(CPU, VMCS_GUEST_CR4, 0x2620))) ||
      probe_control(CPU, VMCS_CTRL_CR0_MASK,
                    probe_variant("cr0-mask") ? 0x80000010 : 0) ||
      probe_control(CPU, VMCS_CTRL_CR4_MASK, 0x2000))
    return 1;
  const struct {
    uint32_t Field;
    uint64_t Value;
  } Fields[] = {{VMCS_CTRL_EXC_BITMAP,
                 probe_variant("exception-bitmap") ? 0 : UINT32_MAX},
                {VMCS_CTRL_VMENTRY_IRQ_INFO, 0},
                {VMCS_GUEST_CR3, 0x1000},
                {VMCS_GUEST_IA32_EFER, 0xd00},
                {VMCS_CTRL_CR0_SHADOW, 0x80010033},
                {VMCS_CTRL_CR4_SHADOW, 0x620},
                {VMCS_GUEST_ACTIVITY_STATE, 0},
                {VMCS_GUEST_INTERRUPTIBILITY, 0},
                {VMCS_GUEST_DEBUG_EXC, 0},
                {VMCS_GUEST_DR7, 0x400},
                {VMCS_GUEST_GDTR_BASE, probe_variant("tables") ? 0x5000 : 0},
                {VMCS_GUEST_GDTR_LIMIT, probe_variant("tables") ? 55 : 0},
                {VMCS_GUEST_IDTR_BASE, 0},
                {VMCS_GUEST_IDTR_LIMIT, 0},
                {VMCS_GUEST_LDTR, 0},
                {VMCS_GUEST_LDTR_BASE, 0},
                {VMCS_GUEST_LDTR_LIMIT, 0},
                {VMCS_GUEST_LDTR_AR, 0x10000},
                {VMCS_GUEST_TR, 0x28},
                {VMCS_GUEST_TR_BASE, probe_variant("tables") ? 0x6000 : 0},
                {VMCS_GUEST_TR_LIMIT, 0x67},
                {VMCS_GUEST_TR_AR, 0x8b}};
  for (unsigned I = 0; I < sizeof(Fields) / sizeof(Fields[0]); ++I)
    if (report("probe guest VMCS",
               hv_vmx_vcpu_write_vmcs(CPU, Fields[I].Field, Fields[I].Value)))
      return 1;
  if (probe_variant("order") &&
      (probe_control(CPU, VMCS_GUEST_CR4, 0x2620) ||
       probe_control(CPU, VMCS_GUEST_CR0, 0x80010033)))
    return 1;
  if (probe_variant("tpr") &&
      report("probe TPR", hv_vcpu_write_register(CPU, HV_X86_TPR, 0)))
    return 1;
  for (unsigned I = 0; I < 6; ++I) {
    const int Code = I == 1;
    if (report("probe segment selector",
               hv_vmx_vcpu_write_vmcs(CPU, VMCS_GUEST_ES + 2 * I,
                                      Code ? 8 : 16)) ||
        report("probe segment base",
               hv_vmx_vcpu_write_vmcs(CPU, VMCS_GUEST_ES_BASE + 2 * I, 0)) ||
        report("probe segment limit",
               hv_vmx_vcpu_write_vmcs(CPU, VMCS_GUEST_ES_LIMIT + 2 * I,
                                      UINT32_MAX)) ||
        report("probe segment access",
               hv_vmx_vcpu_write_vmcs(CPU, VMCS_GUEST_ES_AR + 2 * I,
                                      Code ? 0xa09b : 0xc093)))
      return 1;
  }
  uint64_t InitialXCR0 = 0;
  if (report("probe initial XCR0",
             hv_vcpu_read_register(CPU, HV_X86_XCR0, &InitialXCR0)))
    return 1;
  printf("initial XCR0=0x%llx\n", (unsigned long long)InitialXCR0);
  if (report("probe XCR0", hv_vcpu_write_register(CPU, HV_X86_XCR0, 3)))
    return 1;
  unsigned A, B, C, D;
  if (!__get_cpuid_count(0xd, 0, &A, &B, &C, &D) || B > 32768 || B < 576)
    return 1;
  if (report("probe initial FP state",
             hv_vcpu_read_fpstate(CPU, RAM + 32768, B)) ||
      report("probe restore FP state",
             hv_vcpu_write_fpstate(CPU, RAM + 32768, B)))
    return 1;
  for (unsigned Step = 0; Step < 2; ++Step) {
    printf("independent Intel probe: %s\n", Step ? "MTF" : "HLT");
    if (probe_control(CPU, VMCS_CTRL_CPU_BASED,
                      CPU_BASED_SECONDARY_CTLS | CPU_BASED_HLT |
                          CPU_BASED_TPR_SHADOW | (Step ? CPU_BASED_MTF : 0)) ||
        report("probe RIP", hv_vcpu_write_register(CPU, HV_X86_RIP, 0x4000)) ||
        report("probe RAX", hv_vcpu_write_register(CPU, HV_X86_RAX, 0)) ||
        report("probe RFLAGS", hv_vcpu_write_register(CPU, HV_X86_RFLAGS, 2)))
      return 1;
    int Completed = 0;
    // The legacy API also returns transparently handled exits. Permit only
    // mapped EPT faults and host IRQs, with a bound independent of the timeout.
    for (unsigned Attempt = 0; Attempt < 32; ++Attempt) {
      const hv_return_t Status =
          Legacy ? hv_vcpu_run(CPU)
                 : hv_vcpu_run_until(CPU, HV_DEADLINE_FOREVER);
      uint64_t Reason = 0, InstructionError = 0, RAX = 0;
      const int ReadFailed =
          report("probe exit reason",
                 hv_vmx_vcpu_read_vmcs(CPU, VMCS_RO_EXIT_REASON, &Reason)) |
          report("probe instruction error",
                 hv_vmx_vcpu_read_vmcs(CPU, VMCS_RO_INSTR_ERROR,
                                       &InstructionError)) |
          report("probe RAX read",
                 hv_vcpu_read_register(CPU, HV_X86_RAX, &RAX));
      printf("probe result: status=0x%x reason=0x%llx instruction_error=0x%llx "
             "rax=0x%llx\n",
             (uint32_t)Status, (unsigned long long)Reason,
             (unsigned long long)InstructionError, (unsigned long long)RAX);
      if (Status != HV_SUCCESS || ReadFailed)
        return 1;
      if (Reason == (Step ? VMX_REASON_MTF : VMX_REASON_HLT)) {
        Completed = RAX == 37;
        break;
      }
      if (!Legacy ||
          (Reason != VMX_REASON_EPT_VIOLATION && Reason != VMX_REASON_IRQ))
        return 1;
    }
    if (!Completed)
      return 1;
  }
  return 0;
}
#endif
