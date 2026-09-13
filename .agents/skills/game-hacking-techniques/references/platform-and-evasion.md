# Platform And Evasion

## EFI/UEFI Cheats

### Boot-Time Loading
```
- EFI manual map: load unsigned driver payload during UEFI boot phase
- ExitBootServices hook: intercept Windows boot to inject kernel code
- Runtime DXE drivers: persist across OS boot via EFI runtime services
- GetVariable/SetVariable: communicate between EFI and OS runtime
```

### EFI-Based Memory Access
```
- Map physical memory via EFI runtime services
- Bypass DSE entirely (code runs before Windows kernel loads)
- Survive Secure Boot if firmware is compromised or test-signed
- Combine with DMA for maximum stealth
```

### Detection Challenges
```
- No driver load event (PsSetLoadImageNotifyRoutine never fires)
- Not visible in MmUnloadedDrivers or PiDDBCacheTable
- Secure Boot + TPM attestation is primary defense
- Firmware integrity measurement (UEFI capsule verification)
```

## HWID Spoofing

### Targets
```
- Disk serial: IOCTL_STORAGE_QUERY_PROPERTY, SMART data
- NIC MAC address: NDIS OID_802_3_PERMANENT_ADDRESS
- SMBIOS: motherboard serial, system UUID, BIOS vendor
- GPU serial: registry-based or NVAPI/ADL queries
- Monitor EDID: display serial number
- Volume serial: NtQueryVolumeInformationFile
- TPM EK: Endorsement Key fingerprint
```

### Techniques
```
- Disk filter driver: intercept IOCTL and replace serial in response
- Registry value spoofing: modify cached hardware IDs
- SMBIOS table patching: modify raw SMBIOS memory region
- NIC driver hook: replace MAC in NDIS miniport response
- Full HWID spoofer: coordinated spoofing across all identifiers
```

## Stack Spoofing

### Return Address Spoofing
```
- Replace return address on stack before API call
- Restore original after call returns
- Evades stack-walk-based detection (RtlWalkFrameChain)
- Techniques: JMP RBX gadget, synthetic frames, fiber-based
```

### Call Stack Reconstruction
```
- Build fake but plausible call stack frames
- Match expected module return addresses (ntdll, kernel32)
- Evade NtQueryInformationThread stack inspection
- Tools: SpoofCallStack, Vulcan, CallStackSpoofer
```

### Detection & Evasion
```
- Anti-cheat walks thread stacks looking for non-module returns
- Stack unwinding via .pdata / UNWIND_INFO validation
- Spoofed stacks must pass RtlVirtualUnwind consistency checks
```

## Anti-Detection Techniques

### Code Protection
- Polymorphic code
- Code virtualization
- Anti-dump techniques
- String encryption

### Runtime Evasion
- Stack spoofing
- Return address manipulation
- Thread context hiding
- Module concealment
