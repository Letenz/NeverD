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
| `backend` | `auto` | `auto`, `unicorn`, `kvm`, `whp` or `hvf` |
| `contract` | `software-cpu-v1` | Versioned execution semantics |
| `architecture` | `x86_64` | `x86_64` or `aarch64` |
| `privilege` | Contract profile | `flat`, `supervisor` or `user`; must match the chosen contract |
| `virtual_address_bits` | Contract profile | Checked profiles use 48 bits; flat profiles expose a 64-bit direct mapping namespace |
| `page_size` | 4096 | Guest mapping granule; other values are rejected |
| `required_features` | `[]` | Required feature names from [the inventory](../include/neverd/emulation/ExecutionConfiguration.def) |

The masked x64 instruction descriptions are the portable baseline. KVM/WHP add `precise_simd_exceptions` to `driver-strict`, `checked-x64-v1` and `checked-user-x64-v1`: the native startup probe verifies a precise `#XM` and both retries before enabling unmasked MXCSR writes, `LDMXCSR` and Windows `CONTEXT` restoration. `ExecutionProfiles.def` owns selection; `supportsSIMDExceptions` exposes the resolved instance capability. Checked Unicorn remains masked; ARM64 and HVF gain no exception capability here.

Native x64 KVM/WHP delivers actual `#XM` faults to driver C SEH through `X64SIMDException`. Hardware-fault `CONTEXT` records preserve XMM0–15 and MXCSR. Filters, exception-unwind finally callbacks and selected handlers run with MXCSR `0x1f80` and DF cleared. A negative filter can edit XMM registers and top-level `CONTEXT.MxCsr`, masked by the guest CPU profile, before retrying the original instruction; `FltSave.MxCsr` does not control kernel restoration. Modeled API raises retain integer/control records; x87/AVX context edits remain unsupported.

`driver-strict` accepts x64 with backend-qualified capabilities; `software-cpu-v1` accepts x64 and ARM64.
`checked-x64-v1` and `checked-aarch64-v1` require their named architecture
and execute at supervisor privilege. `checked-user-x64-v1` and
`checked-user-aarch64-v1` execute the corresponding bounded instruction inventory
at CPL3 and EL0, respectively, with architectural MMU isolation and explicit
service-request exits. They support Unicorn and matching-host KVM/WHP/HVF; `auto`
follows the existing host selection.
Flat profiles have no architectural user/supervisor MMU isolation contract;
their address width describes the direct mapping interface, not a claim of a
64-bit hardware virtual-address mode. Checked x64 advertises bounded SIMD and
floating-point families: legacy SSE/SSE2 moves and logic, MOVLHPS/MOVHLPS,
and masked scalar CVTTSS2SI/CVTTSD2SI/SUBSS/SUBSD. MXCSR preserves sticky status,
rounding and FTZ; portable execution rejects unmasked exceptions. Supervisor x64
additionally supports checked scalar
MMIO and prepared-read string transfers; checked user profiles reject device
mappings. All checked profiles reject port I/O and parallel CPU requirements.
The instruction inventory remains authoritative; a feature flag does not
admit every encoding in a family. Only checked user profiles
advertise `service_traps`, meaning the interception boundary described below;
supervisor and flat profiles do not advertise that boundary.

Checked x64 also admits masked legacy `ADD`, `SUB`, `MUL`, `DIV`, `SQRT`, `MIN` and `MAX` in `SS`, `SD`, `PS` and `PD` forms. `X64SSEInstructions.def` owns operand widths, alignment and admission. `MaskedSSEArithmeticMatchesIndependentHostExecution` compares register and RAM forms against an independent host CPU oracle, including all four rounding modes, FTZ, signed zero, subnormal inputs and NaNs; `SSEMemoryObserverStopsBeforeResultAndStatusChanges` verifies cancellation before effects. x87 and AVX remain excluded.

With the portable software profile, Unicorn applies DAZ to the value selected by legacy `MINSS/MINSD/MINPS/MINPD` and `MAXSS/MAXSD/MAXPS/MAXPD`. A selected subnormal input becomes signed zero; NaN payloads and existing MXCSR status are preserved. `test_x86_sse_minmax_daz` checks register, RAM and aliased forms with DAZ on/off, all rounding modes, FTZ and sticky status.

KVM/WHP now discover `MXCSR_MASK` with private `FXSAVE64` execution and verify advertised DAZ using a signed subnormal arithmetic probe. Checked Unicorn exposes its software mask. `supportedControlBits` reports the immutable CPU mask; FX/XSAVE, snapshots and Windows CONTEXT use that same capability. Checked `LDMXCSR/STMXCSR` access exactly m32, validate the complete RAM span and preserve state on fault or observer cancellation. Reserved load bits raise #GP; portable execution rejects unmasked SIMD exceptions. `X64MXCSRTests.cpp` checks controls and restart behavior; `X64DAZData` compares all 28 admitted SSE arithmetic forms against original host instructions with DAZ, rounding and FTZ. This does not extend HVF DAZ support.

