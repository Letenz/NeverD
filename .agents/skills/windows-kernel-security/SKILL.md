---
name: windows-kernel-security
description: Analyze Windows kernel security mechanisms used by drivers and game protection. Use for callbacks, IRQL, kernel memory, PatchGuard, DSE, HVCI, or driver trust; use dma-attack-techniques for PCIe DMA.
---

# Windows kernel security

Use this skill for authorized Windows kernel and defensive game-security work. Match implementation details to the target Windows version and verify undocumented structures rather than treating historical layouts as stable.

## Topic routing

Read only the reference that matches the task:

- [Kernel foundations](references/kernel-foundations.md) for objects, symbols, PatchGuard, DSE, VBS, HVCI, and Secure Boot.
- [Drivers and observation](references/drivers-and-observation.md) for callbacks, driver structure, hooking, APCs, and ETW.
- [Memory and forensics](references/memory-and-forensics.md) for segment heap, pool artifacts, virtual or physical memory, and MDLs.
- [Threats and virtualization](references/threats-and-virtualization.md) for vulnerable drivers, boot threats, PatchGuard research, and hypervisor defenses.
- [Resource map](references/resources.md) for tools and upstream references.

Use `anti-cheat-systems` for product-level detection architecture and `dma-attack-techniques` for PCIe/IOMMU threat modeling. Separate documented APIs from version-specific internals and label unsupported techniques accordingly.
