**Languages**: [English](darwin-emulation.md) | [简体中文](zh-CN/darwin-emulation.md) | [繁體中文](zh-TW/darwin-emulation.md) | [日本語](ja/darwin-emulation.md) | [한국어](ko/darwin-emulation.md) | [Français](fr/darwin-emulation.md) | [Deutsch](de/darwin-emulation.md) | [Español](es/darwin-emulation.md) | [Italiano](it/darwin-emulation.md) | [Русский](ru/darwin-emulation.md) | [العربية](ar/darwin-emulation.md)

[← Documentation index](README.md)

# macOS and iOS guest process environments

NeverD's Darwin environments run bounded freestanding Mach-O processes. They
are guest OS models under `lib/emulation/os/darwin/`, separate from the host
CPU transport. Enable `NEVERD_ENABLE_CPU_EMULATION`; Windows driver emulation
is not required.

| Profile | Mach-O platform | Guest architectures | OS page size |
| --- | --- | --- | --- |
| `macos-macho64-v1` | macOS | x86-64, baseline ARM64 | 4 KiB x64; 16 KiB ARM64 |
| `ios-macho64-v1` | iOS device | baseline ARM64 | 16 KiB |
| `ios-simulator-macho64-v1` | iOS Simulator | x86-64, baseline ARM64 | 4 KiB x64; 16 KiB ARM64 |

Platform selection is explicit. An iOS device binary is not a simulator image,
and macOS host execution does not change the guest's profile. Matching macOS
host/guest ISAs can use [HVF](macos-hvf.md); cross-ISA execution uses Unicorn
under `auto`. The CPU's mapping granule remains 4 KiB even when the guest OS
requires 16 KiB alignment and allocation units.

```sh
neverd emulate guest.macho --profile=ios-macho64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"]}'
```