Native x64 startup validation uses one `5 s` budget, including cold transport preparation and all probe steps. Guest execution budgets remain independent. Interrupted initialization reports the instruction phase, `stop_requested` and `deadline_reached`, preserving the typed interruption and underlying transport diagnostic. It does not retry or admit a partially validated CPU.

`PAUSE` (`F3 90`) executes through the shared x64 machine boundary on KVM/WHP and checked Unicorn, including native `driver-strict`. `X64PauseTests.cpp` checks complete state preservation, pre-execution stops, context restoration, invalid `LOCK` rejection and spin-loop deadline/resumption. This processor hint does not schedule guest threads or guarantee a particular delay.

`PUSHFQ` (`9C`) and 16-bit `PUSHF` (`66 9C`) run through native KVM/WHP and checked Unicorn, including `driver-strict`. The shared ISA layer removes private single-step TF from the stack image before committing the RAM transaction. Implicit stack addressing uses full RSP; prefix order determines operand width. Whole-span permissions, observer stops and fault retry preserve atomicity. `X64PushFlagsTests.cpp` covers all 256 admitted flag combinations and nine encodings, cross-page aliases, user permissions, cancellation and context restoration.

`POPFQ` (`9D`) and 16-bit `POPF` (`66 9D`) restore flags in the shared x64 ISA layer for KVM, WHP and checked Unicorn, including `driver-strict`. With the profiles’ fixed IOPL of zero, CPL0 can change IF; CPL3 preserves IF and IOPL. Reserved bits and VM/VIF/VIP are ignored, and RF clears. Effective changes to guest TF, NT, AC, ID or CPL0 IOPL remain unsupported and fail before publication. The complete stack read precedes an atomic FLAGS/RSP/RIP update; full RSP and effective prefix order determine the operand. The stack may be read-only or alias executable memory. Shared completion avoids clearing the transport’s private TF. `X64PopFlagsTests.cpp` includes independent native CPL3 instructions and checks every input bit, faults, observers and subsequent native execution.

Checked x64 admits `CLC/STC/CMC` and `LAHF/SAHF`. The carry instructions use the transport; AH transfers share one ISA transition across KVM, WHP and checked Unicorn, including native `driver-strict`. LAHF writes the five status flags and fixed bits into AH; SAHF changes only CF/PF/AF/ZF/SF. OF/IF/DF and unselected registers remain intact. Ignored prefixes, including every REX value, retain implicit AH. `X64StatusFlagsTests.cpp` checks original host instructions, full state, cancellation and continuation. The pinned Unicorn translator also preserves implicit AH under REX in the portable profile and rejects LOCK for these five instructions before state changes.

Checked x64 executes `SHLD/SHRD` with 16/32/64-bit destinations and imm8 or CL counts through KVM, WHP and Unicorn. Counts use the architectural mask; undefined 16-bit counts above 16 reject before effects. RAM destinations use exact-width read/write admission and the existing transaction: result observers run before publication, and cancellation rolls back CPU and memory. LOCK and device operands remain unsupported.

`X64ScalarShiftInstructions.def` owns the 8/16/32/64-bit destination effects of `SHL/SHR/SAR`, `ROL/ROR` and `RCL/RCR`. Register and RAM forms accept implicit 1, imm8 or CL counts, including masked-zero counts and SAL aliases. RAM writes pass through permission checks, result observers and the shared transaction; stops and callback errors restore CPU and memory. KVM, WHP and checked Unicorn use this same boundary. LOCK and MMIO forms remain unsupported.

`X64LoopInstructions.def` admits `LOOP/LOOPE/LOOPNE` through the processor transport. Address size selects RCX or zero-extended ECX, and FLAGS remain unchanged. Target width follows the CPU model: Intel ignores `66H` in long mode; AMD retains its 16-bit override, with REX.W taking precedence. Native KVM/WHP use the host processor; Unicorn uses its default Intel Haswell model. Shared admission rejects LOCK and REP before effects, even when decoder metadata omits them.

`X64BranchModel` keeps relative `JMP/Jcc` decoding consistent with execution. Native KVM/WHP measure non-taken `66H` branches and REX.W precedence during bounded startup; checked Unicorn uses its Intel model. The immutable result selects the decoder for both instruction observers and Windows driver policy. Intel retains rel32 near branches and full-width targets in long mode; AMD honors its 16-bit override. Incomplete instruction bytes fail before observers or CPU entry.

