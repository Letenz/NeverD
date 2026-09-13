# Detection And Forensics

## Detection at the PCIe Layer

### Configuration Integrity
```
- VID/DID/SVID/SDID against known-real-silicon list
- Capability-chain walk: DWord-aligned Next pointers, no overlaps, no cycles
- Signature-residue scanning: Xilinx 7-series default byte patterns at
  known relative offsets (Device Capabilities field bits, reserved bits,
  VSEC vendor IDs)
- Capability presence consistency: donor model's known caps must all be present
- BAR mask verification: write 0xFFFFFFFF, compare size mask against donor
```

### BAR Memory Read Probing
```
Send Memory Read TLPs to BAR ranges, validate responses by donor class:

NIC donor BAR0: register layout with receive/transmit ring descriptors,
  interrupt mask, link status. Offset 0x00 returns specific bit pattern.

NVMe donor BAR0: NVMe controller registers — CAP (MQES, DSTRD,
  MPSMIN/MPSMAX), VS, CC, CSTS, AQA, ASQ/ACQ, doorbells at 0x1000.

USB XHCI donor BAR0: Capability Registers (CAPLENGTH, HCSPARAMS, HCCPARAMS).

zerowrite4k returns all-zeros; loopaddr echoes address. Both are
trivially distinguishable from real content.
Tier-4 firmwares implement donor-class responders but usually only
cover registers checked at probe time, leaving others divergent.
```

### R/W Consistency Probing
```
- Command Register: toggle Memory Space, I/O Space, Bus Master Enable
- Device Control: change MPS, MRRS, Error Enables
- MSI Control: toggle Enable, change Multiple Message Enable
- Walk every W1C bit (Status, AER Status): write 1s, confirm clear
- Walk reserved bits: write 1s, confirm read-back as 0
- Per-register writable masks must match donor

Tier-2 (no overlay) fails immediately.
Tier-3 (single global mask) fails on W1C and reserved-bit cases.
```

### LTSSM and Link-State Validation
```
Sample PCIe Express Capability Link Status over time:
- Negotiated Width (Link Status[9:4]): consistent with donor deployment
  and FPGA hard block capability
- Current Link Speed (Link Status[3:0]): track slot's actual speed
- Gen4 x8 capability but Gen2 x1 Link Status = contradiction in one read
- DLL Active (Link Status[13]): should be 1 during operation
- Slot Clock Config (Link Status[12]): match real common-clock state
```

### ASPM Behavioral Validation
```
Real devices claiming ASPM exhibit characteristic L0 ↔ L1 transitions.

Spoofed device anomalies:
- Claims ASPM capability but never transitions out of L0
- Transitions with exit latency inconsistent with claimed value
- Never reaches L1.1 / L1.2 when donor and platform both support

Sample Link Status "DLL Active" bit over time + PMC counters.
```

### AER Baselining
```
- Departure from donor baseline: per-silicon correctable-error footprint
  should be stable. Implausibly clean (zero correctables when donor
  normally produces Bad TLP / Replay Timer Timeout) is anomalous.
- Implausible Header Log content (default/zeroed values)
- Inconsistent UR/CA responses to probes of unimplemented offsets
```

### Completion Latency Fingerprinting
```
Real silicon: completion latency shaped by DRAM contention,
internal arbiters, PCIe pipeline depth → heavy-tailed distributions.

BRAM-backed emulators: fixed FPGA clock cycles + PCIe transit
→ much lower variance, even if mean is similar.

Detection signal is distribution shape, not absolute mean.

Statistical methods:
- Kolmogorov–Smirnov test: compare empirical CDFs
- Hill estimator: estimate tail index (real silicon has non-trivial tail;
  emulated firmware without stochastic jitter has no tail)
- Anderson-Darling test: sensitive to tail differences

Collect N latency samples (Memory Reads to BAR), compare against
per-donor reference distribution, flag devices deviating
beyond per-test-statistic threshold.

Tier-5 firmwares add LFSR-based jitter generators, but matching
real distribution shape (mean, variance, tail index, mode count)
requires modeling donor's DRAM access pattern.
```