The existing [process C, Python and CLI APIs](process-emulation.md#cli-and-sdk)
share this implementation and the same limits and report schema. Returning
Darwin services add an `error` Boolean to their records: a positive errno in
`result` with `error=true` represents BSD carry, not a Linux negative result.
Nonreturning or unsupported requests have no result or error field.

## Image and startup contract

`MachOExecutionImage` uses LLVM's Mach-O parser and the shared loader entry
reader. It preserves original file bytes without analysis relocation patches.
Only thin little-endian `MH_EXECUTE` images with one unambiguous platform and
entry are admitted. Universal images require an explicitly extracted slice;
the execution loader does not choose one from the host.

The complete input file, including unmapped metadata and trailing bytes, must
fit `memory_limit` before parsing or copying. The loader reads a bounded private
snapshot from a regular file and rejects embedded-NUL paths, short reads and
size changes. It does not parse through a live file mapping. This input limit
and the mapped guest-memory limit are separate ceilings with the same value;
host filesystem I/O has no hard wall-clock guarantee.

Segments retain their permissions, zero-fill and maximum protection.
`__PAGEZERO` reserves addresses without allocating its often multi-gigabyte
extent. File and virtual ranges, OS-page alignment, rounded overlap, header
ownership, executable entry and memory budget are checked before the private
address space becomes executable. The Mach header's segment must be readable
and executable. A final file page retains its original bytes through the page
boundary (or EOF); subsequent complete VM pages are zeroed, following XNU's
[Mach-O loader](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/mach_loader.c).
Guard pages and the private main-return gate remain reserved.

`LC_MAIN` receives `argc`, `argv`, `envp` and the apple vector as four integer
arguments under the platform's scalar entry convention. A return produces the
low eight-bit process exit status. The bounded startup model accepts the
standard `/usr/lib/dyld` command only for import-free `LC_MAIN`; it implements
that entry handoff without loading or running host dyld. Nonzero `stacksize`
requests are rejected, since the caller's explicit `stack_size` owns the budget.

`LC_UNIXTHREAD` accepts exactly one complete native 64-bit general-register
record with only its PC populated. The initial stack contains argc, terminated
argv, terminated envp, and a terminated apple vector containing
`executable_path=<input filename>`. Custom SP, flags, other register state,
extra flavors and conflicting entries are rejected. No host environment or
auxiliary Linux vector is inherited. Startup follows the distinction described
in Apple's [dyld architecture](https://github.com/apple-oss-distributions/dyld/blob/main/doc/dyld4.md).

External dylibs, imports, rebases/chained fixups, constructors/destructors,
thread-local sections, arm64e/PAC and other nonbaseline CPU subtypes,
encrypted payloads and unmodeled load commands fail before execution. PIE
images without fixups use their preferred addresses; this is deterministic
placement, not ASLR. Code-signing blobs are metadata, not an implementation
of AMFI or entitlement policy.

## Darwin services

ARM64 uses X16, X0–X5 and `svc #0x80`; x64 uses the BSD class `0x02000000`
with RAX and RDI/RSI/RDX/R10/R8/R9. Successful calls clear carry and supply the
documented scalar result; errors set carry and return positive errno. ARM64
clears X1; x64 clears RDX on success and preserves it on error. X64 SYSCALL
return clobbers are explicit. These rules come from the XNU
[ARM64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/arm/systemcalls.c)
and [x64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/i386/systemcalls.c)
entry paths; no Apple implementation code is incorporated.

The core service inventory is `exit`, `write`, `getpid`, `getppid`, `getuid`,
`geteuid`, `getgid`, `getegid`, `mmap`, `mprotect` and `munmap`. PID, UID and
GID are deterministically 1000, and parent PID is 1. Output descriptors 1 and
2 are initially captured byte sinks, including NUL and non-UTF8 bytes. Their
duplicates retain the original sink; writes through closed or read-only
descriptors return EBADF. A partial readable prefix is captured, but a subsequent copy
fault retains EFAULT, consistent with XNU's
[write error propagation](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/sys_generic.c).
The raw write length is limited to `INT_MAX`; larger requests return EINVAL
before descriptor lookup, guest-pointer access or output-budget admission.

Memory services support private anonymous data mappings with descriptor -1
and offset zero (`flags=0x1002`). Lengths and nonfixed hints round up to the OS
page size; an occupied hint searches upward before falling back. The raw
legacy zero-length mmap succeeds with address zero without allocating;
`MAP_UNIX03` (`0x40000`) additionally permits the standard flag spelling
and rejects zero length with EINVAL. Unmap/protect addresses must be aligned.
NONE, READ and WRITE protections are supported, and WRITE
implies READ. Physical owners are per OS page, so partial unmap releases its
budget and subsequent mappings are zeroed. Maximum protections are distinct
from current rights. A failed protect across a hole or maximum-rights boundary
leaves the complete range unchanged. The rules are grounded in XNU's
[BSD VM services](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_mman.c).

Shared/fixed/JIT mappings, executable mappings, Mach traps,
indirect system calls, threads, signals, host filesystem/network access, dyld
linking, Objective-C/Swift runtime and Foundation/UIKit are outside this
profile. They stop explicitly. This is neither a full Apple OS compatibility
layer nor the iOS Simulator application.

## Explicit file inputs and descriptors

`darwin_files` adds a closed catalogue of immutable regular files to all three
profiles. `files` is required; each entry has a canonical absolute guest `path`
and hexadecimal `bytes_hex`. Optional `stdin_hex` supplies a finite input stream:
omitted input is unknown and stops on a nonzero read; an empty string is EOF.
Neither guest paths nor standard input consult the host. Missing catalogue
configuration stops `open`; an explicit empty catalogue still contains root, while absent paths return ENOENT.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839"}],"stdin_hex":"00ff78","descriptor_limit":32}}
```

The additional services are `open`, `read`, `pread`, `lseek`, `close`, `dup`,
`dup2` and `fcntl`. The `read`, `write`, `open`, `close`, `fcntl` and `pread`
nocancel entries use the same owners. `open` supports O_RDONLY, O_CLOEXEC and O_DIRECTORY.
`fcntl` supports F_DUPFD, F_DUPFD_CLOEXEC, F_GETFD, F_SETFD, F_GETFL and F_GETPATH.
Separate opens have independent cursors; duplicated descriptors share a cursor
but have independent close-on-exec flags. `pread` never changes the cursor.
Closing/replacing descriptors 0, 1 or 2 affects subsequent I/O, and a duplicated
output descriptor keeps its capture sink and the shared output budget.

Configuration allows at most 256 specified file/directory paths (including
metadata/contents-only ancestor paths), 16 MiB of combined paths/NUL/file/input/CWD
and encoded directory-record bytes, paths shorter than 1024 bytes and components of at most 255 bytes.
`descriptor_limit` is an exclusive ceiling from 3 to 4096, default 256.
JSON retains the existing 64 KiB request limit. Invalid configuration is rejected
before image loading, including a file that is another file's ancestor.

Raw read lengths above INT_MAX return EINVAL before FD lookup. EOF does not
touch the destination; invalid writable addresses return EFAULT. A partially
writable destination stops before copying or advancing the cursor, because
partial filesystem copyout effects are outside this model. Seek supports
SET/CUR/END, preserving the cursor on negative-position or overflow errors.
Writable files, legacy stat metadata, sparse-file seeks
and other fcntl operations remain unsupported.
Path prefixes describe implicit directories; a regular file used as an ancestor
returns ENOTDIR. The ABI is grounded in XNU's
[read path](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/sys_generic.c),
[open/seek path](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/vfs/vfs_syscalls.c)
and [descriptor operations](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_descrip.c).

## Directories and relative paths

Optional `directories` entries contain a canonical absolute `path` and optional
complete `metadata`. Root and ancestors are implicit; empty directories can be
explicit. Metadata may describe root or an implicit ancestor, but never creates
a missing path. Directory mode is `0x4000` plus permissions; its `size` is an
explicit observation in [0, INT64_MAX]. Missing size or timestamps are not inferred.
`working_directory` must name an existing canonical directory. Omitting it leaves
CWD unknown; it never inherits the host directory.

`openat` (463), `openat_nocancel` (464), `chdir` (12), `fchdir` (13) and
`fstatat64` (470) share the `DarwinFiles` component walker with open and stat64.
Relative paths use the directory FD or `AT_FDCWD=-2`. Absolute paths ignore the
FD. Repeated separators, `.`, `..` and trailing slashes retain ancestor checks:
`/file/..` returns ENOTDIR and `/missing/..` returns ENOENT. Failed CWD changes
preserve the previous directory; closing, reusing or replacing its original FD
does not change CWD. `F_GETPATH=50` copies the canonical path and NUL, including
through duplicated descriptors, with no writes after the terminator.

Directory read/pread returns EISDIR, even for zero length; a negative positioned
offset takes EINVAL precedence. SET/CUR seek uses a shared cursor; END requires
explicit metadata. Directory mmap returns EINVAL. `fstatat64` accepts zero,
`AT_SYMLINK_NOFOLLOW=0x20`, `AT_SYMLINK_NOFOLLOW_ANY=0x800`, and
`AT_FDONLY=0x400` (ignore the pathname entirely). Invalid flag bits return EINVAL;
`AT_REALDEV=0x200` stops because real-device metadata is unmodeled. Stream path
and directory identities remain unknown. Permission bits are observations,
not an access-control model; directory mutations remain unsupported.

The original `directories` workload compares these services with the native
macOS kernel and all five guest combinations; native stat records cover both
files and directories. Intel HVF Actions remains suspended. ABI references:
[XNU VFS](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c),
[XNU flags](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/fcntl.h).

Directory validation (2026-10-05, Release): 467 registered Darwin cases, 227 passed, 240 skipped, zero failed; all 60/60 required ARM64 HVF cases executed. The 10 native macOS workloads, 37 public C/CLI/report tests without skips, five Python guest combinations and 66 evidence-runner tests passed. Counts overlap. Evidence: `build-hvf-arm64/darwin-directory-verified-evidence/`. Other native transports and physical iOS remain unvalidated for this increment.

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"3031"}],"directories":[{"path":"/work/empty"}],"working_directory":"/work"}}
```

## Explicit directory snapshots

`getdirentries64` (344) enumerates an optional immutable `contents` snapshot on
an existing `directories` entry. C++ uses `DarwinFileOptions::DirectoryContents`.
Supply the complete ordered `entries`, including `.` and `..` and every immediate
catalogue child. Missing snapshots remain unknown, even for an empty directory.
Snapshots create neither paths nor stat metadata and never consult host files.

Each entry requires `name`, nonzero `inode`, `type` (0 unknown, 4 directory,
8 regular file), `next_offset` and `seek_offset`. Type must agree with the path;
inodes must agree for the same resolved path across snapshots and metadata.
`next_offset` is a nonzero cookie up to INT64_MAX, unique within that directory,
with no ascending-order requirement. Zero rewinds. `seek_offset` is the separate
unsigned 64-bit d_seekoff observation; duplicate zero values are valid. Integer
fields use the same lossless decimal-string rules as stat metadata.

`contents.minimum_buffer_size` is a required positive payload minimum up to
128 MiB, including at EOF. An entry's optional `minimum_buffer_size` (default 0)
adds a constraint when a call starts at that entry. The example records the
observed APFS minimum of 64 bytes for the initial dot pair, then 1 byte at EOF;
other positions must fit at least one complete record. Records use the native
LP64 layout and eight-byte alignment, with size `roundUp(25 + nameBytes, 8)`.
At most 4096 records are admitted across all snapshots. Encoded records join
the 16 MiB input budget; metadata/contents-only ancestor paths count once toward
the 256-path limit. The 64 KiB JSON request limit still applies.

Independent opens have independent cursors; dup shares them. Enumeration resumes
at zero or a supplied cookie; unknown positions stop explicitly. Each call emits
a maximal whole-record prefix. Counts >=1024 reserve the last four requested
bytes for EOF flags (1 at EOF, 0 otherwise); only the record payload is capped at
128 MiB. The suffix address retains original unsigned count arithmetic, including
wrap. Data is copied first, the cursor advances, the pre-read position is copied,
then flags are written. Later EFAULT preserves earlier effects; EOF skips the
empty data copy. A partially writable individual copy stops unsupported before
that copy, retaining any preceding copies and cursor effects.

The original `directory-entries` workload compares record fields, dup/rewind,
small reads, EOF and copy ordering with the native macOS kernel. A separate test
compares captured native record bytes, including long names, against the SDK
layout. Snapshot cookies remain fixed on rewind; this does not reproduce APFS's
dynamic cookie generations. Legacy `getdirentries` (196), writable directories,
other native transports and physical iOS remain outside this acceptance.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"3031"}],"directories":[{"path":"/empty"},{"path":"/","contents":{
  "minimum_buffer_size":1,"entries":[
    {"name":".","inode":41,"type":4,"next_offset":11,"seek_offset":0,"minimum_buffer_size":64},
    {"name":"..","inode":41,"type":4,"next_offset":22,"seek_offset":0},
    {"name":"empty","inode":42,"type":4,"next_offset":7,"seek_offset":0},
    {"name":"data","inode":73,"type":8,"next_offset":99,"seek_offset":0}]}}]}}
