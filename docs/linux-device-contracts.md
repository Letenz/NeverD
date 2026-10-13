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
files remain immutable. Closing retires the model's descriptor, without
executing a driver's release callback.

Every open starts its own ordered ioctl sequence. Other opens, even to the
same path, have independent positions; this contract does not infer shared
hardware state. Threads using the same descriptor share its position. A
successful transaction consumes exactly one entry. Exhaustion, a request
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
file/link/transaction payload bytes. At most 1,024 ioctl entries are admitted
across the catalogue, with 16 memory regions per entry. The process JSON
request limit and execution budgets also apply. Both C++ and JSON inputs use
the same validation. Unknown fields and malformed hex or numeric values fail
before execution.

Device `read`, `write`, seek, metadata, polling and memory mapping remain
unsupported without their own semantics. A successful ioctl does not supply
`mmap` backing bytes, create threads, execute callbacks or prove equivalence
with an original device. Reports must retain the input contract alongside
actual native/service events and the first remaining boundary.
