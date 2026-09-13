# Kernel Foundations

## Core Kernel Concepts

### Important Structures
- EPROCESS / ETHREAD
- KTHREAD / KAPC / KAPC_STATE
- MMVAD / VAD tree nodes
- PEB / TEB
- DRIVER_OBJECT
- DEVICE_OBJECT
- IRP (I/O Request Packet)

### Key Tables
- SSDT (System Service Descriptor Table)
- IDT (Interrupt Descriptor Table)
- GDT (Global Descriptor Table)
- PspCidTable (Process/Thread handle table)
- PiDDBCacheTable / MmUnloadedDrivers / PoolBigPageTable

## User-Mode Kernel Symbol Walking

### Methodology
```
- Load local ntoskrnl image (typically C:\Windows\System32\ntoskrnl.exe)
- Use dbghelp + symbol server path (srv*cache*https://msdl.microsoft.com/download/symbols)
  to resolve exported symbol RVAs and type information
- Build structure-aware field lookup:
  - Query field offset directly (e.g., _EPROCESS.Token)
  - Enumerate all members of a target struct (_TOKEN, _EPROCESS, etc.)
  - Search a field name across all known structs (useful when parent type is unknown)
- Keep symbol path configurable for offline/private symbol repositories
```

### Why It Matters in Game Security
```
- Reduces hardcoded-offset fragility across Windows builds
- Helps map kernel object layouts used by anti-cheat and drivers
- Supports rapid adaptation when anti-cheat-relevant fields shift
  (EPROCESS, ETHREAD, token/handle/security-related members)
```

### Gadget Scanning Workflow
```
- Map executable sections of ntoskrnl image in user mode
- Scan for short control-flow gadgets (e.g., pop rcx ; ret, jmp rax)
- Use as a research primitive for:
  - ROP chain feasibility analysis
  - Kernel exploit mitigation evaluation
  - Anti-cheat hardening review against gadget-dependent attack paths
```

## Security Features

### PatchGuard (Kernel Patch Protection)
```
- Protects critical kernel structures
- Periodic verification checks
- BSOD on tampering detection
- Multiple trigger mechanisms
```

### Driver Signature Enforcement (DSE)
```
- Requires signed drivers
- CI.dll verification
- Test signing mode
- WHQL certification
```

### Virtualization-Based Security (VBS)
```
Architecture:
- Uses the Windows hypervisor to create an isolated execution environment
- Splits the system into Virtual Trust Levels (VTLs)
  - VTL0: Normal world — standard Windows kernel and user-mode processes
  - VTL1: Secure world — Secure Kernel, security policy enforcement
- Even if VTL0 kernel is fully compromised, VTL1 remains isolated
- Three main buckets:
  - Memory-protection features (HVCI)
  - Virtual Trust Levels (VTL0/VTL1 separation)
  - VBS enclaves (isolated execution for selected workloads)
```

### Hypervisor-Enforced Code Integrity (HVCI)
```
- Also known as Memory Integrity
- Ensures only trusted, validated code executes in kernel mode
- Combines Windows hypervisor + Secure Kernel (VTL1) for enforcement
- Key mechanism: W→X transition restriction
  - Executable kernel pages cannot become writable
  - Writable pages cannot become executable without re-validation
- Enforcement pipeline:
  - Code integrity policy defines what is trusted
  - Hypervisor memory enforcement via second-stage address translation (EPT/SLAT)
  - Once a kernel page is validated, strict execution rules are enforced
- Driver compatibility requirements: drivers must be HVCI-compatible
```

### Secure Boot
```
- UEFI-based boot verification
- Boot loader chain validation
- Kernel signature checks
- DBX (forbidden signatures)
- Foundation for attestation and DMA-hardening assumptions
```