### MSI/MSI-X Behavioral Validation
```
A device with MSI Enable, Address/Data programmed, and attached driver
should produce interrupts:

- Zero interrupts when driver should exercise device = anomalous
- Implausibly uniform arrival times (exact 60 Hz heartbeat)
  = timer-driven generator, not event-driven
- Implausibly bursty patterns not matching donor class

Monitor via OS interrupt accounting, ETW/performance telemetry,
driver counters, kernel instrumentation.
```

### Cheat-Phase Access Pattern Recognition
```
Two distinct patterns:

Development phase:
  Slow, broad scanning, signature search, MemProcFS walking.
  Rare during live competitive play.

Execution phase:
  Narrow, periodic reads (60–240 Hz) of small offset set
  (player positions, entity arrays, view matrices).

Execution phase statistical signature:
  High temporal periodicity, low address-space breadth,
  alignment to game-frame intervals.

Distinguishing features:
- Fano factor
- Autocorrelation at frame intervals
- Address-space coverage entropy

Honeypot regions complement this when combined with:
  IOMMU denial/fault logging, hypervisor-managed protected pages,
  device-domain sandboxing, decoy IOVA mappings, or server-side
  behavioral traps tied to fake game objects.
```

### Hot-Plug and Link Disruption Events
```
Firmware swaps and donor reconfigurations require link renegotiation.
Three observable signals:

1. AER Surprise Down — uncorrectable fatal error logged when link
   drops unexpectedly mid-session.

2. Slot Status hot-plug events — Presence Detect Changed,
   Data Link Layer State Changed, MRL Sensor Changed.
   Register via IoRegisterPlugPlayNotification with
   EventCategoryTargetDeviceChange or EventCategoryDeviceInterfaceChange.

3. LTSSM re-training cycles — Link Training bit toggling,
   transient Recovery state.

A device that surprise-downs then reappears with subtly different
Config Space (adjusted MSI layout, different latency profile,
different VID/DID) = fingerprint of mid-session firmware reload.
```

## Detection at the IOMMU Layer

### Fault-Rate Monitoring
```
Sample per-device fault counters from IOMMU fault-recording structure
(VT-d Fault Recording Registers, AMD-Vi Event Log) or from WHEA.

Pipeline:
1. Snapshot baseline at session start
2. Sample at intervals; compute per-device fault rate
3. Threshold-trigger additional PCIe probing on devices exceeding baseline
4. Aggregate evidence across signals before action
```

### Domain Assignment Audit
```
Walk IOMMU domain assignments for anomalies:
- Devices on passthrough/identity domains when strict mode is active
- Devices in unexpectedly large IOMMU groups (poor ACS topology)
- Multiple devices sharing Domain ID when they shouldn't
```

### ACS Topology Verification
```
Walk PCIe bridge topology between every endpoint and root complex.
For each bridge with ACS Capability:
- Verify Source Validation (SV) enabled
- Verify Translation Blocking (TB) enabled
- Verify P2P Request Redirect (RR) and Completion Redirect (CR) enabled

Bridges without ACS at all = isolation holes by topology.
Bridges with ACS Capability but Control bits not set = misconfiguration.
```

### IOMMU as Containment Primitive
```
Active containment when suspect device is identified:

1. IOMMU domain re-remapping:
   Reprogram device's domain to sandbox memory instead of revoking access.
   Cheat keeps "reading" but receives garbage data.

2. Bus Master Enable clearance:
   Toggle Command[2] to 0. Effective for tier-0 through tier-3.
   Cheats monitoring BME can race; may need repeated clearance.

3. Downstream Port Containment (DPC):
   When DPC is enabled on root port (Extended Cap ID 0x001D),
   triggers cause port to enter Contained state — all TLPs dropped,
   completions blocked, link logically isolated.
   Enforced at upstream port, no race against firmware-side BME restore.
   Not universal on all chipsets.

4. Anti-cheat-owned device domain:
   For device owned by AC driver, allocate and map only sandbox IOVAs,
   never expose game memory.

5. Hypervisor-integrated enforcement:
   Enforce policy above guest kernel by trapping IOMMU MMIO programming.
   Requires privileged platform integration.
```

## External Trust Anchors