```

[XNU getdirentries64](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [dirent ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent.h), [extended flags](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent_private.h).

Enumeration validation (2026-10-05, Release): 498 Darwin registrations, 246 passed, 252 unavailable-backend skips, zero failures; all 63/63 required ARM64 HVF cases executed. All 11 original native macOS workloads, 38 public C/CLI/report checks without skips, five Python guest combinations with eight file workloads, and 66 runner tests passed. Counts overlap. Evidence: `build-hvf-arm64/darwin-dirents-verified-evidence/`. Intel HVF Actions remain suspended; other native transports and physical iOS are unvalidated.

## Private file mappings

`mmap` accepts `MAP_PRIVATE` regular catalogue-file mappings (`flags=0x2`
or `0x40002` with `MAP_UNIX03`). Offsets must be OS-page aligned. The complete
mapped pages contain the original file bytes, including bytes beyond a short
requested length; the remaining part of the final EOF page is zero. Private
writes change only that mapping. Independent mappings, original file bytes,
fixed metadata and shared descriptor cursors remain independent. Closing or
reusing the descriptor does not retire an existing mapping. Read-only and
PROT_NONE mappings are initialized before their guest protections apply;
`mprotect` can subsequently grant WRITE, which implies READ.

File-end overflow and UNIX03 zero-length/alignment errors return EINVAL before
FD lookup; an invalid FD returns EBADF before memory-budget admission. Legacy
zero length still validates the FD and returns address zero without allocating.
Legacy unaligned offsets, stream descriptors, empty-file pages and any complete
page beyond EOF stop as unsupported before allocation. Native macOS accepts
such EOF mappings but faults with SIGBUS on access; this model does not invent
zero-filled readable pages or claim to deliver that signal. Shared, fixed,
executable and JIT mappings remain outside the contract.

`DarwinFiles` alone resolves descriptor kind and bytes. `DarwinMemory` owns
placement, page allocation, maximum protection and rollback; mapping bytes are
charged to the existing memory limit. All file data comes from `darwin_files`,
with no host-file passthrough. The independent `file-mapping` workload checks
private writes, close lifetime, cursor preservation, errors and anonymous page
reuse. A separate native comparison checks a nonzero file offset, every byte
of the final page and the real SIGBUS boundary. ABI reference:
[XNU mmap](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c).

## Explicit file metadata

A file entry may additionally contain `metadata`. When present, every field
shown below is required. Decimal strings preserve full-width integers; numeric
JSON is limited to exactly represented integers within ±(2^53−1). Device IDs
are signed 32-bit; mode and link count are unsigned 16-bit; inode is unsigned
64-bit; UID, GID, flags and generation are unsigned 32-bit. `size` must match
`bytes_hex`, blocks fit signed 64-bit, and block size fits nonnegative signed
32-bit. Times use signed 64-bit seconds and nanoseconds in [0, 999999999].
The mode must match the catalogue kind, with optional permission bits.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839","metadata":{
  "device":1,"inode":"18364758544493064720","mode":33188,"link_count":1,
  "uid":1000,"gid":1000,"size":10,"block_size":4096,"blocks":8,
  "flags":0,"generation":0,
  "access_time":{"seconds":-1,"nanoseconds":1},
  "modification_time":{"seconds":2,"nanoseconds":3},
  "change_time":{"seconds":4,"nanoseconds":5},
  "birth_time":{"seconds":6,"nanoseconds":7}
}}]}}
```