`X64StackInstructions.def` admits 16/64-bit `PUSH/POP` with general registers and ordinary RAM, plus immediate PUSH on KVM, WHP and checked Unicorn, including `driver-strict`. PUSH reads its source before decrementing RSP; POP computes an RSP/ESP-based destination after incrementing it. Address-size wrapping applies only to the explicit operand. Whole-span permissions, ordered observations and one RAM transaction preserve CPU and memory on faults, cancellation or callback errors. LOCK and device operands remain unsupported.

KVM, WHP and checked Unicorn also admit 16/64-bit `LEAVE`. It reads the saved frame through full RBP, including with `67H`, and preserves unselected RBP bits in the 16-bit form. Faults or cancelled reads retain the original RSP and CPU context. LOCK, REP and device frames remain unsupported.

KVM, WHP and checked Unicorn admit 16/64-bit `ENTER`, with unsigned 16-bit allocation and nesting modulo 32. Full RSP/RBP and effective prefix order determine the accesses. On a guest fault, completed stack stores remain visible while RSP, RBP and PC retain their entry values. The final stack check validates write permission over the operand width without storing data. LOCK, REP, APX prefixes and device frames remain unsupported.

`X64PackedIntegerInstructions.def` admits 45 legacy SSE2 packed integer operations: wrapping and saturating addition/subtraction, comparisons, multiplication, averages, extrema, byte differences, packing and unpacking. XMM and aligned 128-bit RAM sources share the existing checked path on KVM, WHP and Unicorn. FLAGS and MXCSR remain unchanged; faults or observer cancellation preserve state. MMX, VEX/EVEX and device operands remain excluded.

`X64PackedShiftInstructions.def` admits ten legacy SSE2 packed shifts. Lane shifts accept imm8 or XMM/aligned m128 counts; byte shifts accept imm8 only. Variable counts use the unsigned low 64 bits without scalar count masking; the high 64 bits are ignored. Memory operands still require a complete 16-byte read for zero or oversized counts. FLAGS and MXCSR remain unchanged; MMX, VEX/EVEX and device operands remain excluded.

`X64VectorOperands.def` owns complete operand rules for legacy SSE moves, arithmetic, shifts, conversions and masks. `MOVMSKPS`, `MOVMSKPD` and `PMOVMSKB` extract XMM sign bits into r32/r64 and zero the remaining destination bits. KVM, WHP and checked Unicorn share this admission; FLAGS, MXCSR and source registers are preserved. Mask memory operands, MMX and VEX/EVEX forms remain unsupported.

`X64ShuffleInstructions.def` adds `PSHUFD`, `PSHUFHW`, `PSHUFLW`, `SHUFPS` and `SHUFPD`. `X64VectorOperands.def` requires the complete three-operand form: XMM destination, XMM or aligned m128 source, and imm8. Original instructions select raw lanes without changing FLAGS or MXCSR. Memory forms validate all 16 bytes; alignment faults precede data observations. KVM, WHP and checked Unicorn share these rules. The same inventory admits `UNPCKLPS`, `UNPCKHPS`, `UNPCKLPD` and `UNPCKHPD` with exactly two operands. They interleave raw elements from the original destination and source. Hardware may fetch only the selected 64 bits; checked RAM validates the aligned m128 operand.

`MOVLPS`, `MOVHPS`, `MOVLPD` and `MOVHPD` transfer exactly eight RAM bytes without an alignment requirement. `X64VectorInstructions.def` declares the store half; `X64VectorOperands.def` requires an XMM/m64 pair. Loads preserve the other 64 bits, and high-half store observers receive the upper half. KVM, WHP and checked Unicorn share whole-span permission checks and RAM rollback. Register-only `MOVHLPS`/`MOVLHPS` retain their distinct semantics.

`CVTSI2SS` and `CVTSI2SD` convert signed 32/64-bit integers using MXCSR rounding and retain precision status. The shared `IntegerSource` rule admits only XMM destinations with r32/r64 or m32/m64 sources. Legacy instructions preserve the upper 96/64 destination bits; memory checks use the integer width. KVM, WHP and checked Unicorn execute the original instruction. MMX and VEX/EVEX remain excluded.

`CVTSS2SI` and `CVTSD2SI` use MXCSR rounding to produce signed 32/64-bit integers through the shared `IntegerResult` rule; `CVTTSS2SI` and `CVTTSD2SI` always truncate. Masked NaN or out-of-range conversions return the integer indefinite and set invalid status; valid inexact results set precision status. Existing sticky bits, FLAGS and XMM sources are preserved. An r32 result clears the upper GPR half. RAM reads use the floating source width, independent of destination width; FTZ does not discard subnormal inputs. These rules apply to KVM, WHP and checked Unicorn.

