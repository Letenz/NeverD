**Languages**: [English](cpu-execution.md) | [简体中文](zh-CN/cpu-execution.md) | [繁體中文](zh-TW/cpu-execution.md) | [日本語](ja/cpu-execution.md) | [한국어](ko/cpu-execution.md) | [Français](fr/cpu-execution.md) | [Deutsch](de/cpu-execution.md) | [Español](es/cpu-execution.md) | [Italiano](it/cpu-execution.md) | [Русский](ru/cpu-execution.md) | [العربية](ar/cpu-execution.md)

# CPU configuration and capability queries

CPU execution is independent of the guest OS, image loader and calling
convention. Enable `NEVERD_ENABLE_CPU_EMULATION` to build it alone, or
`NEVERD_ENABLE_DRIVER_EMULATION` to include the Windows driver environment.
The [architecture guide](architecture.md#cpu-execution) describes ownership,
backend selection and current platform limitations.

`NEVERD_ENABLE_SEMANTIC_TESTS` defaults to `ON` and controls the test group in `unittests/semantic`, including its aggregate runners. To build native CPU tests without Unicorn, keep `BUILD_TESTING=ON` and set both `NEVERD_ENABLE_SEMANTIC_TESTS=OFF` and `NEVERD_EMULATION_BACKEND_UNICORN=OFF`. The native KVM/WHP tests remain available, including Windows ARM64/MSVC builds with suitable SDK headers. Enabling Unicorn on Windows ARM64 still requires an ARM64 LLVM-MinGW toolchain. This build separation does not establish native ARM64 runtime coverage.

## Configuration

The public [`ExecutionConfiguration`](../include/neverd/emulation/ExecutionConfiguration.h)
is consumed by both the CPU factory and capability reporting. Requirements are
validated before CPU allocation or address-space attachment. Omitted values
select a contract's fixed profile; explicit unsupported values fail.

| JSON field | Default | Meaning |
|------------|---------|---------|
| `backend` | `auto` | `auto`, `unicorn`, `kvm` or `whp` |
| `contract` | `software-cpu-v1` | Versioned execution semantics |
| `architecture` | `x86_64` | `x86_64` or `aarch64` |
| `privilege` | Contract profile | `flat`, `supervisor` or `user`; must match the chosen contract |
| `virtual_address_bits` | Contract profile | Checked profiles use 48 bits; flat profiles expose a 64-bit direct mapping namespace |
| `page_size` | 4096 | Guest mapping granule; other values are rejected |
| `required_features` | `[]` | Required feature names from [the inventory](../include/neverd/emulation/ExecutionConfiguration.def) |

`driver-strict` accepts x64 with backend-qualified capabilities; `software-cpu-v1` accepts x64 and ARM64.
`checked-x64-v1` and `checked-aarch64-v1` require their named architecture
and execute at supervisor privilege. `checked-user-x64-v1` and
`checked-user-aarch64-v1` execute the corresponding bounded instruction inventory
at CPL3 and EL0, respectively, with architectural MMU isolation and explicit
service-request exits. They support Unicorn and matching-host KVM/WHP; `auto`
follows the existing host selection.
Flat profiles have no architectural user/supervisor MMU isolation contract;
their address width describes the direct mapping interface, not a claim of a
64-bit hardware virtual-address mode. Checked x64 advertises bounded SIMD and
floating-point families: legacy SSE/SSE2 moves and logic, MOVLHPS/MOVHLPS,
and masked scalar CVTTSS2SI/CVTTSD2SI/SUBSS/SUBSD. MXCSR preserves sticky status,
rounding and FTZ; DAZ and unmasked exceptions are rejected. Supervisor x64
additionally supports checked scalar
MMIO and prepared-read string transfers; checked user profiles reject device
mappings. All checked profiles reject port I/O and parallel CPU requirements.
The instruction inventory remains authoritative; a feature flag does not
admit every encoding in a family. Only checked user profiles
advertise `service_traps`, meaning the interception boundary described below;
supervisor and flat profiles do not advertise that boundary.

Checked x64 also admits masked legacy `ADD`, `SUB`, `MUL`, `DIV`, `SQRT`, `MIN` and `MAX` in `SS`, `SD`, `PS` and `PD` forms. `X64SSEInstructions.def` owns operand widths, alignment and admission. `MaskedSSEArithmeticMatchesIndependentHostExecution` compares register and RAM forms against an independent host CPU oracle, including all four rounding modes, FTZ, signed zero, subnormal inputs and NaNs; `SSEMemoryObserverStopsBeforeResultAndStatusChanges` verifies cancellation before effects. This does not admit DAZ, unmasked exceptions, x87 or AVX.

Checked x64 also admits scalar `XCHG`, `XADD` and `CMPXCHG` at 8/16/32/64 bits,
with natural alignment for locked or implicit-lock memory forms. An ISA-owned
footprint describes every ordinary RAM write. The physical execution lease and
bounded `RAMTransaction` keep the actual next CPU/RAM state private until the
observers accept it; result callbacks see original state and exact processor
write values. Cancellation, callback failure or a transport error discards
speculative effects. On a processor exception, RAM rolls back before OS
delivery while architectural exception status is retained. ARM64 scalar/pair
stores use the same RAM authority. Devices, unknown footprints and parallel
hardware SMP remain outside this transaction. CPU snapshots do not undo already
committed RAM.

x64 contexts also preserve the complete x87 state: control/status, TOP,
physical nonempty tags, opcode, instruction/data pointers and eight physical
80-bit payloads. `FP0`–`FP7` use `RegisterValue`; scalar access rejects them
instead of truncating their high 16 bits. `FPTag` is the physical abridged mask.
All existing register identities, including `Invalid`, remain unchanged.
KVM/WHP and both Unicorn paths share the same reset and state model. This
transport coverage does not admit x87 instructions into checked profiles.

User execution requires `UserAccessible` as well as the appropriate `Read`,
`Write` or `Execute` bit on **every** mapped page. Existing mappings default to
supervisor access. Aliases have independent rights even when sharing physical
bytes. `UserAccessible` alone grants no access. Trusted host memory operations
and supervisor CPUs continue to use RWX; they can prepare or inspect user and
supervisor pages. Flat software profiles ignore the privilege bit. For example:

```cpp
Configuration.Contract = ExecutionContract::CheckedUserX64;
Configuration.Privilege = ExecutionPrivilege::User;
auto CPU = llvm::cantFail(createExecutionBackend(Configuration, Space)).CPU;
llvm::cantFail(CPU->map(Code, 4096, Read | Write | Execute | UserAccessible));
```

A CPU's privilege is fixed by its contract. Context restore and address-space
binding preserve it; writing x64 segment selectors cannot escalate privilege.
Recoverable data-access faults retain the original instruction and registers
until their owner resolves or delivers the fault. Host-side `canAccess` checks
exactly the requested bits; add `UserAccessible` to query user visibility.
`executable` checks the selected CPU's execution permissions.

Checked x64 RAM operands may span pages backed by separate allocations or
aliases. The complete operand is validated before native execution. An
inaccessible later page reports its first failing address and remaining
in-page extent; the CPU and writable prefix retain their original state.
REP MOVS commits one complete element per restart boundary, so earlier
completed elements survive a later fault. Mixed RAM/device operands remain
unsupported before observers or device callbacks. Independent native x64
fault probes check these ordinary and REP store boundaries.

Checked x64 admits `DIV` and `IDIV` with register or ordinary RAM operands at
all four integer widths. The processor computes the quotient and remainder;
zero divisors and quotient overflow report the actual `#DE`, preserving the
faulting context. An admitted `RecoverableFault` callback leaves the event
pending until its OS owner consumes it and explicitly installs a continuation.
Otherwise it remains a terminal guest trap. Native synchronous exceptions are
distinct from transport failures; this does not admit unlisted instructions.
KVM uses a private supervisor IDT/IST gateway chosen outside guest mappings,
while WHP intercepts an explicit architectural exception bitmap.

Page tables are private CPU projections. A supervisor projection preserves its
existing supervisor-only translation semantics; a user projection marks only
explicitly user-accessible guest pages as user pages. ARM64 user projections
also make user pages non-executable at EL1. This does not expose mutable guest
page tables or a privilege-switch instruction API. Service ABI handling,
exception delivery, process loading and OS services remain separate runtime work.

The [Linux process profile](process-emulation.md) uses these user contracts for
real ELF startup and system-call execution through `neverd emulate` and the
shared SDK. Its service implementations belong to the OS model, independently
of the selected CPU transport.

Unknown fields, null field values, invalid names, duplicate required features,
invalid numeric widths and unsupported combinations fail. Input is limited to
64 KiB. The old CPU factories and driver C options remain compatible.

## Query without executing a workload

```bash
neverd cpu-capabilities
neverd cpu-capabilities \
  --configuration='{"contract":"checked-aarch64-v1","architecture":"aarch64"}'
neverd cpu-capabilities \
  --configuration='{"contract":"checked-x64-v1","backend":"auto"}' \
  --probe-host
```

The schema version is 1. The report separates:

- `requested_configuration`: requirements before profile defaults are filled.
- `configuration`: normalized requirements and the selected backend.
- `capabilities`: static semantic support, address model, RAM/mapping ceilings,
  instruction admission families, observation and execution-control precision.
- `build`: adapter build and compiled host ABI compatibility, without creating
  a CPU or calling a hypervisor API.
- `host`: null unless `--probe-host` is requested. A probe initializes a
  temporary CPU over private RAM and reports live availability.

`native_execution` means the contract permits a native adapter. It does not
mean this build includes it or this machine can initialize it. Instruction
families do not admit every encoding or operand combination; admission remains
authoritative. Host probing checks initialization only, including any adapter
startup probe, and does not certify a workload or replace independent native ARM64 validation.
Availability can change after the query. Reasons distinguish disabled builds,
host platform/ISA mismatches, device access, host API errors, missing capability
and other initialization failures. No unavailable backend silently falls back.

`thread_pointer` covers x64 FS/GS base state and admitted scalar memory operands,
and ARM64 `TPIDR_EL0` state with its exact `MRS`/`MSR` encodings. Each native
transport synchronizes that state, and CPU snapshots preserve it independently
of memory. Exact FPCR/FPSR accesses are admitted separately; other ARM64
system-register encodings remain outside the checked inventory. This capability does not create OS threads or allocate TLS blocks.

Whole-instruction memory preflight is specific to the checked contracts.
Software engine callbacks can describe split accesses and partial instruction
effects. Stop/deadline control is cooperative: between checked instructions or
through an engine request. `hard_wall_clock_bound` is false. Native entry and
caller callbacks must not be assumed to finish by a deadline.

The CLI returns zero for a valid report, including an unavailable backend, and
one for an invalid configuration or query failure. Inspect `build` and `host`
when deciding whether to create a CPU.

## SDK and C++ boundaries

[`neverd_cpu_capabilities_json`](../include/neverd/sdk/NeverDCAPICPU.h) accepts
an existing session, optional configuration JSON and a `ProbeHost` value of 0
or 1. No loaded binary is required. NULL configuration uses the defaults above.
Release returned JSON with `neverd_free_string`; NULL indicates an error
available through `neverd_last_error`. CPU-disabled builds export the same
function and report the disabled feature explicitly.

Python plugins use the same parser and report through:

```python
report = session.cpu_capabilities(
    '{"contract":"checked-aarch64-v1","architecture":"aarch64"}',
    probe_host=False,
)
```

C++ consumers can use `executionCapabilities`,
`resolveExecutionConfiguration`, `queryExecutionBackendBuild` and
`probeExecutionBackend` separately. `createExecutionBackend(Configuration, Space)`
attaches a CPU to an existing address space; the memory-limit overload creates
private RAM and a default space. JSON parsing/serialization lives in
[`ExecutionReport.h`](../include/neverd/emulation/ExecutionReport.h).

## CPU outcomes

Native execution checks normal budgets between instructions. KVM and WHP also
request cancellation of an active native entry after its transport allowance
or a stop request, and acknowledge it before retiring execution resources. An
acknowledged interruption discards speculative CPU/RAM effects and returns a
retryable stop or deadline. Genuine host, capture and guest faults retain
priority with independent `StopRequested` and `DeadlineReached` facts. KVM uses
a private execution thread and a temporarily unblocked realtime signal without
changing caller signal
masks or process handlers. The selected signal must remain non-ignored during
an active entry. No profile promises a hard wall-clock bound.

`CPU.runUntilExit(PC, TimeoutMicroseconds)` returns a typed
[`ExecutionExit`](../include/neverd/emulation/ExecutionExit.h). Preconditions
and setup failures return `llvm::Error`; runs that begin execution report an
explicit stop, deadline, service request, recoverable fault, guest fault/trap,
unsupported operation, device failure, backend failure or unexplained engine
stop. Fault/device/backend outcomes outrank a simultaneous stop or deadline;
the independent stop/deadline facts and fault details are retained.

`TimeoutMicroseconds` must be positive and fit both the clock duration and its
absolute deadline. Zero and overflowing values return an error before changing
CPU state, projections or observations. This also applies to `run`: zero no
longer inherits Unicorn's unbounded behavior or the checked profile's immediate
timeout. Use an explicit finite budget for every invocation. An invalid budget
does not fault the CPU or prevent a subsequent valid run.

The result does not consume a pending recoverable fault. Its OS owner must use
`takeRecoverableFault` and install a validated exception transfer before
resumption. The existing `run`, `fault` and `timedOut` APIs remain available;
`run` adapts the typed outcome to the existing error and fault accessors.
Older external CPU implementations that only override
`run` reject the new typed boundary until they implement it.

## Service requests

Checked user x64 intercepts the exact unprefixed `SYSCALL` encoding; checked
user ARM64 intercepts `SVC #imm16`. Other mechanisms, including `SYSENTER`,
`INT`, `HVC` and `BRK`, remain unsupported. Admission and the capability
instruction inventory use the same per-ISA `.def` files.

The instruction observer runs first. Unless it stops or faults the CPU, the
CPU returns `ExecutionExitKind::ServiceRequest` **before** executing the service
instruction or entering any backend transport. `Exit.Service` contains the
instruction kind, original `PC`, sequential `NextPC` and SVC `Immediate` (zero
for x64). Registers, flags, stack, PC and privilege remain unchanged. In
particular, x64 RCX/R11 have not received architectural SYSCALL clobbers, and
ARM64 has not entered an exception vector. This is a pre-entry handoff for a
modeled OS, not native architectural exception delivery. The SVC immediate is
an operand, not a universally defined service number. Architectural service
entry is described in the [Intel instruction manual](https://www.intel.com/content/www/us/en/content-details/671110/intel-64-and-ia-32-architectures-software-developer-s-manual-combined-volumes-2a-2b-2c-and-2d-instruction-set-reference-a-z.html)
and [Arm system-call guide](https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/Learn%20the%20Architecture/Armv8-A%20Instruction%20Set%20Architecture.pdf).

`pendingServiceRequest()` inspects the request. It remains pending across
return from `runUntilExit` and blocks subsequent execution, CPU mutation,
address-space binding and CPU context capture/restore. `takeServiceRequest()`
consumes it exactly once while stopped; consumption alone changes no register
or PC. The owner then decodes its chosen OS ABI, handles the service, applies
result/clobber registers and explicitly chooses the next PC or exception
transfer. Unsupported services must fail explicitly at that owner. Retrying
the original PC produces another request; no implicit NOP or successful
service return is synthesized. Legacy `run` reports a pending-service error.
A service event outranks simultaneous stop/deadline facts, while guest or
backend failures retain higher precedence.

A stopped CPU, software HLT, deadline or guest trap never establishes successful
workload completion. Instruction/event budgets, service dispatch, process/thread
exit, exception delivery and workload success remain runtime/OS decisions.
The shared runtime supports cooperative quanta and explicit service/fault
continuations; the Linux process model executes bounded static ELF workloads.
General dynamic linking, OS threads, signals and the additional OS workloads
remain unfinished. CPU user isolation alone does not establish OS compatibility.
Native Windows and ARM64 user execution still require hardware validation;
Unicorn execution and cross-compilation do not replace that evidence.

`driver-strict` supports KVM on matching Linux x64 hosts and WHP on matching Windows x64 hosts; `auto` selects that native transport, and cross-ISA execution selects Unicorn. Explicit Unicorn and the original V1 API retain the portable software profile. Native execution checks canonical addresses and instruction effects before entry; unavailable hardware fails without fallback. Unsupported instructions and OS behavior remain explicit errors. Native Windows x64 CI verifies the declared WHP CPU/state/memory cases and built-in driver outcomes without Unicorn ([`e7f205ab`](https://github.com/NeverSight/NeverD/actions/runs/36894495012)). Native ARM64 runtime evidence is still pending, and this does not establish arbitrary-driver or Android/Darwin compatibility.

Use `executionCapabilities(Contract, ISA, Backend)` to query the selected profile. `NativeLegacyX64` describes native x64 driver execution. `NeverDNativeDriverTests` validates the original corpus and can run with Unicorn disabled.

Checked ARM64 has one complete state boundary. `Registers.def` defines 39 scalar fields and 32 128-bit vectors; `captureAArch64State` stages every read, applies declared widths and NZCV normalization, then publishes once. Unicorn, KVM and WHP transfer the same inventory, including TPIDR_EL0, TPIDRRO_EL0, TPIDR_EL1, FPCR and FPSR. Native adapters enable FP/SIMD through CPACR_EL1. Any scalar/vector read failure or cancelled entry preserves all caller state.

ARM64 KVM/WHP startup executes the private `AArch64MachineProbe.def` program: NOP, FP32 addition rounded toward positive infinity and a two-lane SIMD addition. Each step compares all 39 scalar fields and 32 vectors, including TLS, NZCV, cleared upper destination bits and retained/cumulative FPCR/FPSR state. The probe uses supervisor monitor storage and one overall deadline. Success verifies this bounded initialization program; independent native ARM64 workload validation remains outstanding.

Native x64 KVM/WHP initialization executes `X64MachineProbe.def` in private supervisor pages. One deadline covers NOP, rounded FP32 addition, two-lane SIMD addition, FS/GS loads and CS/SS/CR8 reads; every step compares the complete scalar, XMM, physical x87 and control state. x64 and ARM64 probes require the exclusive physical-memory execution lease. `MemoryProjection` owns cache identity (ISA, address space, mapping generation, privilege and monitor variant) and committed root history per ISA. Builders invalidate before rewriting private bytes; failed replacement cannot reuse partially written tables, and callers cannot supply stale roots. These probes certify bounded initialization only; independent native ARM64 workload validation remains outstanding.

The shared XSAVE decoder distinguishes standard and compacted initial SSE state. With XSTATE_BV[1] clear, both forms initialize XMM registers; standard format still reads and validates MXCSR, while compacted format initializes MXCSR. `X64XsaveCases.def` supplies independent packet layouts and original host XRSTOR programs. `X64XsaveTests.cpp` checks rejected-state atomicity and compares both formats with actual host execution, preserving the caller’s FP/SSE state. The host oracle skips explicitly when the architecture or required instruction feature is unavailable.

`X64FPState.def` declares compacted AVX, AVX-512, CET_U/CET_S and AMX transport layouts, including 64-byte component alignment. Present extension payloads must be architectural zero init state; absent payloads and alignment padding do not define state. Layout bits determine offsets, and unknown layouts, non-initial payloads or incorrect lengths fail before publication. `CompactedOffsetsFollowLayoutRatherThanPresentBits`, `WideLayoutIgnoresAbsentComponentsAndAlignmentPadding`, `InitialCETComponentsDoNotHideFPState` and `InitialWideComponentsDoNotHideFPState` cover 872-byte and 10752-byte WHP packets. This transport support does not admit those extension instructions.

`WhpXsaveRegisters.def` supplements complete XSAVE packets with named x87/SSE control registers. Last opcode and instruction/data pointers are written explicitly and captured from the host; zero packet slots may be supplemented, while conflicting nonzero metadata or inconsistent shared controls fail before publication. `NamedMetadataRestoresOmittedPacketFields` checks the omitted-field case without dropping any FP payload.

Native `FOP/FIP/FDP` follow the host’s x87 save/restore rules. AMD may clear these fields without a pending unmasked exception; snapshots retain observed values. `X64MachineProbe.def` and exact NOP/context tests seed a coherent pending exception so every field remains valid and is compared without masking. The host-process FXRSTOR64/FXSAVE64 oracle checks both states; backends never substitute input metadata for host results.

The shared `encodeX64XsaveState` / `decodeX64XsaveState` codec owns standard/compacted FP/SSE packets, physical TOP rotation, absent-component init state and atomic validation. WHP uses complete XSAVE APIs, preferring `WHvGetVirtualProcessorState` / `WHvSetVirtualProcessorState` with the older XSAVE APIs as a compatibility path. Legacy individual x87 registers cannot replace complete packets. Non-initial extended components, malformed headers, invalid controls and truncated captures fail explicitly. WHP mapping failures retain HRESULT, GPA and size for diagnosis.

`CheckedX64Instructions.def` admits unsigned `MUL` at 8/16/32/64 bits and `CBW/CWDE/CDQE/CWD/CDQ/CQO` through the existing processor transport. `NeverDX64IntegerTests` uses independent `X64IntegerCases.def` encodings and expected values at both privilege levels: partial-register preservation, 32-bit zero extension, both product halves, defined CF/OF results and unchanged flags for sign extension. Ordinary-RAM multiplication retains whole-span permission checks and read observers; a fault or observer stop preserves implicit output registers and PC. Device operands remain unsupported. These cases also run on checked Unicorn; unavailable native transports skip explicitly.

`WhpResourceCache.h` separates logical CPU state from WHP partitions. The runtime keeps one active native partition: consecutive steps on the same CPU reuse it; switching CPU retires the old partition before rebuilding mappings, a virtual processor and full state. Logical CPUs retain independent `MemoryProjection` views and authoritative RAM. Lease acquisition observes cancellation and the current deadline; retiring an inactive CPU cannot destroy another CPU's partition. x64 preserves the host's default XSAVE feature set and validates the effective partition via `WHvGetPartitionProperty`; it does not clear dependent features to force a reduced mask. Cooperative CPU switching does not provide parallel hardware SMP.

`CheckedAArch64Instructions.def` and `AArch64InstructionEffects` admit bounded baseline FP32/FP64 arithmetic, comparisons, moves and fixed-width SIMD operations at EL0/EL1. FPCR supports four rounding modes, FZ and DN; FPSR retains cumulative status and QC. Unsupported control/status bits are rejected before mutation. FP16 arithmetic, SVE/SME, unmasked exceptions, optional extensions and unlisted forms fail explicitly. This CPU support does not add Windows ARM64 driver loading or another OS environment.

`AArch64InstructionEffects` owns scalar and FP/SIMD single/pair RAM footprints, including operands up to 128 bits. The shared address space validates every page before CPU entry; `RAMTransaction` commits only complete declared physical writes. A 128-bit write observer receives two ordered 64-bit words before effects. Stops and faults preserve RAM, vectors and writeback. Numeric Xn/Vn overlap is valid; wrapping pair footprints are rejected. `NeverDAArch64MemoryTests` uses independent `AArch64CrossPageCases.def` and `AArch64VectorMemoryCases.def` encodings.

KVM x64/ARM64 uses `KvmRunControl` to prepare state, enter `KVM_RUN` and capture state on one private vCPU worker. Preparation runs once across `EINTR` retries; cancelled entry or failed capture cannot publish. `KvmAArch64Machine.cpp` performs translation maintenance and complete scalar/vector transfers on this worker under one step deadline. The caller publishes only after acknowledgement; ISA decoding, RAM transactions, OS policy and observers remain on the caller thread. Native ARM64 runtime evidence is still pending.

KVM compares general registers and the complete FP/SSE state against the last acknowledged debug capture using `X64HostRegisters.def` and `X64FPState.def`, and reinstalls changed input. Host writes and context restoration participate in this comparison; exceptions, cancellation and failures invalidate reuse. Stepping is armed and actual general/FP state is read back for every instruction.

Hardware execution alone does not guarantee lower end-to-end latency. Current native execution performs instruction admission, observation, state transfer and a VM exit for each step. Compare the same original images and scenarios with identical instruction/event budgets and report outcome parity alongside timings; include CLI startup and loading when measuring CLI latency.

Checked Unicorn uses `MachineRunControl`: one allowance covers ARM64 maintenance, guest execution and complete state capture. `UC_HOOK_CODE` checks the borrowed stop token and deadline at instruction entry; the synchronous engine call retires its hook borrow before returning, while the machine step retains control through publication. Unicorn and WHP stage complete CPU state and check the same control before publishing a successful step. WHP creates its allowance once before preparation. An authenticated x64 CPU exception takes precedence over a stop arriving during capture. The checked RAM transaction discards speculative stores when capture is cancelled; the unrestricted software contract is unchanged. `MachineInterruptedError` distinguishes acknowledged cancellation from host or capture failure. The shared checked CPU returns `Stopped` or `Deadline`, preserves CPU/RAM and permits retry; genuine failures remain `BackendFailure` even with a simultaneous stop.

`RunDeadline::invoke` rejects a stopped or expired WHP entry before calling the host, retains an actual host result during cancellation, and acknowledges interrupt callbacks before releasing the borrowed token. KVM and WHP validate a successfully captured private packet on the owning caller before classifying a concurrent stop or deadline. Genuine host/capture failures and authenticated x64 CPU exceptions retain priority. Ordinary successful state stays private until cancellation checks finish; an acknowledged interruption discards speculative CPU/RAM effects and permits retry. Preparation, native execution and capture share one step allowance. These controls provide cooperative cancellation, without a hard wall-clock guarantee.
