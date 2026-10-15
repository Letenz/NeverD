# Explicit Linux device transactions

Linux ELF64 and Android API 28 workloads can supply device sessions under
`linux_files.devices`. This is an input contract for analysis, not a kernel
driver, inferred hardware behavior or access to the host's devices. An absent
or mismatched transaction stops execution; it never supplies implicit success.
Recorded and synthetic inputs must be distinguished by the caller.

```json
{"linux_files":{"files":[],"devices":[{
  "path":"/fixture/control","access_mode":2,"open_errno":0,
  "ioctls":[{"request":2701131777,"result":0,"memory":[
    {"offset":0,"expected_hex":"3132","output_hex":"5253"}
  ]}]
}]}}
```

C++ uses `LinuxFileOptions::Devices` and the types in
[`LinuxDeviceOptions.h`](../include/neverd/emulation/LinuxDeviceOptions.h).
The C ABI, CLI and Python use the same process JSON parser and Linux owner.

Each device requires `path`, `access_mode`, `open_errno` and `ioctls`. Paths
share the existing closed file/link namespace and cannot overlap another
entry or serve as its parent. Access mode is the exact admitted `O_ACCMODE`
(0, 1 or 2). Opens admit optional `O_CLOEXEC` and the architecture's
`O_LARGEFILE`, without creation, truncation, nonblocking or direct-I/O behavior.
`open_errno:0` explicitly admits a descriptor; 1–4095 specifies a guest error.
The shared descriptor ceiling and lowest-free allocation still apply. Regular
files remain immutable. Closing retires the descriptor; device mappings can
retain its open description. No host driver's release callback executes.

`dup`, `dup2` and `dup3` share the original open description: file cursor,
device transaction position, context ownership and captured output destination.
`dup` selects the lowest free descriptor; replacement is atomic in this
single-core model. `dup2(fd,fd)` validates then retains the descriptor;
`dup3(fd,fd,flags)` returns `EINVAL`. `dup3` admits zero or `O_CLOEXEC`; execution
and `fcntl` inspection of close-on-exec flags remain unsupported. ARM64 has no
raw `dup2` syscall, but its Bionic import shares the same operation.

Every open starts its own ordered ioctl sequence. Other opens, even to the
same path, have independent positions; this contract does not infer shared
hardware state. Threads using the same descriptor share its position. A
successful transaction consumes exactly one entry. Duplicated descriptors share
that position. Exhaustion, a request
mismatch or a failed input check stops before outputs and consumption. Closing
may abandon unconsumed entries; reopening starts a fresh sequence.

Each ioctl requires `request` (unsigned 32-bit), `result` (signed 64-bit,
at least -4095), and `memory` (possibly empty). Optional `argument` matches
the complete unsigned 64-bit third argument. Wide integers may use decimal
strings. Raw x64/ARM64 ioctl, Android `ioctl`, variadic `syscall` and named
dynamic calls share descriptor lookup and the low-32-bit fd/request values.
Invalid descriptors return `EBADF` before examining a transaction. Bionic
alone converts kernel errors to `-1` and thread errno. Dynamic-provider
lifetime checks remain in force.