`COMISS`, `COMISD`, `UCOMISS` and `UCOMISD` compare scalar XMM or m32/m64 operands through the shared `Source` rule. They set CF/PF/ZF, clear OF/SF/AF and preserve other FLAGS and source lanes. COMIS signals invalid for any NaN; UCOMIS does so only for signaling NaNs. NaN handling precedes denormal status. MXCSR sticky bits are retained; rounding and FTZ do not change comparison. KVM, WHP and checked Unicorn share exact memory checks. The pinned Unicorn comparison helpers reuse its denormal-input classifier.

`CMPSS`, `CMPSD`, `CMPPS` and `CMPPD` execute the eight legacy predicates on KVM, WHP and checked Unicorn. The shared `Source` rule accepts decoded predicate aliases; reserved controls remain unsupported. Scalar forms preserve upper lanes and use m32/m64; packed forms require aligned m128. FLAGS and existing MXCSR status are preserved, with invalid/denormal status accumulated per active lane. Capstone owns the family IDs and SSE conditions, replacing the lifter-only identity repair. Unicorn classifies denormal inputs inside each comparison helper.

`CVTSS2SD`, `CVTSD2SS`, `CVTPS2PD` and `CVTPD2PS` convert legacy SSE precision through the shared `Source` rule. Scalar results retain the upper 64/96 destination bits. Packed widening reads m64 and writes two doubles; packed narrowing reads aligned m128, writes two singles and clears the upper 64 bits. Original execution on KVM, WHP and checked Unicorn preserves FLAGS and accumulates masked MXCSR status under the selected rounding and FTZ controls. Unicorn classifies each active denormal input in its conversion helpers. VEX/EVEX remain excluded.

`CVTDQ2PS` and `CVTDQ2PD` convert packed signed 32-bit integers through the shared `Source` rule. Single precision consumes aligned m128 and uses MXCSR rounding; double precision consumes unaligned m64 and is exact. All destination XMM bits are replaced, FLAGS and existing MXCSR status are preserved, and inexact single results accumulate precision status. KVM, WHP and checked Unicorn execute the original instructions. Unicorn identifies both packed widening helpers before selecting the eight-byte load. MMX and VEX/EVEX remain excluded.

`CVTPS2DQ` and `CVTPD2DQ` use MXCSR rounding; `CVTTPS2DQ` and `CVTTPD2DQ` truncate. The shared `Source` rule requires aligned m128 or XMM inputs. Each NaN or out-of-range lane produces signed32 indefinite and invalid status; valid inexact lanes independently add precision status. Single inputs produce four integers; double inputs produce two and clear the upper 64 destination bits. FLAGS and existing MXCSR status are preserved; FTZ does not discard subnormal inputs. KVM, WHP and checked Unicorn execute the original instructions. MMX and VEX/EVEX remain excluded.

`X64AlignmentTests.cpp` checks that misaligned operands of admitted aligned SSE instructions report recoverable or terminal `#GP(0)` before data observers, permission checks or device callbacks. Faults retain the complete public x64 register context, PC and RAM; address-size wrapping precedes FS/GS addition, and repairing the address retries the original instruction. Direct KVM/WHP machine cases independently verify the hardware boundary. Windows ring3 delivers classified `operand_alignment` faults; other `#GP` causes remain unsupported.

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

`CMPXCHG8B` and `CMPXCHG16B` execute their original encodings on KVM, WHP and checked Unicorn in driver and user profiles. Successful and failed comparisons both require read/write access; faults are classified as writes. `CMPXCHG16B` checks 16-byte alignment before memory access and reports `#GP(0)`. Its two result observations share one RAM transaction: stopping or throwing in either publishes no registers or RAM. Unlocked `CMPXCHG8B` may cross pages; locked operands retain the natural-alignment contract. `X64WideAtomicTests.cpp` compares original host results and direct native faults, aliases, prefixes, address rules, repair and cancellation. Original Windows driver and ring3 PE fixtures exercise both widths; the WDK fixture also executes `_InterlockedCompareExchange128`. The CPU model must support `CMPXCHG16B`.

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
of memory. Exact FPCR/FPSR accesses and `MRS CTR_EL0` are admitted separately; other ARM64
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

