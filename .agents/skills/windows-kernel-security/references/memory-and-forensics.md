# Memory And Forensics

## Kernel Segment Heap Architecture

### Timeline
```
Windows NT ~ 1809   : Legacy NT Pool Manager (ExAllocatePoolWithTag)
Windows 10 19H1     : Kernel Segment Heap introduced (March 2019, build 1903)
                      └─ User-mode Segment Heap ported to the kernel
Windows 10 2004     : ExAllocatePool2 / ExAllocatePool3 added
                      └─ ExAllocatePoolWithTag officially deprecated
Windows 10 20H2~    : Dynamic KDP (Kernel Data Protection) stabilized
Windows 11          : VBS/HVCI enabled by default; Secure Pool usage expanded

Common misconception: Many sources claim "the Segment Heap was introduced
in Windows 10 2004," but the kernel segment heap was actually introduced
in 19H1 (1903). Windows 10 2004 added the new Pool APIs built on top of it.
```

### Legacy NT Pool Structure (_POOL_HEADER, pre-19H1)
```
_POOL_HEADER (16 bytes, x64):
Offset  Field           Size   Description
0x00    PoolIndex        1 B    Pool descriptor index
0x01    PreviousSize     1 B    Previous chunk size
0x02    PoolType         1 B    Pool type (Paged, NonPaged, etc.)
0x03    BlockSize        1 B    Current chunk size (>> 4)
0x04    PoolTag          4 B    4-byte ASCII tag
0x08    ProcessBilled    8 B    KPROCESS pointer (valid only with PoolQuota)

Memory layout:
[POOL_HEADER 16B][user data ...][POOL_HEADER 16B][user data ...]
      ↑ plaintext, predictable        ↑ adjacent → overwritable

Security weaknesses:
- Pool Walking: traverse chunks linearly via BlockSize
- Pool Overflow: corrupt adjacent header for arbitrary write on free
- PoolIndex Overwrite: OOB dereference into pool descriptor array
- ProcessBilled Overwrite: arbitrary address dereference on free path

Windows 8 partial mitigation:
  ProcessBilled = KPROCESS_PTR ^ ExpPoolQuotaCookie ^ CHUNK_ADDR
  But plaintext _POOL_HEADER remained until 19H1.
```

### _SEGMENT_HEAP Core Structure
```
Each pool type is managed by its own independent _SEGMENT_HEAP instance.

_SEGMENT_HEAP (kernel offsets, based on 20H2):
0x000   EnvHandle (10 B)     — heap environment handle
0x010   Signature (4 B)      — always 0xDDEEDDEE
0x028   UserContext (8 B)
0x048   AllocatedBase (8 B)  — LFH structure allocation base
0x058   SegContexts[2] (0x180 B) — segment context array
0x100   VsContext (0xC0 B)   — VS allocator context
0x280   LfhContext (0x4C0 B) — LFH allocator context
higher  LargeAllocMetadata   — large allocation metadata
higher  LargeReservedPages / LargeCommittedPages

Per pool type instances (nt!PoolVector / HEAP_POOL_NODES):
├── NonPagedPool (NP)       → _SEGMENT_HEAP instance #1
├── NonPagedPoolNx (NPNx)   → _SEGMENT_HEAP instance #2  ← primary target
├── PagedPool (PP)          → _SEGMENT_HEAP instance #3
├── PagedPoolSession        → _SEGMENT_HEAP stored in current thread
└── (other special pools)
```

### Allocation Routing Flow
```
ExAllocatePoolWithTag / ExAllocatePool2 / ExAllocatePool3
        │
        ▼
  ExAllocateHeapPool (internal)
        │
        ├─ size ≤ 0x200 AND LFH activated  ──▶  kLFH
        │   └─ RtlpHpLfhContextAllocate
        │
        ├─ 0x1e1 ≤ size ≤ 0xfe0            ──▶  VS Allocator
        │   └─ RtlpHpVsContextAllocateInternal
        │
        ├─ page-aligned (0x20000~0x7f0000)  ──▶  Segment Allocator
        │   └─ RtlpHpSegAlloc
        │
        └─ large (> 0x7f0000)              ──▶  Large Allocator
            └─ RtlpHpLargeAlloc
```