`stat64` (338), `fstat64` (339) and `lstat64` (340) return one 144-byte LP64
record on ARM64 and x64. Path queries share `open`'s component resolver, while
FD queries follow descriptor duplication and closure. The regular-file rdev,
padding and reserved fields are zero. Metadata is a fixed caller observation:
reads do not change timestamps, and mode bits do not change catalogue access.
Missing metadata, stream status, symbolic links, legacy stat layouts and
extended-security variants remain unsupported. Missing paths and
bad descriptors precede output-pointer checks; partial output stops before any
bytes are written. Status queries neither allocate FDs nor change cursors.
The ABI is described by XNU's [stat records](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/sys/stat.h).
The native test compares all record bytes with a real filesystem observation
and checks offsets and widths against the macOS SDK. The authored raw-call
fixture independently checks all three services against the native kernel.

## Verification

`NeverDDarwinProcessTests` builds original freestanding C fixtures with Clang
and `ld64.lld`, without an Apple SDK or proprietary binary. It exercises all
five platform/ISA combinations through Unicorn and available native transports.
Independent Mach-O records test direct thread entry and malformed metadata;
memory tests exercise 4 KiB and 16 KiB behavior, including partial initial
stack/data unmap under a full physical budget. `NeverDProcessPublicTests`
checks C API/CLI report parity for every profile and ISA. Python's
`test_process_integration.py` covers the same five Mach-O fixture combinations
when `NEVERD_TEST_LIBNEVERD` and `NEVERD_TEST_DARWIN_FIXTURES` are configured.

