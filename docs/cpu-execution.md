# CPU configuration and capability queries

CPU execution is independent of the guest OS, image loader and calling
convention. Enable `NEVERD_ENABLE_CPU_EMULATION` to build it alone, or
`NEVERD_ENABLE_DRIVER_EMULATION` to include the Windows driver environment.
The [architecture guide](architecture.md#cpu-execution) describes ownership,
backend selection and current platform limitations.

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
| `privilege` | Contract profile | `flat`, `supervisor` or `user`; `user` is currently rejected |
| `virtual_address_bits` | Contract profile | Checked profiles use 48 bits; flat profiles expose a 64-bit direct mapping namespace |
| `page_size` | 4096 | Guest mapping granule; other values are rejected |
| `required_features` | `[]` | Required feature names from [the inventory](../include/neverd/emulation/ExecutionConfiguration.def) |

`driver-strict` accepts x64; `software-cpu-v1` accepts x64 and ARM64.
`checked-x64-v1` and `checked-aarch64-v1` require their named architecture.
Flat profiles have no architectural user/supervisor MMU isolation contract;
their address width describes the direct mapping interface, not a claim of a
64-bit hardware virtual-address mode. Checked profiles execute at supervisor
privilege with their fixed page-table model. Saving vector register state does
not establish permission to execute SIMD: checked profiles reject FP/SIMD,
MMIO, port I/O, service traps, user isolation and parallel CPU requirements.

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
startup probe, and does not certify a workload or native ARM64 execution.
Availability can change after the query. Reasons distinguish disabled builds,
host platform/ISA mismatches, device access, host API errors, missing capability
and other initialization failures. No unavailable backend silently falls back.

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

Native execution checks normal budgets between instructions. WHP also requests
cancellation of an active native entry after its transport allowance or a stop
request. An interrupted entry with uncertain guest progress is terminal, even
when `StopRequested` or `DeadlineReached` is set. KVM checks those requests
before entry and after host interruptions; an uninterrupted entry still relies
on hardware single stepping. No profile promises a hard wall-clock bound.

`CPU.runUntilExit(PC, TimeoutMicroseconds)` returns a typed
[`ExecutionExit`](../include/neverd/emulation/ExecutionExit.h). Preconditions
and setup failures return `llvm::Error`; runs that begin execution report an
explicit stop, deadline, recoverable fault, guest fault/trap, unsupported
operation, device failure, backend failure or otherwise unexplained engine
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

A stopped CPU, software HLT, deadline or guest trap never establishes successful
workload completion. Instruction/event budgets, service dispatch, process/thread
exit, exception delivery and workload success remain runtime/OS decisions.
General runtime extraction, real user privilege, independently interruptible KVM
and the additional OS workloads remain unfinished; these queries do not claim
those capabilities.