### kLFH (Low Fragmentation Heap)
```
Size range:       ≤ 0x200 bytes (512 B), when LFH activated for that size class
Activation:       After 18 consecutive allocations of the same size
Key function:     RtlpHpLfhContextAllocate
Chunk header:     _POOL_HEADER (16 B, still present)
Metadata:         _HEAP_LFH_SUBSEGMENT (isolated, not inline)
Bucket count:     129 (Buckets[129])

Bucket structure:
_HEAP_LFH_CONTEXT
└── Buckets[129]
     ├── Bucket #0:   size 1~8 B
     ├── Bucket #1:   size 9~16 B
     ├── ...
     └── Bucket #128: size ~0x1FF B
         (each bucket has AffinitySlots → _HEAP_LFH_SUBSEGMENT)

Security properties:
- Block placement within subsegment is randomized
- Next allocation position managed through FreeHint, encoded with LfhKey
- Adjacent chunk overflow cannot directly corrupt management structure
```

### VS Allocator (Variable Size)
```
Size range:       (a) ≤ 0x1e0 && LFH inactive; (b) 0x1e1~0xfe0;
                  (c) 0x1001~0xffff && non-page-aligned
Key function:     RtlpHpVsContextAllocateInternal
Chunk header:     _HEAP_VS_CHUNK_HEADER (16 B, HeapKey XOR encoded)
Free management:  Red-Black Tree (FreeChunkTree)
Algorithm:        Best-fit

_HEAP_VS_CHUNK_HEADER (allocated state):
┌──────────────────────────────────────────────────────────┐
│  Sizes (8 B) — XOR encoded: HeaderBits ^ self_addr ^ HeapKey
│    ├─ UnsafeSize      : chunk size / 16
│    ├─ UnsafePrevSize  : previous chunk size / 16
│    ├─ MemoryCost      : pages occupied
│    └─ UnusedBytes     : whether unused bytes exist
│  EncodedSegmentPageOffset (1 B)
│    — (self_addr ^ self ^ HeapKey) & 0xFF
│    — page distance to VS subsegment start
└──────────────────────────────────────────────────────────┘

Memory layout:
[_HEAP_VS_CHUNK_HEADER 16B][_POOL_HEADER 16B][user data ...]
       ↑ HeapKey XOR             ↑ PoolTag etc. still present

VS subsegment structure (_HEAP_VS_SUBSEGMENT):
├── ListEntry      — subsegment linked list
├── CommitBitmap   — page commit state bitmap
├── CommitLock     — lock used during commit
├── Size (2 B)     — subsegment size (>> 4)
└── Signature (15 bit) + FullCommit (1 bit) — integrity check
```

### Segment Allocator (Backend)
```
Size range #1:    0x20000 < size ≤ 0x7f000 (128 KB ~ 508 KB)
Size range #2:    0x7f000 < size ≤ 0x7f0000 (508 KB ~ ~7 MB)
Core structure:   _HEAP_PAGE_SEGMENT + 256 page descriptors
Segment mask:     0xFFFFFFFFFFF00000

The kernel uses two independent SegContexts (unlike user-mode's single context).

Page segment signature encoding:
check = page_segment ^ page_segment->Signature
      ^ 0xA2E64EADA2E64EAD ^ RtlpHpHeapGlobals.HeapKey
```

### Large Allocator
```
Size range:       > 0x7f0000 (typically page-aligned)
Key function:     RtlpHpLargeAlloc
Metadata:         _SEGMENT_HEAP.LargeAllocMetadata
Tracking:         BigPagePoolTable (PoolTrackTable)
No inline header; metadata recorded externally.
```

### Header Layout Per Allocation Path
```
Path          Memory layout (chunk start → user data)
────────────────────────────────────────────────────────────────
kLFH          [_POOL_HEADER 16B] [data]
VS            [_HEAP_VS_CHUNK_HEADER 16B] [_POOL_HEADER 16B] [data]
Segment       [_HEAP_PAGE_SEGMENT header] ... [page descriptors]
Large         Metadata in BigPagePoolTable; no inline header
CacheAligned  [_POOL_HEADER #1] ... [_POOL_HEADER #2 (CacheAligned)] [data]
```

