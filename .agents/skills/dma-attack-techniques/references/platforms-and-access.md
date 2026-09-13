# Platforms And Access

## Thunderbolt / USB4 DMA

### Attack Surface
```
- Thunderbolt 1-4 / USB4 provide direct PCIe tunneling
- Hot-plug capable: device can be attached at runtime
- Pre-boot DMA: device has memory access before OS loads
- Thunderbolt Security Levels:
  - SL0 (None): no security, legacy mode
  - SL1 (User Auth): user must approve new devices
  - SL2 (Secure Connect): device must match previously approved UUID
  - SL3 (No PCIe tunneling): completely disables DMA
```

### Thunderbolt-Specific Attacks
```
- Thunderclap: malicious Thunderbolt peripherals bypass IOMMU
- Device re-identification: change UUID to bypass SL2
- OS-level Thunderbolt driver vulnerabilities
- PCIe tunneling through USB4 hubs
```

### Defensive Measures
```
- Kernel DMA Protection (Windows 10 1803+): automatic IOMMU for hot-plug
- Thunderbolt firmware verification
- Platform-level: BIOS setting to disable Thunderbolt PCIe tunneling
- macOS: T2 chip enforces DMA restrictions on Thunderbolt ports
```

## Memory Access Techniques

### Physical Memory Reading
```c
// Typical pcileech API usage
HANDLE hDevice;
BYTE buffer[0x1000];
pcileech_read_phys(hDevice, physAddr, buffer, sizeof(buffer));
```

### Virtual Address Translation
```c
// Walk page tables: PML4 → PDPT → PD → PT → Physical
PHYSICAL_ADDRESS TranslateVA(UINT64 cr3, UINT64 virtualAddr) {
    UINT64 pml4e = ReadPhys(cr3 + PML4_INDEX(virtualAddr) * 8);
    UINT64 pdpte = ReadPhys(PFN(pml4e) + PDPT_INDEX(virtualAddr) * 8);
    UINT64 pde = ReadPhys(PFN(pdpte) + PD_INDEX(virtualAddr) * 8);
    UINT64 pte = ReadPhys(PFN(pde) + PT_INDEX(virtualAddr) * 8);
    return PFN(pte) + PAGE_OFFSET(virtualAddr);
}
```

### DTB (Directory Table Base) Finding
```
- Scan physical memory for valid CR3 values
- Look for kernel structures
- Use signature scanning
- Validate page table entries
```
