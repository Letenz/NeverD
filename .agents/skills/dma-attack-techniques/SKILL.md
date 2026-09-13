---
name: dma-attack-techniques
description: Analyze PCIe DMA threats, FPGA device behavior, and IOMMU defenses. Use for DMA-specific threat modeling or detection; use windows-kernel-security for kernel mechanisms without a DMA boundary.
---

# DMA attack techniques

Use this skill for defensive research into hostile PCIe devices and DMA-capable hardware. Keep analysis within the user's authorized security scope.

## Topic routing

Read only the reference that matches the task:

- [PCIe and FPGA](references/pcie-and-fpga.md) for TLPs, configuration space, pcileech, FPGA constraints, and device emulation.
- [IOMMU and virtualization](references/iommu-and-virtualization.md) for VT-d/AMD-Vi, ACS, ATS/PASID, domain assignment, hypervisors, and split address spaces.
- [Detection and forensics](references/detection-and-forensics.md) for device fingerprinting, containment, telemetry, evidence capture, and defensive synthesis.
- [Platforms and memory access](references/platforms-and-access.md) for Thunderbolt/USB4 and physical-memory access mechanics.
- [Resource map](references/resources.md) when maintaining the source collection or locating upstream material.

For general anti-cheat architecture, use `anti-cheat-systems`. For Windows driver, callback, PatchGuard, or kernel-memory internals without a PCIe focus, use `windows-kernel-security`.

Prefer threat models, detection limits, and mitigations over operational bypass instructions. Clearly separate hardware capability, observed evidence, and inference.