```sh
cmake --build build-hvf --target NeverDDarwinProcessTests NeverDProcessPublicTests --parallel 8
ctest --test-dir build-hvf -L '^(NeverDDarwinProcessTests|NeverDProcessPublicTests)$' --output-on-failure
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/native-evidence --require-hvf
```

The native gate requires matching HVF cases to execute, including Darwin
fixtures; missing `ld64.lld` cannot turn the required suite into a skip. Each
transport must be verified on its own host; the results below distinguish
Apple Silicon HVF, Intel HVF, Linux KVM and Windows WHP. The historical Darwin
inventories below passed on their respective matching hosts. Those results do
not validate subsequently added file services on Intel HVF, KVM or WHP. Intel
HVF runtime remains unvalidated and its Actions testing stays suspended. See the
[HVF validation record](macos-hvf.md#implementation-validation-2026-10-02-to-2026-10-03).

The focused workload gate additionally requires **every** Darwin process case
on each platform supported by the host ISA: 60 cases on ARM64, or 40 on x64.
Both `LC_MAIN` and independent raw `LC_UNIXTHREAD` programs are required on
every supported platform. A source-inventory regression ensures each new
Darwin process test joins this required set.
On a native macOS build, `DarwinNativeTests.cpp` also runs the same authored
object against the host kernel. A separate host executable links libSystem
only for dyld's real main handoff; guest images remain import-free. Return,
exit, memory protection/reuse and oversized-write error ordering must match
the fixture's exit status and exact output. The file and nocancel cases reuse
the same authored object with an isolated host file containing the guest's
exact bytes. Descriptor replacement/redirect cases also run on the host.
Memory-only tests cover both OS page sizes, denied and partial copyout,
configuration admission, absent/empty input and FD exhaustion. Public C/CLI
and Python checks cover file, nocancel, binary stdin, output redirection and
stat64 observations.
on every platform/ISA combination. This native reference is required
by the HVF gate; it does not establish native iOS execution.
The independent [native kernel workflow](../.github/workflows/darwin-kernel-reference.yml)
runs these same cases on both Intel and Apple Silicon macOS without building
NeverD or LLVM. `DarwinNativeCases.def` owns the modes and expected byte output
for both runners. Wrong host architectures, Rosetta, timeouts and any result
mismatch fail the reference gate; its JSON records the source, OS and compiler.
Run it locally with `python3 scripts/run_darwin_kernel_reference.py
--architecture arm64 --evidence build-kernel-reference` (use `x86_64` on Intel).
The focused workload gate preserves the full inventory and original test XML, and fails on missing or
skipped native workloads even when loader-only tests pass:

```sh
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/darwin-workload-evidence --require-darwin-backend hvf
```

Use `kvm` on Linux or `whp` on Windows with the same command. The manual
[`Native Darwin workloads` workflow](../.github/workflows/darwin-native.yml)
builds without Unicorn and runs both x64 transports on hosted runners. It
also accepts a single `kvm` or `whp` selection for focused reruns. It
requires actual virtualization and `ld64.lld`; unavailable hardware or fixture
tools fail explicitly. The result validates the bounded guest OS model and
shared CPU contracts, not Intel HVF or native iOS hardware.

### Metadata verification, 2026-10-05

After adding stat64, the Release gate reconciled 409 unique registrations:
193 passed, 216 skipped and zero failed. All 54 required ARM64 HVF outcomes
ran, and Unicorn covered all five guest combinations. Native SDK layout and
full-record comparisons passed, along with all eight original macOS programs,
36 public/report cases without skips, Python on all five combinations and
66 evidence-runner tests. Counts overlap. The native harness now uses a separate
output file per case, fixing stale trailing bytes after shorter outputs.
These additions still have no native Intel HVF/KVM/WHP or physical iOS evidence.

### Private mapping verification, 2026-10-05

The Release Darwin gate reconciled 438 unique registrations: 210 passed,
228 skipped and zero failed. All 57 required ARM64 HVF cases executed;
Unicorn covered all five guest platform/ISA combinations. All nine original
native macOS programs passed. A separate native test compared every byte of
a nonzero-offset EOF page and confirmed SIGBUS in an isolated child for the
next complete page. All 36 public/report checks ran without skips, and Python
executed all five combinations, including `file-mapping`. The 66 evidence-runner
and 38 provenance regression tests passed. Counts overlap. Evidence is under
`build-hvf-arm64/darwin-mmap-verified-evidence/`; this adds no Intel HVF/KVM/WHP
or physical iOS acceptance. Intel HVF Actions remain suspended.

### Remaining environment work

1. Extend the file model with bounded writable state,
   shared mappings and EOF fault delivery. Keep native acceptance for cursor,
   mapping lifetime and error-order interactions as the supported set grows.
2. Add explicit time/system observations and required Mach/thread services,
   then Mach-O dependency loading, rebases/binds, initializers and TLS. Validate
   small real executables at each boundary before admitting general libraries.
3. Add Objective-C/Swift and Foundation/UIKit behavior with executable native
   references. Physical iOS comparison needs an iOS SDK and device environment;
   Intel HVF remains unvalidated and its Actions workflows remain suspended.

### File services verification, 2026-10-05

The Release Darwin gate on Apple Silicon, with HVF and Unicorn enabled,
reconciled 381 registrations: 177 passed, 204 skipped and zero failed. All
51 required ARM64 HVF workloads executed. The software backend exercised
all five platform/ISA combinations. The seven original native macOS cases,
35 process-report/public C/CLI tests, Python's five Darwin combinations and
66 evidence-runner tests passed. Counts overlap and must not be added.
The new file services have no native Intel HVF, KVM or WHP evidence; earlier
results below refer to their recorded inventories. The host lacks an iOS SDK,
and neither guest execution nor macOS references establish iOS device results.

### Local verification, 2026-10-03

Release evidence on Apple M4 Max / macOS 15.6.1, source
`36e11ca8a3d80aecf585d3328018839ce7fdb989`:

| Check | Passed | Failed | Skipped | Required native cases |
| --- | ---: | ---: | ---: | ---: |
| Full HVF gate, Unicorn disabled, 20 owners | 834 | 0 | 5,903 | 13 / 13 |
| Every Darwin workload, Unicorn disabled | 65 | 0 | 221 | 39 / 39 |
| Darwin and public C API/CLI, with Unicorn | 138 | 0 | 156 | — |

The rows overlap and are not additive. Native summaries record clean source,
no missing registrations and no unexecuted required cases. Skips belong to
foreign architectures, unavailable transports or the disabled software backend.
The 65 Darwin checks include loader/VM tests, every ARM64 process workload and
the original host-kernel reference. With the later native-interruption inventory,
the full HVF gate requires 16 checks on ARM64 or 14 on Intel, including the native reference; the focused
Darwin gate separately requires all 39 or 26 process workloads. The later full
ARM64 gate at clean source `561ebf37b9eaaec08043ac5816b2e083ecccaf68` passed 841
checks, failed none, skipped 5,939 and executed all 16 required outcomes. Its
evidence is in `build-hvf-native/hvf-cancellation-full-evidence/`.

Evidence is under `build-hvf-native/hvf-release-evidence/`,
`build-hvf-native/darwin-release-evidence/` and
`build-hvf/verification/darwin-release-public.xml`. Regressions cover whole-file
admission including unmapped trailing bytes, direct thread entry on every
platform, oversized-write error ordering and formatted native-case inventories.
The native inventory/result/CI regression suite passed 106 tests. Capability,
localized-documentation, provenance and formatting checks also passed.

After later `dev` integration, clean source
`f4bf8dde5cbc33d18ce053cb0722a54047e5a2d0` repeated the independent ARM64
Darwin gate: 65 passed, zero failed, 221 skipped and all 39 required native
workloads executed. Evidence is in
`build-hvf-native/hvf-final-dev-darwin-evidence/`. This rerun validates the
Darwin owner; it does not claim a new complete CPU gate for unrelated Windows
process changes merged in the meantime.

The later clean-source method run at
`d5864c055116a687546320e4acf0788ef4a4e735` again passed all 39 ARM64 Darwin
workloads (65 passed, 221 skipped), with all 286 CTest identities reconciled
against original GoogleTest XML. The full twenty-owner run at that source
passed 849 cases, failed none and skipped 5,993 across all 6,842 registrations.
Its sixteen native requirements all passed. These runs include the subsequent
Windows environment changes and explicitly record method-level process
isolation. Evidence is in `build-hvf-native/hvf-method-clean-{full,darwin}-evidence/`.

Earlier integration checks exercised all five platform/ISA combinations through
the Python SDK. The packaged engine matched 18 no-Unicorn CLI reports across
all three ARM64 profiles, and the bundle passed dependency/signature checks for
186 Mach-O images plus Cocoa startup. With both HVF and Unicorn disabled, the
Darwin/HVF/configuration targets built and passed 38 checks, with 231 backend
cases skipped and no Hypervisor.framework linkage. Those are integration and
build-isolation checks, not additional native guest executions. The separate
[desktop GUI workflow](https://github.com/NeverSight/NeverD/actions/runs/37053518872)
passed on macOS, Windows and Ubuntu at `e078b129c`.

### Hosted native verification, 2026-10-03

The [Intel HVF Darwin gate](https://github.com/NeverSight/NeverD/actions/runs/37106013999)
at clean source `8dcc74c59da303176801b99747a60339161b824b` passed every required
x64 workload: **26/26**, across macOS and iOS Simulator. All 286 CTest
registrations were reconciled against original GoogleTest XML: **52 passed,
0 failed, 234 skipped**, with no missing, duplicate or unexecuted required
results. The 234 skips are 65 disabled-Unicorn cases, 39 foreign ARM64
guests and 130 foreign-host backends. All 32 method processes exited
successfully. The native macOS kernel reference also passed; these results do not establish iOS device-kernel or
broader Intel CPU acceptance.

Artifact `11267489438` was downloaded and verified against SHA-256
`cd8fabbd7d031ac4ad7b891b8e5a52f3e3abe3c39306d9c4a1893e40912e78ef`.
Evidence is retained under `build-hvf/verification/hvf-intel-darwin-accepted/`.
Execution uses the documented serial method policy on hosted Intel, including
all parameters and the CTest environment. The runner was macOS x86-64 with
four logical CPUs and Darwin 24.6.0. The same run also passed its ten-case
transport preflight, 100 interruption/recovery repetitions and isolated CR8
regression; these overlapping checks are not added to the Darwin totals.

Both x64 transports executed the latest focused Darwin workload gate with
Unicorn disabled, including the file-budget, direct-thread-entry and oversized
write regressions. All 26 required process cases across macOS and iOS Simulator
passed at `36e11ca8a3d80aecf585d3328018839ce7fdb989`:

| Host / transport | Passed | Failed | Skipped | Required native cases |
| --- | ---: | ---: | ---: | ---: |
| [Windows / WHP](https://github.com/NeverSight/NeverD/actions/runs/37062839703/job/111023370601) | 51 | 0 | 235 | 26 / 26 |
| [Ubuntu 24.04 / KVM](https://github.com/NeverSight/NeverD/actions/runs/37062839703/job/111023370857) | 51 | 0 | 235 | 26 / 26 |

The artifacts `darwin-native-whp-x64` and `darwin-native-kvm-x64` contain the
inventory, JUnit results, CTest log and source/host summary. Both summaries
record clean source trees and zero missing or unexecuted required cases. Skips
include the macOS-only kernel reference, foreign ISAs and other transports.
Downloaded artifact hashes were verified against GitHub's SHA-256 digests.

These runs establish the bounded Darwin model on the indicated transports.
Intel Mac HVF and broader backend CPU behavior require their own gates.

### Independent native macOS reference, 2026-10-03

At `e727d3eab7086063bb392444bd55014ac48d43c3`, the original programs passed
directly on both macOS 15.7.9 kernels, compiled with Apple Clang 17.0.0:

| Host architecture | Native workloads passed |
| --- | ---: |
| [Apple Silicon ARM64](https://github.com/NeverSight/NeverD/actions/runs/37064795867/job/111029848093) | 4 / 4 |
| [Intel x86-64](https://github.com/NeverSight/NeverD/actions/runs/37064795867/job/111029847805) | 4 / 4 |

Both clean-source summaries record exit status 37 for `return`, `exit`,
`memory` and `write-length`, with exact expected output and empty stderr.
The artifact digests were verified. This independently checks the original
workloads' kernel contracts on both ISAs; the emulated executions remain
covered by the separate HVF/KVM/WHP results above.