### Residual _POOL_HEADER Under Segment Heap
```
_POOL_HEADER was not fully removed. Remaining usage:

Field           Status under Segment Heap
PoolTag         Still recorded (for debugging/tracing)
PoolType        Recorded, not used for allocator selection on free
BlockSize       Unused in VS path; still present in kLFH
PreviousSize    Unused, set to 0
PoolIndex       Unused, set to 0
ProcessBilled   Valid only with PoolQuota flag (encoded with ExpPoolQuotaCookie)
```

### Pointer Encoding Mechanisms
```
Global key structure: _RTLP_HP_HEAP_GLOBALS (nt!RtlpHpHeapGlobals)
Generated randomly at boot time; global in ntoskrnl.

{
    UINT64 HeapKey;   // VS Allocator + Segment Allocator header encoding
    UINT64 LfhKey;    // LFH callback pointer encoding
}

Encoding formulas:

VS chunk header — Sizes field:
  encoded = (real Sizes) ^ (address of vs_chunk_header) ^ HeapKey

VS chunk — EncodedSegmentPageOffset:
  encoded = ((real page distance) ^ vs_chunk_header ^ HeapKey) & 0xFF

Segment context signature:
  check = page_segment ^ page_segment->Signature
        ^ 0xA2E64EADA2E64EAD ^ HeapKey

LFH callback function pointer:
  encoded = real function address ^ HeapKey ^ address of LfhContext

ProcessBilled (POOL_HEADER, Windows 8+):
  encoded = KPROCESS_PTR ^ ExpPoolQuotaCookie ^ CHUNK_ADDR

Implications for attackers:
- Must leak HeapKey and LfhKey from RtlpHpHeapGlobals
- Must know chunk's own virtual address (self-referential XOR)
- Failing encoding validation triggers:
  BugCheck 0x139 (KERNEL_SECURITY_CHECK_FAILURE) or
  BugCheck 0x13A (KERNEL_MODE_HEAP_CORRUPTION)
```

### Dynamic Lookaside and Delay Free
```
Dynamic Lookaside:
_HEAP_VS_CONTEXT
└── Lookaside buckets (_RTL_DYNAMIC_LOOKASIDE)
     ├── Per-size singly-linked lists
     ├── Depth (2 B)     — current list depth
     └── NextEntry (8 B) — pointer to next cached chunk

Rebalancing (every 3 Balance Set Manager scans):
- alloc count < 25 → Depth decreases by 10
- miss ratio ≥ 0.5% → Depth increases
- miss ratio < 0.5% → Depth decreases by 1
- Range: minimum 4 ~ MaximumDepth

Delay Free (VS Allocator):
- size < 1 KB AND Config.Flags bit 4 == 1:
  → stored in DelayFreeContext list
  → batch freed after 32 entries accumulate
- Otherwise: inserted immediately into FreeChunkTree
- Security: disrupts UAF timing (cannot immediately reuse freed chunk)
```

### New Pool APIs: ExAllocatePool2 / ExAllocatePool3
```
Evolution:
ExAllocatePool                 (legacy, no tag)
ExAllocatePoolWithTag          (pre-19H1 standard, deprecated in 2004)
ExAllocatePoolWithTagPriority  (priority support)
ExAllocatePoolWithQuotaTag     (quota tracking)
        ↓
ExAllocatePool2                (general case, zero-initialized by default)
ExAllocatePool3                (extended parameters, priority + Secure Pool)

ExAllocatePool2:
  PVOID ExAllocatePool2(POOL_FLAGS Flags, SIZE_T NumberOfBytes, ULONG Tag);
  - Zero-initialized by default (no RtlZeroMemory needed)
  - Returns NULL on failure by default
  - POOL_FLAG_RAISE_ON_FAILURE converts to exception
  - POOL_FLAG_USE_QUOTA integrates legacy PoolQuota

ExAllocatePool3:
  PVOID ExAllocatePool3(POOL_FLAGS Flags, SIZE_T NumberOfBytes, ULONG Tag,
                        PCPOOL_EXTENDED_PARAMETER ExtendedParameters, ULONG Count);
  Extended parameter types:
  - PoolExtendedParameterPriority: allocation priority (e.g., HighPoolPriority)
  - PoolExtendedParameterSecurePool: KDP Secure Pool (VTL0 write-protected)

Down-level compatibility:
  #define POOL_ZERO_DOWN_LEVEL_SUPPORT
  ExInitializeDriverRuntime(DriversRuntimeInitSupportFlags);
  → ExAllocatePool2 internally falls back to alloc + memset on older OS
```

