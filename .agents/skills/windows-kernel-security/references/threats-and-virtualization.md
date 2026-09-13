# Threats And Virtualization

## Vulnerable Driver Exploitation

### Common Vulnerability Types
- Arbitrary read/write primitives
- IOCTL handler vulnerabilities
- Pool overflow
- Use-after-free

### Notable Vulnerable Drivers
```
- gdrv.sys (Gigabyte)
- iqvw64e.sys (Intel)
- MsIo64.sys
- Mhyprot2.sys (Genshin Impact)
- dbutil_2_3.sys (Dell)
- RTCore64.sys (MSI)
- Capcom.sys
```

### Exploitation Steps
1. Load vulnerable signed driver
2. Trigger vulnerability
3. Achieve kernel read/write
4. Disable DSE or load unsigned driver
5. Execute arbitrary kernel code

## PatchGuard Bypass Techniques

### Timing-Based
- Predict PG timer
- Modify between checks

### Context Manipulation
- Exception handling
- DPC manipulation
- Thread context tampering

### Hypervisor-Based
- EPT manipulation
- Memory virtualization
- Intercept PG checks

## EFI/Boot-Time Threats

### EFI Driver Cross-Reference
```
The README's > EFI Driver subcategory (under Cheat) contains 30+ projects:
- EFI bootkit frameworks: UEFI DXE drivers that persist across boots
- Boot-time memory mappers: inject code before Windows kernel initializes
- ExitBootServices hooks: intercept Windows boot handoff
- EFI runtime service abuse: GetVariable/SetVariable for kernel ↔ EFI comm

See also: game-hacking skill for EFI cheat workflows
```

### Boot-Time Access
```
- EFI runtime services persist after ExitBootServices
- DXE (Driver Execution Environment) phase: full hardware access
- Pre-kernel execution: no DSE, no PatchGuard, no HVCI enforcement
- Secure Boot is the primary mitigation (firmware signature verification)
```

### Memory Access
```
- GetVariable/SetVariable: pass data between EFI and OS runtime
- Runtime memory mapping via EFI memory map
- Physical memory access before Windows memory manager initializes
- ACPI table injection for persistent low-level modifications
```

## Hypervisor Development

### Hypervisor Types
```
Type 1 (bare-metal):
- Runs directly on hardware
- Examples: VMware ESXi, Microsoft Hyper-V, Xen
- Used for VBS, production security enforcement

Type 2 (hosted):
- Runs on top of a host operating system
- Examples: Oracle VirtualBox, VMware Workstation
- Common for research, development, and testing
```

### Hardware Virtualization Platforms
```
Intel VT-x:
- Introduced 2005, widely supported on modern Intel CPUs
- Foundation for VMCS, EPT, VM exits

AMD-V (SVM):
- AMD's counterpart to VT-x, also introduced 2005
- VMCB structure, NPT (Nested Page Tables)

ARM Virtualization Extensions:
- EL2 (hypervisor mode) and stage-2 memory translation
- Used on ARM platforms for mobile and embedded security
```

### Intel VT-x Core Concepts

#### VMCS (Virtual Machine Control Structure)
```
Central data structure for Intel VT-x:
- Describes guest state, host state, and virtualization controls
- Tells the processor:
  - What state to restore on VM entry
  - What state to save on VM exit
  - Which events transfer control back to the hypervisor

Guest/Host State Areas:
- Control registers (CR0, CR3, CR4)
- Segment registers (CS, SS, DS, ES, FS, GS)
- Debug registers (DR7 — hardware breakpoints)
- Descriptor-table registers (GDTR, IDTR)
- Key fields:
  - CR3: root of guest page tables, central to virtual memory
  - GDTR/IDTR: Global/Interrupt Descriptor Tables
  - CS/SS: code and stack segments
  - DR7: hardware breakpoint control

Control Fields:
- Pin-based controls
- Primary processor-based controls
- Secondary processor-based controls
- Events that cause VM exits:
  - CPUID interception
  - INVLPG interception
  - Control-register access
  - EPT violations
  - MSR access
```

#### EPT (Extended Page Tables)
```
Intel's implementation of SLAT (Second-Level Address Translation):
- Gives the hypervisor independent control over guest memory
- Two-stage address translation pipeline:
  1. GVA → GPA: Guest Virtual → Guest Physical (via guest page tables, rooted at CR3)
  2. GPA → HPA: Guest Physical → Host Physical (via EPT, rooted at EPTP in VMCS)
- Guest believes it owns its own memory mappings
- Hypervisor has a second, independent layer controlling:
  - What physical memory is reachable
  - What permissions apply (read/write/execute)

EPT Hierarchy:
- PML4 → PDPT → PD → PT (4-level page table)
- Each entry carries read/write/execute permissions
- EPT violations trigger VM exits when access permissions are violated

Page Table Entries (PTE):
- Maps GVA to GPA
- Carries: read/write, supervisor-only, caching, software-defined bits
- Guest PTEs and EPT serve different roles:
  - Guest PTE: controls guest's view of memory
  - EPT: controls hypervisor's view of the guest
```

