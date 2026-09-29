# Guest process emulation

`neverd emulate` executes an image under an explicit guest OS profile. CPU
transport, image parsing, process entry and OS services have separate owners.
Build with `NEVERD_ENABLE_CPU_EMULATION=ON`; driver emulation also includes it.

The first profile, `linux-elf64-v1`, runs bounded little-endian x64 and AArch64
ELF `ET_EXEC` programs at CPL3 or EL0. It loads real ELF segments, constructs
the initial stack, resumes instruction quanta and handles explicit Linux
system-call requests. It is a freestanding process model, not a full Linux
distribution or a promise to run arbitrary libc binaries. Dynamic linking,
`PT_TLS`, signals, threads, file systems and unsupported services fail
explicitly. Windows user processes, Android, Darwin and other kernel workloads
remain separate implementation work.

## CLI and SDK

```bash
neverd emulate guest.elf --profile=linux-elf64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"],"instruction_limit":100000}'
```

Matching Linux hosts select KVM and matching Windows hosts select WHP. Other
host/guest ISA combinations use Unicorn. An unavailable selected backend is an
error, with no silent fallback. An ELF guest still uses the Linux process model
when executed on Windows. See [CPU execution](cpu-execution.md) for the checked
instruction inventory, native availability and cancellation limitations.
The x64 profile admits the listed SSE/SSE2 moves, logical operations and masked
scalar subtraction/conversion forms; AArch64 remains an integer instruction
profile. Vector register storage does not imply an unrestricted SIMD ISA.

The CLI emits one JSON report. Its exit code is 0 for a guest exit status of
zero, 2 for another guest exit status, 3 for incomplete execution (including
faults and limits), and 1 for invalid setup/API failure. The actual guest status
is in `exit_status`; it is not substituted for the CLI exit code.

The additive C entry point is
[`neverd_emulate_process_json`](../include/neverd/sdk/NeverDCAPIProcess.h).
Pass a session, nonempty input path, explicit profile and optional options JSON.
Free its result with `neverd_free_string`. A null result is a setup failure;
read `neverd_last_error`. A guest fault or resource stop returns a report. The
session's loaded analysis image is neither required nor changed.

Python exposes the same native validation:

```python
report = session.emulate_process(
    "guest.elf", "linux-elf64-v1",
    '{"backend":"unicorn","arguments":["guest"],"environment":[]}',
)
output = bytes.fromhex(report["stdout_hex"])
```

## Options and results

Options are a JSON object of at most 64 KiB. Unknown fields, null field values,
invalid types, embedded NULs in strings and nonpositive limits are rejected.

| Option | Default | Contract |
|--------|---------|----------|
| `backend` | `auto` | `auto`, `unicorn`, `kvm` or `whp` |
| `arguments` | Input filename | Complete argv, including argv[0]; empty selects the default |
| `environment` | `[]` | Explicit guest strings; never inherits the host environment |
| `instruction_limit` | 100000 | Shared admitted instruction attempts |
| `event_limit` | 10000 | System-call events; charged before OS service handling |
| `timeout_microseconds` | 5000000 | One monotonic deadline starting after process setup |
| `memory_limit` | 67108864 | Physical/mapped memory budget |
| `stack_size` | 1048576 | Page-aligned stack within the memory budget |
| `output_limit` | 1048576 | Combined captured stdout/stderr bytes |
| `instruction_quantum` | 1024 | Admission interval before yielding to the runtime |

`schema_version` is 1. Results include profile, architecture, selected backend
and its selection reason, `stop_reason`, nullable `exit_status`, diagnostic,
entry/current PC, instruction/event counters, service records and the last
typed CPU exit. Addresses, syscall numbers, argument registers and raw return
bits are hexadecimal strings **without** a `0x` prefix; they never lose bits
through a JSON floating-point consumer. `stdout_hex` and `stderr_hex` are
lowercase byte encodings, preserving NUL and invalid UTF-8. A null syscall
result denotes no modeled return, including process exit or an unsupported
request; it is distinct from a successful zero return.

## Linux profile semantics

The existing ELF loader supplies decoded program headers. OS policy validates
ABI tags, segment alignment, mapped program-header tables and user-address
bounds before execution. A generic mapping plan checks extents, permissions,
overlap and budget before allocation; the model publishes only a fully prepared
private address space. Linux mappings preserve file-page prefix/tail bytes,
zero BSS, honor segment permissions and reserve stack guard gaps. Page-overlap
layouts and contradictory headers are rejected rather than guessed.

The initial stack contains aligned argc/argv/envp/auxv, mapped PHDR/PHENT/PHNUM,
entry/page-size values and identity entries. Model PID/TID/UID/GID are 1000.
`AT_RANDOM` contains the first 16 bytes of the input's SHA-256 for reproducible
execution; this is explicitly a deterministic model policy, not cryptographic
entropy. HWCAP/HWCAP2 are zero; there is no vDSO. Startup conventions follow the
[Linux ELF loader](https://github.com/torvalds/linux/blob/master/fs/binfmt_elf.c).

Implemented calls are `write`, `exit`, `exit_group`, `getpid` and `gettid`, with
separate [x64](https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl)
and [asm-generic ARM64](https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/unistd.h)
numbers. Returning x64 SYSCALL applies its RCX/R11 clobbers as well as RAX and
the next PC. ARM64 uses x8 for the number and x0 for the result. Unknown calls
stop as `unsupported_service`; they never execute host syscalls.

Descriptors 1 and 2 are virtual byte sinks. `write` validates readable user
pages, returns a readable prefix when a later page is inaccessible, and returns
guest `EFAULT` when no bytes are readable. A bad descriptor returns `EBADF`;
a zero-count write on a valid descriptor succeeds without pointer access.
These sinks do not model Linux pipe atomicity or file objects. Native fixture
comparison agrees on ordinary output and on partial writes to regular files;
Linux pipes can reject that same small cross-page write completely. Output
limits stop before publishing an over-budget write.

## Verification

```bash
cmake --build build-cpu --target NeverDLinuxProcessTests NeverDExecutionSessionTests NeverDX64MemoryUpdateTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(LinuxProcess|ExecutionSession|X64MemoryUpdate)Tests$' --output-on-failure
# In a shared-library/CLI build:
cmake --build build-cpu --target NeverDProcessPublicTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDProcessPublicTests$' --output-on-failure
```

Tests compile original freestanding ELF entry assembly and C for both ISAs.
They exercise data/BSS, actual startup metadata, syscall errors, binary output,
permission faults, partial writes, unsupported services and budget preservation
across quanta. Backend cells report unavailable transports as skips. The public
suite traverses the shared C ABI and CLI and checks report/exit-code agreement.
Cross-compilation and Unicorn ARM64 results are not native ARM64 KVM/WHP evidence.