### Kernel Data Protection (KDP) and Secure Pool
```
KDP leverages Segment Heap's Secure Pool feature to allocate
read-only kernel memory that cannot be modified from VTL0.

Virtual address space layout:
  Dedicated 512 GB Secure Pool region (1 PML4 entry)
  Base address: randomized at boot
  Managed by: Secure Kernel (VTL1)
  VTL0 writes: blocked via NAR (Node Address Range)

Initialization flow:
1. NT Memory Manager boot Phase 1
2. Randomly calculate 512 GB Secure Pool virtual address
3. INITIALIZE_SECURE_POOL Secure Call → Secure Kernel
4. Secure Kernel creates NAR + initializes NTE (Node Table Entry)

Anti-cheat usage:
  // Create Secure Pool context
  ExCreatePool(POOL_FLAG_NON_PAGED, tag, &securePoolHandle);

  // Allocate detection rule table (immutable after init)
  POOL_EXTENDED_PARAMS_SECURE_POOL sp = {
      .Cookie           = MY_COOKIE,
      .SecurePoolHandle = securePoolHandle,
      .Buffer           = &detectionRuleTable,
      .SecurePoolFlags  = SECURE_POOL_FLAGS_FREEABLE
      // MODIFIABLE flag omitted → write-protected after init
  };
  g_DetectionRules = ExAllocatePool3(POOL_FLAG_NON_PAGED,
      sizeof(detectionRuleTable), 'DRul', &extParams, 1);
  // g_DetectionRules is now immutable — no VTL0 code can modify it
```

### Attack Technique Evolution (Segment Heap Era)
```
Technique comparison:
Technique                    NT Pool (pre-19H1)    Segment Heap (19H1+)
─────────────────────────────────────────────────────────────────────────
Adjacent header overwrite    Easy                  Blocked by encoding
Pool Walking                 Possible              Impossible (metadata isolation)
ProcessBilled overwrite      Requires Win8+ cookie Requires cookie + HeapKey
kLFH pool spray              Predictable           Possible but needs precise control
VS FreeChunkTree corruption  N/A                   Requires HeapKey bypass
Large chunk BigPool tracking PoC exists             PoC exists (PoolTrackTable)

Modern kLFH exploit requirements:
1. Find target object of same size (same kLFH bucket)
2. Target must contain exploitable members (pointer, function table)
3. Target allocation must be triggerable from user mode
4. Vulnerable and target objects must be in same pool type
   (NonPagedPoolNx and PagedPool use separate _SEGMENT_HEAP instances)

VS chunk overflow recovery (must restore to avoid BugCheck):
  ghost_chunk->Sizes.HeaderBits =
      (real_sizes) ^ (ULONG_PTR)ghost_chunk ^ HeapKey;
  ghost_chunk->EncodedSegmentPageOffset =
      ((real_page_offset) ^ (ULONG_PTR)ghost_chunk ^ HeapKey) & 0xFF;
  // Failure → BugCheck 0x13A

Required pre-exploit leak values:
Symbol                          Purpose               Source
nt!RtlpHpHeapGlobals           HeapKey, LfhKey        Pattern scan ExFreePoolWithTag
nt!ExpPoolQuotaCookie           Decode ProcessBilled   Pattern scan ExAllocatePoolWithQuotaTag
nt!PsInitialSystemProcess      EPROCESS chain         ntoskrnl import analysis
Chunk's own virtual address    Self-referential XOR   Requires info-leak primitive
```

### BugCheck Codes (Segment Heap Related)
```
Code    Name                               Trigger
0x139   KERNEL_SECURITY_CHECK_FAILURE      VS/LFH header integrity check failure
0x13A   KERNEL_MODE_HEAP_CORRUPTION        Heap metadata corruption detected
0xC5    DRIVER_CORRUPTED_EXPOOL            Pool accessed at incorrect IRQL
0x19    BAD_POOL_HEADER                    _POOL_HEADER validation failure (LFH path)
```

## Pool Allocation & Forensics

### Pool Forensics Artifacts
```
PiDDBCacheTable:
- Tracks historically loaded drivers by hash + timestamp
- Anti-cheat inspects this to detect BYOVD or test-signed driver loads
- Attackers attempt to remove entries post-load

MmUnloadedDrivers:
- Circular buffer of recently unloaded drivers (name + address range)
- Cannot be cleared from user mode
- Anti-cheat uses to detect load-unload-reload patterns

PoolBigPageTable:
- Maps large pool allocations (>= PAGE_SIZE) to owning driver tag
- Used for: identifying hidden drivers, finding leaked pool allocations
- Anti-cheat walks this to detect manually mapped driver memory
```