A memory entry requires `offset`, `expected_hex` and `output_hex`. Empty
expected/output bytes omit that direction; both cannot be empty. The address
is `argument + offset`. Optional `pointer_offset` first reads one little-endian
64-bit pointer at `argument + pointer_offset`, then adds `offset`. No structure
layout or transfer size is derived from the request's `_IOC` bits; ioctl
arguments can also be scalars, as the
[kernel interface documentation](https://docs.kernel.org/driver-api/ioctl.html)
describes.

All pointers and expected bytes refer to memory at transaction entry. The
model validates every input and output span before writing, then applies
outputs in listed order. Overlapping outputs therefore have explicit order,
while an earlier output cannot change the pointer or expected bytes used by
a later entry. Missing permissions, overflowing addresses and mismatched
bytes stop; they do not invent a driver's partial copy or error code.
An explicit negative result may still have declared outputs, if that is the
provided observation. Backend failures remain terminal errors.

The combined namespace is limited to 256 entries, with 16 MiB of path and
file/link/transaction/update payload bytes. At most 1,024 ioctl entries and
scheduled updates combined are admitted
across the catalogue, with 16 memory regions per entry. The process JSON
request limit and execution budgets also apply. Both C++ and JSON inputs use
the same validation. Unknown fields and malformed hex or numeric values fail
before execution.

## Read-only shared mapping snapshots

An optional `read_only_mappings` array supplies complete RAM pages, independently
of ioctl transactions. Each entry has `offset` (unsigned byte offset), one of
`bytes_hex` or `bytes_base64`, and optional `updates` described below.
Base64 uses canonical padded RFC 4648
encoding; whitespace, URL-safe variants and nonzero padding bits are rejected.
The decoded bytes have identical validation and mapping semantics. This compact
encoding permits a 32 KiB snapshot plus options within the unchanged 64 KiB
JSON request limit; larger complete requests still fail at that boundary.
Offsets and nonempty payload lengths must be multiples of
4,096; their sum must fit a signed 64-bit offset. Entries cannot overlap.
At most 16 entries per device are admitted, and their bytes count against the
same 16 MiB catalogue budget. Omitting the array preserves the unsupported
mapping boundary. No ioctl result creates a mapping input implicitly.

The admitted operation is non-fixed `MAP_SHARED` with `PROT_READ` or `PROT_NONE`.
The page-rounded request must fit wholly within one supplied interval. Missing
pages, private/fixed mappings, unknown flags and other devices remain explicit
unsupported boundaries. A matching read-only interval rejects writable or
executable protection, and a write-only descriptor, with `EACCES`. Empty lengths
and unaligned offsets return `EINVAL`; invalid descriptors return `EBADF`.
Bionic alone converts raw errors to `MAP_FAILED` and errno. Address hints and
allocation limits use the same Linux placement owner as anonymous mappings.

All live mappings of a catalogue entry and offset share the same physical page,
including mappings made through independent opens. Closing the descriptor leaves
its mappings valid, consistent with the [Linux mmap contract](https://man7.org/linux/man-pages/man2/mmap.2.html).
`mprotect` can switch between read and no access but cannot add write or execute
permission. A permission change across adjacent mappings retains any successful
prefix before reaching a denied interval. Partial `munmap` retires exactly the
affected aliases and protection policy. Physical pages are released when the last
alias or pinned observer releases them; a later fresh mapping reads the current
device snapshot, including committed model/producer writes. Virtual aliases and physical pages retain their separate
memory-budget charges.

Without model writes or supplied updates these remain static RAM snapshots.
They do not implement register side effects, cache coherency or GPU execution.
Device advice, read/write, seek, metadata and polling still require their own
semantics. Reports must retain the inputs, actual native/service events and first
remaining boundary; a supplied snapshot does not establish original-device
equivalence or execute callbacks.

## Finite external producer updates

A mapping's optional `updates` array supplies deterministic external byte writes:

```json
"updates": [
  {"after_nanoseconds":1000000,"offset":40,"bytes_hex":"07000000"},
  {"after_nanoseconds":2000000,"offset":48,"bytes_hex":"07000000"}
]
```

Time is elapsed virtual time from workload start; `offset` is relative to the
mapping's initial bytes. Both are unsigned 64-bit integers or decimal strings.
Each patch is nonempty, at most 4,096 bytes, wholly within its mapping. Updates
must be ordered by nondecreasing time; equal-time writes keep array order.
Cross-page writes are permitted within that size bound. Unknown fields fail.

`LinuxClock` advances through admitted sleeps and, optionally, the explicit
[`nanoseconds_per_instruction` policy](process-emulation.md#explicit-guest-clocks).
Before guest execution resumes, due writes update the authoritative device
snapshot and live physical aliases, including `PROT_NONE` mappings. They never
grant guest write permission. Partial unmapping and address reuse cannot redirect
a later update into anonymous RAM; no live mapping is required for the snapshot
to advance. Instructions within one execution interval observe the preceding
boundary's bytes, so `instruction_quantum` remains part of the experiment.
Host time, sample PCs and inferred hardware events do not trigger writes.

## Explicit KGSL context subset

A device may opt into `"kgsl_contexts":{"memstore_offset":4096,"context_limit":3}`.
The offset must select one declared mapping whose 40-byte slots fit IDs 0 through
the inclusive limit (1–1,024). This is an LP64 Adreno protocol subset, not a GPU
driver. Every context starts idle; command submission, externally submitted
queued timestamps, fault recovery, secure contexts, hardware scheduling and
original SDKs are absent.
An external producer trace is a declared workload assumption, not command execution.

The implemented requests are `0xc0080913` (create), `0x40040914` (destroy), and
`0xc00c0916` (read context timestamp). Create admits flags `0x12`
(`PREAMBLE|NO_GMEM_ALLOC`) or the returned default `0x8052`; missing required
flags return `EINVAL`, other flags stop. It allocates the lowest free device-wide
ID, writes back default priority/per-context timestamp flags, and zeros only
the consumed and retired words. Finite ID exhaustion returns `ENOSPC`.

Contexts belong to the creating open description, shared by duplicated FDs and
retained by that description's mappings. Another independent open cannot query
or destroy them (`EINVAL`), even though its mapping sees the same device bytes.
Timestamp types 1 and 2 read the consumed/retired words at slot offsets 0 and 8;
type 3 requires the queue proof below, and invalid types copy zero with EINVAL.
Read-only or partial result buffers stop before allocation or output effects. Static ioctl entries cannot shadow these three modeled requests.

Destroy or final-reference retirement admits only equal consumed and retired
timestamps under the declared quiescent producer contract. An unequal pair
stops before closing/replacing the last FD or unmapping the last retained VMA;
the model cannot infer a hardware wait or the eventual completion value.
Closing an FD while a VMA retains the description preserves its contexts.
Once all references are gone, IDs become available to other opens.

The ABI facts and narrow lifecycle rules were independently implemented from
the pinned AOSP msm revision `12d6459848fcd3bf073765e61299d8d87ec14fdf`:
[UAPI](https://android.googlesource.com/kernel/msm/+/12d6459848fcd3bf073765e61299d8d87ec14fdf/include/uapi/linux/msm_kgsl.h),
[ioctl and ownership](https://android.googlesource.com/kernel/msm/+/12d6459848fcd3bf073765e61299d8d87ec14fdf/drivers/gpu/msm/kgsl.c),
[Adreno context initialization](https://android.googlesource.com/kernel/msm/+/12d6459848fcd3bf073765e61299d8d87ec14fdf/drivers/gpu/msm/adreno_drawctxt.c).
Selecting this model does not identify a captured device's kernel version or
establish equivalence to all vendor variants.

### Queued timestamp admission

A newly created context has a known empty queue only if no declared producer
update can touch its 40-byte memstore slot. Type 3 then reports zero. A producer
for that context makes queued state unknown: consumed/retired page words do not
supply a queued timestamp. Unknown timestamp types return EINVAL and copy out a
zero timestamp. This follows the read/copy-out contracts in the pinned
[KGSL ioctl owner](https://android.googlesource.com/kernel/msm/+/973f4134d9deb396415846f902848f0a32cb4cfa/drivers/gpu/msm/kgsl.c)
and [Adreno timestamp owner](https://android.googlesource.com/kernel/msm/+/a65478df351382d255d293d745aee9bae3a3a741/drivers/gpu/msm/adreno.c).
Blocking waits, command execution and busy-context retirement remain unmodeled.

## Binder control and looper protocol

An explicit device entry can set `"binder":true`, with `access_mode`,
`open_errno` and an empty `ioctls` array as usual. For example:

```json
{"path":"/dev/binder","access_mode":2,"open_errno":0,
 "ioctls":[],"binder":true}
```

The LP64 protocol-8 subset implements VERSION, SET_MAX_THREADS, THREAD_EXIT and
WRITE_READ with REGISTER/ENTER/EXIT_LOOPER commands. Every independent open owns
its control state and per-native-thread loopers; dup aliases share that owner.
O_NONBLOCK is admitted for this device model. Fresh Binder threads can receive
BR_NOOP or a thread-pool spawn request; an empty nonblocking read thereafter
returns EAGAIN while preserving the kernel's observable BR_NOOP buffer write.
Complete write/read records, consumed counts, guards and overlapping output
copy order are retained. A zero-length stream does not dereference its pointer.

There are no inferred peers, service-manager entries, Parcel values, remote
objects, transaction buffers, death notifications or system-service replies.
Such commands and blocking reads that need work stop explicitly. Captured ioctl
replies, read-only mappings and KGSL cannot be combined with this model. Command
streams are bounded to 4096 bytes and 4096 thread identities; partial command or
record faults, short read buffers and resumed read buffers are not admitted.
The state machine follows the pinned
[Binder driver contract](https://android.googlesource.com/kernel/common/+/cf21a9ac5ee4f57c6f45fe2b09d197bdc038f39d/drivers/android/binder.c),
without executing or copying its implementation or contacting a host driver.
