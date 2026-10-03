//===- probe_hvf_cr8.c - Isolated Intel CR8 virtualization diagnosis ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include <Hypervisor/Hypervisor.h>
#include <Hypervisor/hv_vmx.h>
#include <cpuid.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *Variant;
static int variant(const char *Name) { return strcmp(Variant, Name) == 0; }
static int report(const char *Operation, hv_return_t Status) {
  printf("%s: 0x%08x\n", Operation, (uint32_t)Status);
  fflush(stdout);
  return Status != HV_SUCCESS;
}
static int control(hv_vcpuid_t CPU, uint32_t Field, uint64_t Required) {
  uint64_t Must = 0, May = 0;
  if (report("VMCS capabilities",
             hv_vmx_vcpu_get_cap_write_vmcs(CPU, Field, &Must, &May)))
    return 1;
  printf("VMCS 0x%x: must=0x%llx may=0x%llx required=0x%llx\n", Field,
         (unsigned long long)Must, (unsigned long long)May,
         (unsigned long long)Required);
  if (Required & ~May)
    return 1;
  if (report("VMCS control",
             hv_vmx_vcpu_write_vmcs(CPU, Field, Must | Required)))
    return 1;
  uint64_t Actual = 0;
  if (report("VMCS control readback",
             hv_vmx_vcpu_read_vmcs(CPU, Field, &Actual)))
    return 1;
  printf("VMCS readback 0x%x=0x%llx\n", Field, (unsigned long long)Actual);
  return 0;
}
static int execute(hv_vcpuid_t CPU, void **Backing) {
  if (report("bind kernel GS",
             hv_vcpu_enable_managed_msr(CPU, HV_MSR_IA32_KERNEL_GS_BASE, 1)) ||
      report("initialize kernel GS",
             hv_vcpu_write_msr(CPU, HV_MSR_IA32_KERNEL_GS_BASE, 0)))
    return 1;
  const size_t Bytes = 65536;
  if (posix_memalign(Backing, (size_t)sysconf(_SC_PAGESIZE), Bytes))
    return 1;
  memset(*Backing, 0, Bytes);
  unsigned char *RAM = *Backing;
  ((uint64_t *)(RAM + 0x1000))[0] = 0x2003;
  ((uint64_t *)(RAM + 0x2000))[0] = 0x3003;
  ((uint64_t *)(RAM + 0x3000))[0] = 0x0083;
  // Original MOV RAX,CR8; HLT. Both exits must preserve the real read result.
  const unsigned char Program[] = {0x44, 0x0f, 0x20, 0xc0, 0xf4};
  memcpy(RAM + 0x4000, Program, sizeof(Program));
  const unsigned char WriteCR8[] = {0x44, 0x0f, 0x22, 0xc3};
  memcpy(RAM + 0x4100, WriteCR8, sizeof(WriteCR8));
  if (report("map",
             hv_vm_map(RAM, 0, Bytes,
                       HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC)))
    return 1;
  if ((variant("apic-address") || variant("apic-access")) &&
      report("APIC address", hv_vmx_vcpu_set_apic_address(CPU, 0x100000)))
    return 1;
  const uint64_t Primary =
      CPU_BASED_SECONDARY_CTLS | CPU_BASED_HLT |
      (variant("cr8-exits") ? CPU_BASED_CR8_LOAD | CPU_BASED_CR8_STORE
                            : CPU_BASED_TPR_SHADOW);
  if (control(CPU, VMCS_CTRL_PIN_BASED, PIN_BASED_INTR | PIN_BASED_NMI) ||
      control(CPU, VMCS_CTRL_CPU_BASED, Primary) ||
      control(CPU, VMCS_CTRL_CPU_BASED2,
              CPU_BASED2_EPT |
                  (variant("apic-access") ? CPU_BASED2_VIRTUAL_APIC : 0)) ||
      control(CPU, VMCS_CTRL_VMENTRY_CONTROLS, VMENTRY_GUEST_IA32E) ||
      control(CPU, VMCS_GUEST_CR0, 0x80010033) ||
      control(CPU, VMCS_GUEST_CR4, variant("xsave") ? 0x42620 : 0x2620) ||
      control(CPU, VMCS_CTRL_CR0_MASK, 0) ||
      control(CPU, VMCS_CTRL_CR4_MASK, 0x2000))
    return 1;
  const struct {
    uint32_t Field;
    uint64_t Value;
  } Fields[] = {{VMCS_CTRL_EXC_BITMAP, UINT32_MAX},
                {VMCS_CTRL_VMENTRY_IRQ_INFO, 0},
                {VMCS_CTRL_TPR_THRESHOLD, 0},
                {VMCS_GUEST_CR3, 0x1000},
                {VMCS_GUEST_IA32_EFER, 0xd00},
                {VMCS_CTRL_CR0_SHADOW, 0x80010033},
                {VMCS_CTRL_CR4_SHADOW, 0x620},
                {VMCS_GUEST_ACTIVITY_STATE, 0},
                {VMCS_GUEST_INTERRUPTIBILITY, 0},
                {VMCS_GUEST_DEBUG_EXC, 0},
                {VMCS_GUEST_DR7, 0x400},
                {VMCS_GUEST_GDTR_BASE, 0},
                {VMCS_GUEST_GDTR_LIMIT, 0},
                {VMCS_GUEST_IDTR_BASE, 0},
                {VMCS_GUEST_IDTR_LIMIT, 0},
                {VMCS_GUEST_LDTR, 0},
                {VMCS_GUEST_LDTR_BASE, 0},
                {VMCS_GUEST_LDTR_LIMIT, 0},
                {VMCS_GUEST_LDTR_AR, 0x10000},
                {VMCS_GUEST_TR, 0x28},
                {VMCS_GUEST_TR_BASE, 0},
                {VMCS_GUEST_TR_LIMIT, 0x67},
                {VMCS_GUEST_TR_AR, 0x8b}};
  for (unsigned I = 0; I < sizeof(Fields) / sizeof(Fields[0]); ++I)
    if (report("guest VMCS",
               hv_vmx_vcpu_write_vmcs(CPU, Fields[I].Field, Fields[I].Value)))
      return 1;
  for (unsigned I = 0; I < 6; ++I) {
    const int Code = I == 1;
    if (report("selector", hv_vmx_vcpu_write_vmcs(CPU, VMCS_GUEST_ES + 2 * I,
                                                  Code ? 8 : 16)) ||
        report("base",
               hv_vmx_vcpu_write_vmcs(CPU, VMCS_GUEST_ES_BASE + 2 * I, 0)) ||
        report("limit", hv_vmx_vcpu_write_vmcs(CPU, VMCS_GUEST_ES_LIMIT + 2 * I,
                                               UINT32_MAX)) ||
        report("access", hv_vmx_vcpu_write_vmcs(CPU, VMCS_GUEST_ES_AR + 2 * I,
                                                Code ? 0xa09b : 0xc093)))
      return 1;
  }
  if (variant("xsave")) {
    unsigned A, B, C, D;
    if (!__get_cpuid_count(0xd, 0, &A, &B, &C, &D) || B < 576 || B > 32768)
      return 1;
    if (report("XCR0", hv_vcpu_write_register(CPU, HV_X86_XCR0, 3)) ||
        report("read FP", hv_vcpu_read_fpstate(CPU, RAM + 32768, B)) ||
        report("write FP", hv_vcpu_write_fpstate(CPU, RAM + 32768, B)))
      return 1;
  }
  const unsigned Levels[] = {0, 1, 3, 15};
  int Mismatch = 0;
  for (unsigned L = 0; L < sizeof(Levels) / sizeof(Levels[0]); ++L) {
    for (unsigned Step = 0; Step < 2; ++Step) {
      printf("variant=%s CR8=%u step=%u\n", Variant, Levels[L], Step);
      if (variant("guest-write")) {
        if (control(CPU, VMCS_CTRL_CPU_BASED, Primary | CPU_BASED_MTF) ||
            report("write CR8 RIP",
                   hv_vcpu_write_register(CPU, HV_X86_RIP, 0x4100)) ||
            report("write CR8 RBX",
                   hv_vcpu_write_register(CPU, HV_X86_RBX, Levels[L])) ||
            report("write CR8 flags",
                   hv_vcpu_write_register(CPU, HV_X86_RFLAGS, 2)))
          return 1;
        for (unsigned Attempt = 0; Attempt < 32; ++Attempt) {
          uint64_t Reason = 0, TPR = 0;
          if (report("guest write CR8 run",
                     hv_vcpu_run_until(CPU, HV_DEADLINE_FOREVER)) ||
              report(
                  "guest write CR8 reason",
                  hv_vmx_vcpu_read_vmcs(CPU, VMCS_RO_EXIT_REASON, &Reason)) ||
              report("guest write CR8 TPR",
                     hv_vcpu_read_register(CPU, HV_X86_TPR, &TPR)))
            return 1;
          printf("guest write CR8 level=%u reason=%llu tpr=0x%llx\n", Levels[L],
                 (unsigned long long)Reason, (unsigned long long)TPR);
          if (Reason == VMX_REASON_MTF)
            break;
          if (Reason != VMX_REASON_IRQ || Attempt == 31)
            return 1;
        }
      }
      if (control(CPU, VMCS_CTRL_CPU_BASED,
                  Primary | (Step ? CPU_BASED_MTF : 0)) ||
          report("RIP", hv_vcpu_write_register(CPU, HV_X86_RIP, 0x4000)) ||
          report("RAX", hv_vcpu_write_register(CPU, HV_X86_RAX, UINT64_MAX)) ||
          report("RFLAGS", hv_vcpu_write_register(CPU, HV_X86_RFLAGS, 2)))
        return 1;
      if (variant("accelerated-apic")) {
        if (__builtin_available(macOS 12.0, *)) {
          bool NoSideEffect = false;
          if (report(
                  "APIC TPR write",
                  hv_vcpu_apic_write(CPU, 0x80, Levels[L] << 4, &NoSideEffect)))
            return 1;
          printf("APIC no_side_effect=%u\n", NoSideEffect);
        } else
          return 1;
      } else if (!variant("guest-write") &&
                 report("TPR write",
                        hv_vcpu_write_register(
                            CPU, HV_X86_TPR,
                            variant("unshifted") ? Levels[L] : Levels[L] << 4)))
        return 1;
      for (unsigned Attempt = 0; Attempt < 32; ++Attempt) {
        const hv_return_t Status =
            variant("legacy") ? hv_vcpu_run(CPU)
                              : hv_vcpu_run_until(CPU, HV_DEADLINE_FOREVER);
        uint64_t Reason = 0, RAX = 0, TPR = 0, RIP = 0;
        const int Failed =
            report("exit reason",
                   hv_vmx_vcpu_read_vmcs(CPU, VMCS_RO_EXIT_REASON, &Reason)) |
            report("RAX read", hv_vcpu_read_register(CPU, HV_X86_RAX, &RAX)) |
            report("TPR read", hv_vcpu_read_register(CPU, HV_X86_TPR, &TPR)) |
            report("RIP read", hv_vcpu_read_register(CPU, HV_X86_RIP, &RIP));
        printf("result variant=%s level=%u step=%u status=0x%x reason=%llu "
               "rax=0x%llx tpr=0x%llx rip=0x%llx\n",
               Variant, Levels[L], Step, (uint32_t)Status,
               (unsigned long long)Reason, (unsigned long long)RAX,
               (unsigned long long)TPR, (unsigned long long)RIP);
        fflush(stdout);
        if (Status || Failed)
          return 1;
        if (Reason == (Step ? VMX_REASON_MTF : VMX_REASON_HLT)) {
          if (RAX != Levels[L])
            Mismatch = 1;
          break;
        }
        if (Attempt == 31 ||
            (Reason != VMX_REASON_IRQ &&
             !(variant("legacy") && Reason == VMX_REASON_EPT_VIOLATION)))
          return 1;
      }
    }
  }
  return Mismatch;
}
int main(int Argc, char **Argv) {
  if (Argc != 2)
    return 2;
  Variant = Argv[1];
  const int Accelerated =
      variant("accelerated-apic") || variant("accelerated-register");
  if (report("VM create",
             hv_vm_create(Accelerated ? HV_VM_ACCEL_APIC : HV_VM_DEFAULT)))
    return 1;
  hv_vcpuid_t CPU;
  const hv_return_t Created = hv_vcpu_create(&CPU, HV_VCPU_DEFAULT);
  int Failed = report("CPU create", Created);
  void *Backing = NULL;
  if (!Failed)
    Failed = execute(CPU, &Backing);
  if (Created == HV_SUCCESS)
    Failed |= report("CPU destroy", hv_vcpu_destroy(CPU));
  const hv_return_t Destroyed = hv_vm_destroy();
  Failed |= report("VM destroy", Destroyed);
  if (Destroyed == HV_SUCCESS)
    free(Backing);
  return Failed;
}