### TPM 2.0
```
Hardware (or firmware-isolated) cryptoprocessor with:
- PCRs: extend-only registers, PCR[n] = SHA256(PCR[n] || new_value)
- Persistent keys: EK (manufacturer), SRK (provisioned), user-defined
- Hierarchy: Endorsement, Storage, Platform, Null
```

### PCR Allocation (Measured Boot)
```
PCR   Measured Content
0     SRTM / Core Root of Trust — UEFI firmware code
1     Platform configuration data — firmware variables
2     Option ROM code — third-party UEFI drivers
3     Option ROM configuration and data
4     IPL / boot manager binary (e.g., bootmgfw.efi)
5     IPL configuration — GPT/partition table, boot config
6     Manufacturer-specific / state-transition events
7     Secure Boot policy (PK, KEK, db, dbx)
8–15  OS-defined (BitLocker binds to PCR[11])
16    Debug
17–22 DRTM measurements (Secure Launch)
23    Application-defined
```

### Remote Attestation Cryptography
```
Trust property: compromised local kernel cannot forge PCR values.

Flow:
1. Server sends nonce
2. Client calls TPM2_Quote(AIK, PCR_selection, nonce)
   TPM computes PCR composite, builds TPMS_ATTEST, signs with AIK
3. Client sends Quote + AIK certificate chain
4. Verifier checks:
   - AIK signature valid
   - AIK certificate chains to trusted TPM manufacturer root
   - EK on known-EK list (binds AIK to real TPM)
   - Nonce matches (freshness, replay protection)
   - PCR composite matches known-good value

A rootkit loading after measured boot cannot alter PCRs.
A software simulator cannot produce valid Quote without TPM private key.
```

### DRTM and Secure Launch
```
Dynamic Root of Trust for Measurement allows "late launch" —
trusted execution environment established after OS boot,
measurement captured into PCR[17].

Intel: GETSEC[SENTER] (TXT)
AMD: SKINIT (SVM extension)

CPU enters measured execution state, Secure Loader Block (SLB)
loaded and hashed into PCR[17], control transfers to
Measured Launch Environment (MLE).

Microsoft System Guard Secure Launch uses this to load HVCI's
hypervisor into measured state independent of SRTM chain.
Defender requests Quote including PCR[17] and matches against
known-good MLE measurement.
```

### UEFI Pre-Boot DMA Integrity
```
Pre-Boot DMA Protection: firmware must isolate DMA-capable devices'
I/O buffers before ExitBootServices().

ACPI indicators:
- Intel: DMA_CTRL_PLATFORM_OPT_IN_FLAG in DMAR table flags
- AMD: DMA remap support bit in IVRS IVinfo field

Windows PCR[7] event: firmware extends EV_EFI_ACTION with
"DMA Protection Disabled" when IOMMU/Kernel DMA Protection
is disabled, providing attestation hook.

Combined picture:
PCR[0]/PCR[7] anchor firmware and DMA-protection policy,
ACPI tables describe runtime IOMMU config,
documented DMA interfaces show what OS actually remaps,
attestation ties local claims to remote-verified known-good policy.
```

## Layered Detection Pipeline

### Pre-Game Environmental Verification
```
- IOMMU active and applied to DMA-capable PCIe paths
- Interrupt Remapping enabled
- Secure Boot enabled
- VBS/HVCI active
- TPM 2.0 present and provisioned
- Attestation Quote validates against expected policy
- BIOS/UEFI version not in known vulnerable pre-boot DMA list
- ACS topology walk: all relevant bridges enforce SV, TB, RR, CR
```

### PCIe Inventory Pass
```
- Enumerate all PCIe devices via PnP tree
- Full 4 KB config-space dump for each
- Check device problem codes (DEVPKEY_Device_ProblemCode)
- Cross-reference SMBIOS slot inventory with populated devices
```

### Configuration Integrity Per Device
```
- VID/DID/SVID/SDID against known-good list
- Capability-chain walk and validation
- Signature-residue scan
- BAR mask verification
- R/W consistency probing
- Compare against per-donor reference database
```

### Behavioral Sampling During Play
```
- Periodic Link Status reads (LTSSM, ASPM transitions)
- AER counter snapshots
- Per-device interrupt rate and distribution
- Per-device IOMMU fault rate
- BAR-region content sampling for class consistency
```