### Pool Tag Forensics
```
- ExAllocatePoolWithTag / ExAllocatePool2: every allocation carries a 4-byte tag
- Pool tag scanning: identify driver presence by known tags
- Tool: pooltag.txt (Microsoft), PoolMon, WinDbg !poolfind
- Anti-cheat technique: scan pool tags for known cheat driver signatures
```

### Modern Pool Scanning (Segment Heap Era)
```
Legacy method (pre-19H1) — NO LONGER WORKS:
  Follow BlockSize in inline header to traverse linearly.
  PPOOL_HEADER h = startAddr;
  while (h->BlockSize != 0) {
      if (h->PoolTag == TARGET_TAG) { /* ... */ }
      h += h->BlockSize;  // Invalid under segment heap
  }

Modern alternatives:

BigPool detection (Large Alloc path):
  Reference nt!PoolBigPageTable (or nt!PoolTrackTable)
  └─ Traverse BigPagePoolTable entries
  └─ Find allocations without corresponding driver objects

Small allocation detection:
  _SEGMENT_HEAP → VsContext → SubsegmentList traversal
  _SEGMENT_HEAP → LfhContext → Buckets[] → AffinitySlots → Subsegments

VS Chunk Header decoding (requires HeapKey):
  real_sizes = encoded_header ^ chunk_address ^ HeapKey
  → Decode to determine chunk size, PoolTag, allocation legitimacy

Anti-cheat scanning targets:
- Suspicious PoolTags: cheat drivers use custom/rare tags; maintain blacklist
- Executable permission pages: NonPagedPool chunks with X permission
  from suspicious sources (no corresponding loaded module)
- Shellcode patterns: scan decoded chunk contents for known cheat signatures,
  ROP gadgets, specific syscall sequences
- kLFH allocation pattern anomalies: unusual allocation patterns in
  specific size buckets can indicate pool grooming

WinDbg commands:
  dt nt!_RTLP_HP_HEAP_GLOBALS nt!RtlpHpHeapGlobals  // HeapKey, LfhKey
  dt nt!_SEGMENT_HEAP <address>
  dt nt!_HEAP_VS_CHUNK_HEADER <address>
  dt nt!_HEAP_LFH_CONTEXT <address>
  !pool <address>
  !poolfind <Tag> [pool_type]
  !poolused [flags]                    // stats by PoolTag
  dt nt!_POOL_TRACKER_BIG_PAGES nt!PoolBigPageTable

VS chunk header decode (manual):
  HeaderBits_raw = poi(<chunk_addr>)
  real Sizes = HeaderBits_raw ^ <chunk_addr> ^ HeapKey
```

### Driver Development Migration Checklist
```
□ ExAllocatePoolWithTag          → ExAllocatePool2
□ ExAllocatePool (without tag)   → Remove or ExAllocatePool2
□ ExAllocatePoolWithTagPriority  → ExAllocatePool3 + Priority param
□ ExAllocatePoolWithQuotaTag     → ExAllocatePool2 + POOL_FLAG_USE_QUOTA
□ RtlZeroMemory after alloc     → Remove (ExAllocatePool2 zero-initializes)
□ Review POOL_FLAG_RAISE_ON_FAILURE (NULL check vs exception)
□ Critical read-only data       → ExAllocatePool3 + Secure Pool
```

### SSDT Hooking (Legacy)
```
- Modify service table entries
- Requires PG bypass
- High detection risk
```

### IRP Hooking
```
- Hook driver dispatch routines
- Less monitored than SSDT
- Per-driver targeting
```

## Memory Manipulation

### Physical Memory Access
```cpp
MmMapIoSpace
MmCopyMemory
\\Device\\PhysicalMemory
```

### Virtual Memory
```cpp
ZwReadVirtualMemory
ZwWriteVirtualMemory
KeStackAttachProcess
MmCopyVirtualMemory
```

### MDL Operations
```cpp
IoAllocateMdl
MmProbeAndLockPages
MmMapLockedPagesSpecifyCache
```
