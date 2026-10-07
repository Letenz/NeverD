# Released Android GKI kernel contracts

NeverD prioritizes the released Android GKI 5.10–6.18 branches before broader
Linux kernel variants. Select a branch explicitly in a process request:

```json
{"linux_kernel":{"gki":"android17-6.18"},"linux_files":{"files":[],"descriptor_limit":16}}
```

The Android native profile's API 28 contract describes Bionic imports and does
not select a Linux kernel version. GKI selection currently controls only the
implemented `pidfd_open` contract; it does not certify an entire kernel, load a
kernel image, or infer device, namespace, credentials or process inventory.
Existing unsupported services still stop explicitly. See the official
[GKI release policy](https://source.android.com/docs/core/architecture/kernel/gki-releases).

## Source pins

The catalogue in `LinuxGKIKernels.def` follows these formal `r1` release tags,
checked on 2026-10-07. Each linked immutable commit supplies `kernel/pid.c`,
`include/uapi/linux/pidfd.h` and `arch/arm64/configs/gki_defconfig`. Flag values
come from the UAPI and the syscall's validation, rather than an Android API
level or the host kernel.

| Request branch | Formal release tag | Pinned source commit | Admitted flags |
| --- | --- | --- | --- |
| `android12-5.10` | `android12-5.10-2026-07_r1` | [b14525331e0d](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/kernel/pid.c) | `PIDFD_NONBLOCK` (`0x800`) |
| `android13-5.10` | `android13-5.10-2026-07_r1` | [b9c8cb19d426](https://android.googlesource.com/kernel/common/+/b9c8cb19d426ec591e0a34dcc2d8638147e4ebc1/kernel/pid.c) | `PIDFD_NONBLOCK` |
| `android13-5.15` | `android13-5.15-2026-09_r1` | [0b6028f1f30d](https://android.googlesource.com/kernel/common/+/0b6028f1f30da3c2143bb40eee912c4974108e5c/kernel/pid.c) | `PIDFD_NONBLOCK` |
| `android14-5.15` | `android14-5.15-2026-07_r1` | [9938d39e2fe9](https://android.googlesource.com/kernel/common/+/9938d39e2fe99593abe347df3b82ea2f85c83d1d/kernel/pid.c) | `PIDFD_NONBLOCK` |
| `android14-6.1` | `android14-6.1-2026-09_r1` | [79480508eb1e](https://android.googlesource.com/kernel/common/+/79480508eb1eed09620f1cd5484dbfa4a677d0a9/kernel/pid.c) | `PIDFD_NONBLOCK` |
| `android15-6.6` | `android15-6.6-2026-07_r1` | [5556e039c32f](https://android.googlesource.com/kernel/common/+/5556e039c32fa02b239611dc8e5ebb958a7f12e1/kernel/pid.c) | `PIDFD_NONBLOCK` |
| `android16-6.12` | `android16-6.12-2026-09_r1` | [894a317b5382](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/kernel/pid.c) | `PIDFD_NONBLOCK` and `PIDFD_THREAD` (`0x80`) |
| `android17-6.18` | `android17-6.18-2026-09_r1` | [bab5f6aca819](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/pid.c) | `PIDFD_NONBLOCK` and `PIDFD_THREAD` |

These pins define the current contract. A later release or backport needs source
verification and regression coverage before changing it. A selected released
GKI branch and an explicit absent `pidfd_open` observation contradict each
other and are rejected before image loading.

## Implemented process-descriptor subset

Raw x64/AArch64 traps and Android Bionic's `syscall` share `LinuxServices` and
one workload-owned descriptor table. The model consumes the low 32 PID/flag
bits and rejects unknown flag bits or nonpositive signed PIDs with `EINVAL`
before reserving a descriptor. The current process is a known live group
leader; it can open a pidfd with the selected flags. Any other positive PID
requires an independent task observation and currently stops unsupported.
An unknown target is not silently treated as an absent process.

A request must declare `linux_files` so the same descriptor limit and ownership
apply to regular files and process descriptors. Allocation uses the lowest
free number; exhaustion returns `EMFILE`, `close` releases it, and a second
close returns `EBADF`. Closing a standard stream permits that number to be
reused. No host pidfd, filesystem or process lookup runs.

On a valid pidfd, scalar `read` and `write` return `EINVAL` before payload
access, and `lseek` returns `ESPIPE` after validating the seek origin. `writev`
imports and validates its vector first, so invalid metadata or user extents
can return `EFAULT` before the missing write operation's `EINVAL`. It does not
read payload mappings or capture output for a pidfd. The shared vector importer
keeps existing regular-output ordering and transfer limits. This follows the
pinned pidfd/pidfs operations and [VFS read/write ordering](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/fs/read_write.c).

Bionic translates raw negative errors to `-1` and thread-local `errno`; a
successful call preserves `errno`. `fstat` needs file metadata not supplied by
this subset. Polling, exit notifications, signal delivery through pidfds,
`pidfd_getfd`, `fcntl`, pidfs ioctls and nonleader/foreign task lookup remain
unsupported. Scheduling and process lifetime are not inferred from a pidfd.

## Validation and next coverage

`LinuxPIDFDTests.cpp` executes independent x64/AArch64 ELF fixtures at O0/O2
across the available transports, for all eight branches. It checks versioned
flags, descriptor reuse/limits, regular-file coexistence, scalar/vector error
ordering, invalid input and retained unsupported boundaries.
`AndroidSyscallTests.cpp` repeats ownership and raw/Bionic error encoding over
O0/O2 Android fixtures with ordinary, Android-packed and RELR relocations.

Source pins and these model executions are evidence for the specified syscall
subset. Native tests booting every pinned GKI image are not yet available.
Extend the released GKI matrix service by service, retaining version and
configuration differences and explicit workload observations, before treating
normal Linux distributions as an additional compatibility target.