### Statistical Analysis Over Session
```
- Latency distribution comparison (KS test, Hill estimator)
- Interrupt arrival distribution
- ASPM transition rate
```

### Cheat-Phase Detection
```
- Honeypot region access
- Memory access frequency / locality classifiers
```

### Containment Before Verdict
```
Each detection produces evidence, not a verdict.
Verdict informed by:
- Multi-signal correlation (single signals can false-positive;
  combinations rarely do)
- Server-side aggregation across sessions
- Behavioral verification (input timing, gameplay statistics)

While verdict accumulates, containment protects the live match:
IOMMU re-remapping to sandbox, BME clearance, or EPT-level
game-process protection degrades cheat effectiveness in real time.
```

### Realistic Limits
```
A firmware that:
- Clones donor byte-for-byte (full 4 KB config + all capabilities)
- Implements donor-class BAR MMIO, MSI generation, overlay RAM
- Adds completion-latency jitter matching donor distribution
- Generates plausible AER correctable-error rates
- Transitions through ASPM states like the donor
- Uses donor not present in gaming PC and not on blacklists
- Operates only within driver-mapped IOMMU domains (legitimate-path exfil)
- Avoids honeypot regions through gameplay-aware address whitelisting

...can defeat every PCIe-layer and IOMMU-layer signature in isolation.

This is why external trust anchors are required:
TPM attestation, measured boot, and server-side correlation operate
outside the "spoof a PCIe endpoint" problem.

A perfectly emulated DMA card cannot forge a TPM Quote.
But the verifier must bind Quote to allowlist/blocklist of BIOS versions,
DMA-protection events, Secure Boot state, VBS/HVCI, and IOMMU policy.

The cost of defeating all four layers simultaneously
(PCIe + IOMMU + hypervisor + attestation) exceeds typical cheat value.
```

## Forensic Evidence Capture

### What to Capture
```
Artifact                     Source                          Purpose
──────────────────────────────────────────────────────────────────────────────
Full 4 KB config dump        Bus interface / ECAM            Donor ID post-hoc
Capability chain walk        Parsed from config              Capability presence
PCIe link state history      Link Status over session        LTSSM anomaly proof
MSI/MSI-X arrival timeline   OS interrupt telemetry          Rate claim refutation
AER correctable counts       AER capability registers        Baseline outlier proof
IOMMU fault log entries      WHEA/ETW, Driver Verifier       DMA-violation proof
IOMMU domain assignments     IOMMU manager state walk        Passthrough anomaly
ACS bridge state             Bridge enumeration              Isolation proof
Honeypot access record       Hypervisor EPT trap log         Unauthorized read evidence
TPM PCR snapshot             TPM Quote API                   Boot-chain attestation
MCFG / DMAR / IVRS tables   ACPI subsystem                  Platform config baseline
SMBIOS slot inventory        DMI subsystem                   Slot-population audit
BIOS version + patch level   SMBIOS                          Pre-Boot DMA fix verify
Latency-distribution hists   Per-session sampling            Statistical fingerprint
```

### Multi-Signal Correlation
```
Strongest evidence packages combine:
1. Hardware-layer signal (config space, BAR, link state)
2. Behavioral-layer signal (interrupt distribution, IOMMU fault rate, honeypot)
3. Temporal correlation (hardware signal preceded behavioral by plausible interval)

Three independent signals push false-positive rates below threshold
where appeals become a practical workload.
```

### PCIe Protocol Captures
```
A PCIe protocol analyzer (interposer) produces the strongest forensic evidence:
full TLP-level captures with byte-exact accuracy and nanosecond timestamps.

Commercial analyzers capture every TLP, DLLP, and physical-layer ordered set.
Traces can be replayed to confirm fingerprinting findings.

Cost (tens to hundreds of thousands USD) limits routine use, but for
high-profile cases (competitive integrity, tournament, prosecution),
protocol-level captures by independent labs are the unambiguous reference.
```

## Security Considerations

### Ethical Use
```
- Security research only
- Authorized testing environments
- Responsible disclosure
- Legal compliance
```

### Risk Awareness
```
- Physical hardware access required
- Potential system instability
- Detection by advanced anti-cheat
- Legal implications
```