`driver-strict` supports KVM on matching Linux x64 hosts and WHP on matching Windows x64 hosts; `auto` selects that native transport, and cross-ISA execution selects Unicorn. Explicit Unicorn and the original V1 API retain the portable software profile. Native execution checks canonical addresses and instruction effects before entry; unavailable hardware fails without fallback. Unsupported instructions and OS behavior remain explicit errors. Native Windows x64 CI with Unicorn disabled passes all 359 required checks: 131 CPU checks, 224 driver outcomes from 26 built-in images, 46 WDK images and 40 scenario cases at both preferred and relocated bases, plus four SEH boundary checks ([`9d4c130c`](https://github.com/NeverSight/NeverD/actions/runs/36981864458)). Native ARM64 runtime evidence is still pending, and this does not establish arbitrary-driver or Android/Darwin compatibility.

Use `executionCapabilities(Contract, ISA, Backend)` to query the selected profile. `NativeLegacyX64` describes native x64 driver execution. `NeverDNativeDriverTests` validates the original corpus and can run with Unicorn disabled.

Checked ARM64 has one complete state boundary. `Registers.def` defines 39 scalar fields and 32 128-bit vectors; `captureAArch64State` stages every read, applies declared widths and NZCV normalization, then publishes once. Unicorn, KVM, WHP and HVF transfer the same inventory, including TPIDR_EL0, TPIDRRO_EL0, TPIDR_EL1, FPCR and FPSR. Native adapters enable FP/SIMD through CPACR_EL1. Any scalar/vector read failure or cancelled entry preserves all caller state.

ARM64 KVM/WHP/HVF startup executes the private `AArch64MachineProbe.def` program: NOP, FP32 addition rounded toward positive infinity, a two-lane SIMD addition, A/B return-address signing/authentication with the keys disabled, and all four BTI forms on unguarded pages. Each step compares all 39 scalar fields and 32 vectors, including TLS, NZCV, cleared upper destination bits and retained/cumulative FPCR/FPSR state. The probe uses supervisor monitor storage and one overall deadline. The probes establish bounded initialization only. Linux ARM64 KVM and Windows ARM64 WHP workload validation remains pending; native macOS results are recorded in the [HVF guide](macos-hvf.md).

The probe also executes `MRS CTR_EL0` twice, `DC CVAU`, `DSB ISH`, `IC IVAU` and `ISB`, checking stable cache geometry and complete state. Checked EL0/EL1 execution admits the original words, all named baseline DSB options and only ISB SY. CTR comes from the selected virtual CPU and may differ between transports. Cache targets must identify readable ordinary RAM with the current privilege, including unaligned addresses and aliases; other targets fail as unsupported. Maintenance produces no data read/write observer events. The projection keeps instruction execution coherent; it does not model private cache contents or parallel hardware SMP. `NeverDAArch64CacheTests` checks full state, read-only page ends, rejected forms, stops, contexts, budgets and guest code updates through cross-page RW/RX aliases. Unavailable KVM/WHP hosts remain explicit skips.

Native x64 KVM/WHP/HVF initialization executes `X64MachineProbe.def` in private supervisor pages. One deadline covers NOP, rounded FP32 addition, two-lane SIMD addition, FS/GS loads and CS/SS/CR8 reads; every step compares the complete scalar, XMM, physical x87 and control state. x64 and ARM64 probes require the exclusive physical-memory execution lease. `MemoryProjection` owns cache identity (ISA, address space, mapping generation, privilege and monitor variant) and committed root history per ISA. Builders invalidate before rewriting private bytes; failed replacement cannot reuse partially written tables, and callers cannot supply stale roots. The probes establish bounded initialization only. Linux ARM64 KVM and Windows ARM64 WHP workload validation remains pending; native macOS results are recorded in the [HVF guide](macos-hvf.md).

The shared XSAVE decoder distinguishes standard and compacted initial SSE state. With XSTATE_BV[1] clear, both forms initialize XMM registers; standard format still reads and validates MXCSR, while compacted format initializes MXCSR. `X64XsaveCases.def` supplies independent packet layouts and original host XRSTOR programs. `X64XsaveTests.cpp` checks rejected-state atomicity and compares both formats with actual host execution, preserving the caller’s FP/SSE state. The host oracle skips explicitly when the architecture or required instruction feature is unavailable.

`X64FPState.def` declares compacted AVX, AVX-512, CET_U/CET_S and AMX transport layouts, including 64-byte component alignment. Present extension payloads must be architectural zero init state; absent payloads and alignment padding do not define state. Layout bits determine offsets, and unknown layouts, non-initial payloads or incorrect lengths fail before publication. `CompactedOffsetsFollowLayoutRatherThanPresentBits`, `WideLayoutIgnoresAbsentComponentsAndAlignmentPadding`, `InitialCETComponentsDoNotHideFPState` and `InitialWideComponentsDoNotHideFPState` cover 872-byte and 10752-byte WHP packets. This transport support does not admit those extension instructions.

`WhpXsaveRegisters.def` supplements complete XSAVE packets with named x87/SSE control registers. Last opcode and instruction/data pointers are written explicitly and captured from the host; zero packet slots may be supplemented, while conflicting nonzero metadata or inconsistent shared controls fail before publication. `NamedMetadataRestoresOmittedPacketFields` checks the omitted-field case without dropping any FP payload.

Native `FOP/FIP/FDP` follow the host’s x87 save/restore rules. AMD may clear these fields without a pending unmasked exception; snapshots retain observed values. `X64MachineProbe.def` and exact NOP/context tests seed a coherent pending exception so every field remains valid and is compared without masking. The host-process FXRSTOR64/FXSAVE64 oracle checks both states; backends never substitute input metadata for host results.

The shared `encodeX64XsaveState` / `decodeX64XsaveState` codec owns standard/compacted FP/SSE packets, physical TOP rotation, absent-component init state and atomic validation. WHP uses complete XSAVE APIs, preferring `WHvGetVirtualProcessorState` / `WHvSetVirtualProcessorState` with the older XSAVE APIs as a compatibility path. Legacy individual x87 registers cannot replace complete packets. Non-initial extended components, malformed headers, invalid controls and truncated captures fail explicitly. WHP mapping failures retain HRESULT, GPA and size for diagnosis.

`CheckedX64Instructions.def` admits unsigned `MUL` at 8/16/32/64 bits and `CBW/CWDE/CDQE/CWD/CDQ/CQO` through the existing processor transport. `NeverDX64IntegerTests` uses independent `X64IntegerCases.def` encodings and expected values at both privilege levels: partial-register preservation, 32-bit zero extension, both product halves, defined CF/OF results and unchanged flags for sign extension. Ordinary-RAM multiplication retains whole-span permission checks and read observers; a fault or observer stop preserves implicit output registers and PC. Device operands remain unsupported. These cases also run on checked Unicorn; unavailable native transports skip explicitly.

`X64BitInstructions.def` admits register and ordinary-RAM `BT/BTS/BTR/BTC` at 16/32/64 bits. A register bit index is signed at the operand width and selects a complete word; an immediate stays within the base word. Address-size wrapping occurs before FS/GS base addition. The processor supplies CF and written values; `RAMTransaction` keeps the result private until observers accept it. Whole-span permission checks cover separate page allocations and aliases. Stops, callback failures and denied pages preserve the original CPU and RAM. LOCK is limited to naturally aligned modifying memory forms; MMIO and parallel hardware SMP remain unsupported. `X64BitStringTests.cpp` compares independent encodings with actual x64 host execution and checks negative indices, width truncation, cross-page accesses, cancellation and invalid LOCK forms. See the [Intel instruction reference](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).

`X64StringInstructions.def` owns ordinary-RAM `MOVS/STOS/LODS` at 8/16/32/64 bits; `CLD/STD` controls direction without changing other flags. Each REP element validates the entire operand before observations and commits at one restart boundary. Earlier completed elements survive a later fault; cancellation or observer failure leaves the current element untouched. FS/GS applies only to the source, after address-size truncation. AL/AX loads preserve upper bits and EAX loads zero-extend. Zero-count address-size-32 REP requires zero upper count bits and, for MOVS/STOS, zero upper participating address bits: real CPU implementations differ otherwise. REPNE on MOVS/STOS/LODS and STOS/LODS device operands remain unsupported. `X64StringTransferTests.cpp` uses independent host instructions for widths, direction, overlap and zero counts, with separate checks for permissions, aliases, wraparound, faults and resumption. The original WDK resource driver executes all four STOS/LODS widths through `driver_resource_strings.def`.

`X64StringInstructions.def` also owns ordinary-RAM `CMPS/SCAS` at 8/16/32/64 bits with `REPE/REPNE`. Every element validates both complete read operands before observers, updates all six arithmetic flags, and stops on the first matching termination condition. A data fault restores the flags from entry to this uninterrupted REP while retaining completed pointer/count changes; a public resume starts from the published CPU state. Stops and observer exceptions leave the current element untouched. Early termination never reads the next element. FS/GS affects only the CMPS source; SCAS leaves the accumulator and unused source register unchanged. Device operands and ambiguous inactive 32-bit upper halves remain excluded. `X64StringComparisonTests.cpp` compares independent host instructions, flags, direction, aliases, wrapping, permissions and recovery; its Linux x64 signal oracle checks actual fault-time registers. The original WDK resource driver executes both conditional-repeat forms at all four widths through `driver_resource_strings.def`. See the [Intel instruction reference](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html). The Linux native oracle checks faults before and after the first element. It distinguishes Intel entry-flag restoration from the last-comparison flags observed on AMD EPYC 7763 under Hyper-V ([native observations](https://github.com/NeverSight/NeverD/actions/runs/37202522130)); unknown CPU vendors fail explicitly. Checked guests keep entry-flag restoration on every backend.

`WhpResourceCache.h` separates logical CPU state from WHP partitions. The runtime keeps one active native partition: consecutive steps on the same CPU reuse it; switching CPU retires the old partition before rebuilding mappings, a virtual processor and full state. Logical CPUs retain independent `MemoryProjection` views and authoritative RAM. Lease acquisition observes cancellation and the current deadline; retiring an inactive CPU cannot destroy another CPU's partition. x64 preserves the host's default XSAVE feature set and validates the effective partition via `WHvGetPartitionProperty`; it does not clear dependent features to force a reduced mask. Cooperative CPU switching does not provide parallel hardware SMP.

`CheckedBackend` owns one fetch buffer and one `cs_disasm_iter` instruction record per CPU. Every step rereads permitted bytes and decodes again; no decoded instruction is cached across code writes, alias changes or resumption. The execution lease rejects recursive entry before reused storage is touched. This removes per-instruction buffer/record allocation while preserving instruction observations, service interception and precise fault handling. The pinned Unicorn single-step path ends before fetching a successor, including indirect translation lookup, and avoids counting an internal code-write retry as a completed instruction.

`WhpX64Partition.h` owns x64 WHP register reuse for one actual partition. Fixed packets use `WhpX64Registers.def` and `X64HostRegisters.def`; every successful step still captures general, control, segment and complete FP/SSE state. Only fully acknowledged debug exits permit skipping unchanged inputs. Comparison ignores reserved bits and union padding. Changed CR3, CPL, TLS, GPR or FP inputs are installed; partial failure, cancellation and exceptions invalidate reuse. Partition replacement starts with a full install. This reduces redundant transfers, without extending instruction admission or claiming an end-to-end speedup.

WHP captures the x87/SSE metadata from `WhpXsaveRegisters.def` in the same `WHvGetVirtualProcessorRegisters` call as the ordinary registers. The stopped vCPU remains under one partition lease. Complete XSAVE capture and all metadata consistency checks still precede publication; this removes one host API call per step, without a measured throughput claim.

`CheckedAArch64Instructions.def` and `AArch64InstructionEffects` admit bounded baseline FP32/FP64 arithmetic, comparisons, moves and fixed-width SIMD operations at EL0/EL1. FPCR supports four rounding modes, FZ and DN; FPSR retains cumulative status and QC. Unsupported control/status bits are rejected before mutation. FP16 arithmetic, SVE/SME, unmasked exceptions, optional extensions and unlisted forms fail explicitly. This CPU support does not add Windows ARM64 driver loading or another OS environment.

The checked SIMD subset includes scalar-D and fixed-width vector `CMHI`
unsigned comparisons. Each element produces an all-ones or zero mask; narrow
and scalar results clear the unused high destination bits. Independently
assembled tests cover all baseline arrangements, overlapping registers, equal
operands and reserved encodings, and compare the complete scalar/vector state
on available transports. See Arm's [Neon comparison reference](https://arm-software.github.io/acle/neon_intrinsics/advsimd.html#greater-than).

The checked EL0/EL1 ARM64 contracts keep all `SCTLR_EL1` pointer-authentication
key enables clear. `AArch64PAuthHints.def` admits only the twelve HINT-space
IA/IB signing/authentication words using x16/x17, zero or SP. They execute
unchanged through the selected transport, preserve registers and memory, and
consume an ordinary instruction attempt. This follows the architectural
[disabled-extension compatibility behavior](https://www.kernel.org/doc/html/v6.6/arch/arm64/pointer-authentication.html).
Native startup checks both return-address keys against the full register
inventory; a transport that changes LR cannot satisfy this contract. Unknown
HINTs, strip operations, non-HINT authentication, authenticated branches/loads,
key registers and attempts to change `SCTLR_EL1` remain unsupported. Active
PAuth, signing keys and authenticated guest pointers are not modeled. Linux
startup continues to advertise no PAuth hardware capability.

`AArch64BTIHints.def` admits the four exact `BTI`, `BTI c`, `BTI j` and
`BTI jc` words in the same checked contracts. All projected leaves have GP
clear; guarded pages and branch-type state are not exposed by this machine.
Under this contract each original word executes through the transport,
preserves scalar/vector/memory state and consumes an ordinary instruction
attempt. Native startup tests all four encodings, including on processors
with FEAT_BTI. This follows Arm's [BTI instruction definition](https://documentation-service.arm.com/static/68da52dfbd7cab51328c0622).
Admission does not enable guarded-page branch-target checking, interpret GNU
BTI properties as enforcement, or advertise the BTI hardware capability to a
guest OS. Unlisted HINTs and system-control writes remain unsupported.

The original baseline `LDAR`, `LDARB`, `LDARH`, `STLR`, `STLRB` and `STLRH` execute through the selected transport on naturally aligned ordinary RAM. Their narrow loads zero-extend; invalid alignment and optional ordered encodings remain unsupported.

Checked ARM64 supports 8/16/32/64-bit `LDXR/STXR`, 32/64-bit register pairs `LDXP/STXP`, their acquire/release variants, and `CLREX`. A shared ISA monitor models 16-byte physical reservation granules for KVM, WHP and checked Unicorn; transport single-step exits do not destroy loop progress. Committed writes invalidate reservations even when bytes remain unchanged, including writes through aliases and retained views. Stops preserve unpublished state; snapshots cannot undo intervening writes. Checked exclusives use FEAT_LSE2 alignment: unaligned operands within one 16-byte aligned block execute; crossing that block raises an `alignment` fault. Equal-width stores match the reserved physical granule. Alignment and permissions are checked before conditional-store success, including when the monitor has expired. MMIO, optional LSE atomics and parallel SMP remain outside this profile. ARM64 Unicorn also uses this monitor in the `Software` contract, including mixed software/checked CPUs sharing physical RAM. See the [Arm instruction definitions](https://documentation-service.arm.com/static/67e40f3398aa3c3b6eea6a85). Unicorn’s `Software` profile retains its engine’s natural-alignment model.

`AArch64InstructionEffects` owns scalar and FP/SIMD single/pair RAM footprints, including operands up to 128 bits. The shared address space validates every page before CPU entry; `RAMTransaction` commits only complete declared physical writes. A 128-bit write observer receives two ordered 64-bit words before effects. Stops and faults preserve RAM, vectors and writeback. Numeric Xn/Vn overlap is valid; wrapping pair footprints are rejected. `NeverDAArch64MemoryTests` uses independent `AArch64CrossPageCases.def` and `AArch64VectorMemoryCases.def` encodings.

KVM x64/ARM64 uses `KvmRunControl` to prepare state, enter `KVM_RUN` and capture state on one private vCPU worker. Preparation runs once across `EINTR` retries; cancelled entry or failed capture cannot publish. `KvmAArch64Machine.cpp` performs translation maintenance and complete scalar/vector transfers on this worker under one step deadline. The caller publishes only after acknowledgement; ISA decoding, RAM transactions, OS policy and observers remain on the caller thread. Native ARM64 runtime evidence is still pending.

KVM compares general registers and the complete FP/SSE state against the last acknowledged debug capture using `X64HostRegisters.def` and `X64FPState.def`, and reinstalls changed input. Host writes and context restoration participate in this comparison; exceptions, cancellation and failures invalidate reuse. Stepping is armed and actual general/FP state is read back for every instruction.

On x64, KVM queries `KVM_CAP_SYNC_REGS` and independently uses the supported `KVM_SYNC_X86_REGS` and `KVM_SYNC_X86_SREGS` capture sets. An acknowledged `KVM_RUN` returns actual registers through shared storage; a continued successful step can avoid both `KVM_GET_REGS` and `KVM_GET_SREGS`. Unsupported sets or a failed optional query retain the ioctl path. Changed inputs are still installed before `KVM_SET_GUEST_DEBUG`; shared dirty bits stay clear. Exceptions, cancellation and capture failure invalidate reuse. Complete FP/SSE capture remains mandatory, while unchanged FP input avoids redundant XSAVE encoding. This reduces transfer calls, without a claim of end-to-end speedup. [KVM API](https://docs.kernel.org/virt/kvm/api.html#kvm-cap-sync-regs).

Hardware execution alone does not guarantee lower end-to-end latency. Current native execution performs instruction admission, observation, state transfer and a VM exit for each step. Compare the same original images and scenarios with identical instruction/event budgets and report outcome parity alongside timings; include CLI startup and loading when measuring CLI latency.

Checked Unicorn uses `MachineRunControl`: one allowance covers ARM64 maintenance, guest execution and complete state capture. `UC_HOOK_CODE` checks the borrowed stop token and deadline at instruction entry; the synchronous engine call retires its hook borrow before returning, while the machine step retains control through publication. Unicorn and WHP stage complete CPU state and check the same control before publishing a successful step. WHP creates its allowance once before preparation. An authenticated x64 CPU exception takes precedence over a stop arriving during capture. The checked RAM transaction discards speculative stores when capture is cancelled; the unrestricted software contract is unchanged. `MachineInterruptedError` distinguishes acknowledged cancellation from host or capture failure. The shared checked CPU returns `Stopped` or `Deadline`, preserves CPU/RAM and permits retry; genuine failures remain `BackendFailure` even with a simultaneous stop.

`RunDeadline::invoke` rejects a stopped or expired WHP entry before calling the host, retains an actual host result during cancellation, and acknowledges interrupt callbacks before releasing the borrowed token. KVM and WHP validate a successfully captured private packet on the owning caller before classifying a concurrent stop or deadline. Genuine host/capture failures and authenticated x64 CPU exceptions retain priority. Ordinary successful state stays private until cancellation checks finish; an acknowledged interruption discards speculative CPU/RAM effects and permits retry. Preparation, native execution and capture share one step allowance. These controls provide cooperative cancellation, without a hard wall-clock guarantee.

## macOS

The `hvf` backend uses Hypervisor.framework for the native host ISA: ARM64 on
Apple Silicon and x86-64 on Intel. See [HVF setup and validation](macos-hvf.md)
for executable signing, ownership, cancellation, tests and validation limits.