#### VM Exits & VMCALL
```
VM Exits:
- Occur when configured events happen in the guest
- Triggers: CPUID, CR access, I/O instructions, EPT violations, MSR access
- On exit: processor saves guest state (per VMCS), restores host state,
  records exit reason for hypervisor handler

VMCALL:
- Guest intentionally transfers control to hypervisor
- Similar in concept to a system call (guest → hypervisor)
- Used for guest-hypervisor communication interfaces
```

#### Nested Virtualization
```
- Running a hypervisor inside a VM managed by another hypervisor
- Useful for research, testing, and development
- Adds complexity: multiple layers participate in the same virtualization flow
- Relevant for testing hypervisor-based defense under VMware/Hyper-V
```

### AMD-V (SVM)
- VMCB (Virtual Machine Control Block) structure
- NPT (Nested Page Tables) — AMD's SLAT equivalent
- SVM operations (VMRUN, VMSAVE, VMLOAD)

### Use Cases
- Memory hiding
- Syscall interception
- Security monitoring
- Anti-cheat evasion
- EPT-based memory protection and introspection

### Windows Hypervisor Platform (WHP) API
```
User-mode hypervisor interface (Windows 10+):
- WHvCreatePartition / WHvSetupPartition: create VM partition
- WHvCreateVirtualProcessor: add vCPU
- WHvMapGpaRange: map host memory into guest physical address space
- WHvRunVirtualProcessor: enter guest execution, blocks until VM exit
- WHvGetVirtualProcessorRegisters / Set: read/write guest CPU state

Key capability:
- Enables hypervisor-assisted analysis from user mode (no kernel driver)
- Page-level trap handling: set R/W/X permissions per guest page
- VM exit reasons: memory access violation, CPUID, MSR access, I/O port, syscall
- Deterministic execution: host controls all guest state and memory

Prerequisites:
- Enable Windows features: Microsoft-Hyper-V-Hypervisor + HypervisorPlatform
- Hardware: VT-x or AMD-V support
- Note: WHP coexists with Hyper-V but conflicts with some third-party hypervisors

See also: reverse-engineering skill → User-Mode Hypervisor-Assisted Tracing
for analysis workflows built on WHP
```

## Hypervisor-Based Defense

### Concept
```
- Security approach using virtualization primitives to enforce protections
  from a higher privilege level than the guest kernel
- Moves security decisions into an isolated execution environment
  that a compromised kernel cannot easily tamper with
- Present across major OS platforms:
  - Windows: Virtualization-Based Security (VBS)
  - Android: Android Virtualization Framework (AVF)
  - Apple: Secure execution environments, hardware-backed isolation
```

### EPT Hooks as Defensive Primitives
```
Mechanism:
- Instead of patching the guest kernel, modify EPT permissions
- Specific memory accesses trigger EPT violations → VM exit
- Hypervisor inspects the access and decides: allow, deny, or log

Example: Watching writes to a sensitive region
1. Remove write permission from the EPT entry for target region
2. Guest runs normally until it attempts a write to that region
3. EPT violation → VM exit → hypervisor receives control
4. Hypervisor evaluates context:
   - Which module performed the access
   - What memory was touched
   - Whether the access is authorized
5. Decision: allow write, deny and return, or log and continue

Advantages over traditional kernel hooks:
- Operate outside the guest OS
- Remain effective even if guest kernel is compromised
- Transparent to guest-level detection
- Cannot be removed by kernel-level rootkits
```

### Protectable Assets via EPT
```
- Executable pages of EPP (Endpoint Protection Platform) drivers
  → Prevents silent patching of security software
- ETW-related structures
  → Unauthorized writes fault into hypervisor
- Callback/callout/routine lists (PsSetCreateProcessNotifyRoutine, etc.)
  → Write authorization moved outside the guest kernel
- Critical kernel data structures
  → PatchGuard-protected regions, SSDT, IDT
```

### Threat Model for Hypervisor Defense
```
Assumes kernel compromise has already happened:
- Attacker has kernel code execution
- Attacker can load vulnerable drivers (BYOVD)
- Attacker can modify kernel memory
- Traditional kernel-resident protections are untrustworthy

Hypervisor advantage:
- Sits above the guest kernel in privilege hierarchy
- Enforces policies from a higher privilege layer
- Kernel-level rootkits cannot disable hypervisor-level enforcement
```

### Attack Scenario: BYOVD vs EPT Protection
```
Without hypervisor defense:
1. Attacker loads vulnerable signed driver
2. Gains kernel R/W primitives
3. Patches callback list to remove EPP callbacks
4. EPP is blinded — attacker operates undetected

With EPT-based defense:
1. Attacker loads vulnerable signed driver
2. Gains kernel R/W primitives
3. Attempts to patch callback list
4. EPT violation triggers VM exit
5. Hypervisor catches the write, evaluates context
6. Write is denied — callback list remains intact
```
