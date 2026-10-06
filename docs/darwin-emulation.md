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
BSD services add an `error` Boolean to their records: a positive errno in
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

BSD calls on ARM64 use X16, X0–X5 and `svc #0x80`; x64 uses the BSD class `0x02000000`
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

Shared/fixed/JIT mappings, executable mappings, other Mach traps,
indirect system calls, threads, signals, host filesystem/network access, dyld
linking, Objective-C/Swift runtime and Foundation/UIKit are outside this
profile. They stop explicitly. This is neither a full Apple OS compatibility
layer nor the iOS Simulator application.

## Explicit file inputs and descriptors

`darwin_files` adds a closed catalogue of initially read-only regular files to all three
profiles. `files` is required; each entry has a canonical absolute guest `path`
and hexadecimal `bytes_hex`. Optional `stdin_hex` supplies a finite input stream:
omitted input is unknown and stops on a nonzero read; an empty string is EOF.
Neither guest paths nor standard input consult the host. Missing catalogue
configuration stops `open`; an explicit empty catalogue still contains root, while absent paths return ENOENT.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839"}],"stdin_hex":"00ff78","descriptor_limit":32}}
```

The file services include `open`, `read`, `pread`, `write`, `pwrite`, `truncate`,
`ftruncate`, `lseek`, `close`, `dup`, `dup2`, `fcntl`, `rename`, `renameat` and `renameatx_np`. The `read`, `write`,
`open`, `close`, `fcntl`, `pread` and `pwrite` nocancel entries use the same owners.
`open` supports O_RDONLY, O_WRONLY, O_RDWR, O_APPEND, O_TRUNC, O_CREAT, O_EXCL, O_CLOEXEC and O_DIRECTORY.
`fcntl` supports F_DUPFD, F_DUPFD_CLOEXEC, F_GETFD, F_SETFD, F_GETFL, bounded F_SETFL and F_GETPATH.
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
Legacy stat metadata and other fcntl operations remain unsupported.
Path prefixes describe implicit directories; a regular file used as an ancestor
returns ENOTDIR. The ABI is grounded in XNU's
[read path](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/sys_generic.c),
[open/seek path](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/vfs/vfs_syscalls.c)
and [descriptor operations](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_descrip.c).

## Mutable existing files

A file entry can opt into process-local mutation with the strict Boolean
`"writable": true`; C++ uses `DarwinFileOptions::WritableFiles`. Omission or false
keeps the initial read-only contract. Unknown write authorization stops explicitly.
No host file is read or changed, and the caller's initial bytes remain unchanged.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233","writable":true}]}}
```

`write` (4/397), `pwrite` (154/415), `truncate` (200), `ftruncate` (201) and
O_TRUNC change a shared file node. Separate opens share bytes but retain independent
cursors; dup shares its open description. Closing the last descriptor does not
reset the file. Growth zero-fills gaps; truncation preserves every cursor.
O_RDONLY|O_TRUNC also truncates. F_SETFL changes only O_APPEND, preserves access,
close-on-exec and the observed FWASWRITTEN bit; unsupported flags stop explicitly.
F_GETFL exposes FWASWRITTEN=0x10000 after nonzero transferred bytes, including
positioned writes and captured output. Successful ftruncate also sets the bit,
even at unchanged size, on the calling open description and its duplicates.
O_TRUNC sets it on the newly opened description, including O_RDONLY; path
truncate leaves existing description flags unchanged. pwrite ignores append
and preserves the cursor. Count above INT_MAX returns EINVAL before FD lookup; pwrite offset -1
returns EINVAL even earlier. Offset INT64_MAX returns EFBIG before zero-count
success; length is clipped at that boundary before append chooses EOF.

Fully readable input commits its bytes and cursor once. A partially readable
input stops before effects because native prefix writes and extension rollback
are filesystem-dependent. Whole EFAULT preserves bytes; nonempty append still
moves its cursor to EOF. Transport failures preserve contents and cursor.
Without an explicit mutation policy, successful nonzero writes, truncation and
nonzero whole-EFAULT attempts invalidate the complete stat observation: native failed append can change mtime/ctime.
Subsequent stat calls stop as unknown before output instead of returning stale
size/timestamps/block allocation. Zero writes leave contents and metadata intact.

The aggregate logical storage cap stays 16 MiB: paths/NUL, input, directory
records, CWD and current file sizes count together; writable path references also
count. Admission precedes allocation. Shrinking replaces the backing vector and
reclaims capacity. Caller-owned initial bytes and one bounded replacement buffer
are additional storage, not claimed to fit within that logical cap. Known writable
inode aliases and immutable/append-only metadata flags are rejected until their
semantics are modeled.

Private mappings retain file leases until all their ranges are unmapped, including
PROT_NONE mappings and mappings whose FDs have closed. Writes and truncation,
including O_TRUNC, stop before effects while a lease exists; failed or legacy
zero-length mappings retain no lease. A fresh mapping sees current file bytes.
O_WRONLY mmap with READ (including normalized WRITE) returns EACCES; PROT_NONE
is allowed and later mprotect may grant read/write, matching XNU's private
maximum-permission normalization.

The original `writable-files` and `writable-files-nocancel` workloads compare
actual bytes, offsets, flags and error order with the native kernel. Unit tests
cover 4 KiB/16 KiB pages, budget reclamation, backend failure, metadata invalidation
and mapping lifetime; C/CLI/Python exercise all five profile/ISA combinations.
Permission enforcement, directory deletion, cross-parent rename, hard links, native filesystem metadata updates, coherent vnode/COW
or shared mappings and EOF SIGBUS remain unfinished. This does not complete the
macOS/iOS environment or establish physical iOS or Intel HVF verification.

References: [XNU write](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c),
[vnode offsets](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c),
[file flags](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/fcntl.h),
[mmap permissions](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c).

## Explicit mutable metadata

A file may add `mutation_policy` next to `writable: true` and complete `metadata`;
C++ uses `DarwinFileOptions::MutationPolicies`. This selects a virtual sparse-unit
filesystem contract. It does not infer APFS allocation or sample the host clock.
Omitting it preserves the unknown post-mutation metadata contract above.

```json
{"mutation_policy":{"allocation_unit":4096,"mutation_time":{"seconds":-7,"nanoseconds":123456789}}}
```

Both fields and both time members are required, with the existing lossless integer
rules. The allocation unit must be a power of two from 512 bytes to 16 MiB,
independent of `block_size` and VM pages. Admission requires ordinary regular-file
permissions (no set-id/sticky), `flags=0`, `link_count=1`, and dense initial
allocation: `blocks=ceil(size/allocation_unit)*(allocation_unit/512)`. Dense is
an explicit assertion even for zero bytes; the model never infers holes from
their values. The policy path reference also counts toward the 16 MiB logical
storage limit. Block accounting is a separate virtual ledger, not an ENOSPC model.

Writes allocate every touched unit, including writes of zero into a hole. Truncate
growth adds zero bytes without allocating units; shrink discards whole units beyond
rounded-up EOF and retains an allocated final partial unit. Regrowth does not restore
discarded allocation. Successful nonzero writes and every successful truncate,
including same-size truncate and empty O_TRUNC, update size/blocks and set mtime/ctime
to the supplied fixed time. All other metadata stays unchanged; reads do not advance
atime. Stat by path, independent opens, dup and reopen share the same node state.
Caller input observations remain unchanged.

Zero writes, budget or mapping refusals, partial-input refusal and backend failure
preserve known metadata. Nonempty whole-EFAULT still makes it unknown; later
successful writes or truncation cannot reconstruct it. Failed stat copyout preserves
the node. The `virtual-file-metadata` guest workload checks the complete 144-byte
record through all five profiles and C/CLI/Python; its allocation results are policy
tests, not native APFS comparisons. Existing native writable workloads verify the
kernel flags, cursors and error ordering separately. Further namespace operations, native
filesystem coherence, Mach services and dynamic runtime loading remain unfinished.

## Sparse file positioning

`lseek` accepts `SEEK_HOLE=3` and `SEEK_DATA=4` on regular files with an explicit
mutation policy whose allocation state is still known. It reads the same unit
ledger as stat; before the first mutation the admitted initial file is dense,
including zero-valued bytes. Within the requested kind of unit it returns the
input offset, otherwise the next matching unit's start. The terminal hole begins
at EOF. Negative offsets return EINVAL; offsets at or beyond EOF, including an
empty file, return ENXIO=6 for either mode. No later data also returns ENXIO.
Errors preserve the cursor; success changes only the open description shared
by dup. Independent opens keep their own cursors, and reopen sees current allocation.
Metadata, flags and bytes are unchanged. High whence carrier bits are ignored.

Missing policies, directories and permanently unknown allocation after whole
EFAULT remain unsupported. No holes are inferred from zero bytes, and rejected
writes or growth cannot invent allocation. The original `sparse-file-seek` program
checks native/guest errors, occupied bytes, EOF and descriptor lifetime without
assuming earlier filesystem-specific extent boundaries. `virtual-file-metadata`
separately checks exact policy geometry; C/CLI/Python cover all five profiles.
[XNU lseek](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

## Removing regular-file names

Per-directory `mutable:true` (C++ `DarwinFileOptions::MutableDirectories`) explicitly authorizes immediate namespace changes, independently of `writable` file contents. The example below permits unlink of a read-only `/work/data`. Missing grants stop as unsupported; permission bits do not invent credentials or EACCES. Admission rejects nonzero known flags on mutable parents or immediate regular children, special parent permission bits, child link_count other than one, and known identity aliases of either the parent or child. Identity checks include stat and directory-snapshot inode observations; distinct explicitly known devices stay distinct. Grants count toward the existing path/entry budgets.

`unlink(10)` and `unlinkat(472)` remove existing regular names; regular-file unlinkat accepts flags zero or `AT_SYMLINK_NOFOLLOW_ANY=0x800`. Unknown flag bits return EINVAL before path/FD checks; AT_REMOVEDIR uses the bounded directory-removal contract below; DATALESS and SYSTEM_DISCARDED remain unsupported. Low 32 flag bits apply. The shared resolver preserves pathname-fault/dirfd order, relative CWD/dirfd use and absolute-path independence. Missing names return ENOENT, file/trailing-slash paths ENOTDIR, ordinary directory targets EPERM and a slash-only guest root EISDIR; root paths ending in `.` or `..` return EBUSY. Original native probes also check terminal `.`/`..` directory paths.

A live namespace owns file objects separately from open descriptions. Unlink preserves existing FD/dup/independent-open bytes, offsets and status flags. Later opens fail; implicit parent directories and CWD remain present. F_GETPATH retains the last linked path, matching the native reference even after name removal. The file's writable grant belongs to its object. Unlinked objects retain their current-byte budget while any description or mapping lease remains; close/dup2 and the next mutation reclaim only unreachable objects, after the final mapped range is unmapped. Initial path/reference costs remain charged. Cross-parent rename, hard links and removal of initial or held directories remain unfinished; creation is described below.

A successful unlink invalidates only its parent's stat and enumeration observations across old/new FDs, dup and path queries. stat/readdir/SEEK_END stop before output or cursor changes; ordinary read/pread still return EISDIR, while SET/CUR, F_GETPATH, fchdir and relative lookup continue. With a still-known file mutation policy, unlink sets nlink=0 and ctime to its fixed time, preserving mtime/atime/bytes/allocation. Later writes or truncation cannot restore nlink=1. Without a policy, or after whole-EFAULT, complete file metadata remains unknown. Rejected unlinks preserve all state. The original `unlinked-file` program compares native kernel name/descriptor behavior; policy timestamps and directory invalidation are explicit model rules.

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"00"}],"directories":[{"path":"/work","mutable":true}]}}
```

[XNU unlink / unlinkat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [F_GETPATH](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_descrip.c).

## Creating regular files

`O_CREAT=0x200` creates an empty regular file only in an explicitly mutable immediate parent. Ordinary/nocancel open and openat share this behavior. The namespace grant makes new objects writable; existing objects retain their separate `WritableFiles` authority. A read-only descriptor can create but cannot write. Without an explicit creation policy, new stat64 and sparse-seek observations remain unknown, including when an old configured name is reused. New objects never inherit that old name's metadata or mutation policy.

`O_EXCL=0x800` with O_CREAT returns EEXIST for an existing file or directory before truncation or writable/mapping checks; alone it has no effect. Existing read-only directory opens with O_CREAT succeed. Invalid access mode precedes descriptor availability, then O_CREAT|O_DIRECTORY returns EINVAL before pathname access. Only a missing final original component can be created: missing ancestors, trailing `/`, `//`, `/.` and `/..` remain ENOENT. Relative CWD/dirfd and absolute-path rules share the existing resolver. New O_CREAT|O_TRUNC does not set FWASWRITTEN; truncating an existing object does.

Only successful insertion invalidates the immediate parent's stat/enumeration observations. New and old unlinked objects at the same name retain independent bytes, descriptors, metadata and mapping leases. The 256-entry cap includes fixed initial non-file entries plus live and retained orphan file objects. Each new object charges its canonical path and NUL alongside current bytes within 16 MiB. Unlink reclaims these dynamic charges only after the last description and mapping lease; closing a still-named object releases neither its entry nor bytes. Initial input/reference charges remain reserved. Exhausted model budgets, including a resolved canonical path of 1024 bytes or more, stop explicitly without inventing ENOSPC or a native pathname error. Rejected creation publishes no name or descriptor.

The original `created-file` workload compares native macOS and all five guest profiles; 4K/16K tests independently cover exact capacity, rollback and mapped name reuse. Permission enforcement, cross-parent rename, links and further directory operations remain separate work.

[XNU open](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).

## Explicit creation metadata and process umask

Optional `darwin_files.umask` (C++ `InitialUmask`) declares the initial process mask, from 0 through octal 07777. It works independently of file creation authority. `umask(60)` returns the previous mask and stores the low 07777 bits, without guest-memory access or a free descriptor. Omission remains unknown; no host/default mask is inferred. The mask initializes once, changes only future creations, and never mutates caller input. The example below supplies decimal 18, or octal 0022.

Optional `darwin_files.creation_policy` (C++ `CreationPolicy`) enables complete metadata for new regular files. Its strict object has exactly `first_inode`, `block_size`, `generation`, `creation_time` and `mutation_policy`; time and mutation objects use the existing strict formats. It requires an explicit umask, at least one mutable parent, and complete metadata for every mutable parent. `block_size` is positive and at most INT32_MAX; generation is uint32. The allocation unit is a power of two from 512 through 16 MiB, independent of block size and VM pages; nanoseconds must be in [0, 1000000000). `first_inode` is a nonzero uint64 greater than every supplied stat/snapshot inode, including other devices. Decimal strings preserve values beyond JSON's exact integer range.

Only a successful new insertion consumes the next global inode. A successful UINT64_MAX exhausts the sequence permanently; close, unlink, name reuse, umask and later namespace lookups cannot reset it. Exclusive, FD, path, entry and byte-budget refusals publish no name, descriptor or inode increment. Existing O_CREAT opens consume none.

New stat64 records use the direct parent's supplied device and GID, the fixed guest effective UID 1000, mode `S_IFREG | (mode & 0777 & ~umask)`, nlink 1, and zero size/blocks/flags. Block size and generation come from policy; all four timestamps equal the fixed creation time. Device/GID remain known after a parent's complete stat/enumeration observation becomes invalid, so later creation can use those fields without restoring that full record. Each new node owns its metadata and allocation state; it never borrows an old same-name object's record. Subsequent write/truncate/unlink use the supplied mutation policy, preserve inode/mode/birthtime and an unlinked nlink=0, and retain permanent whole-EFAULT invalidation. Existing nodes are unaffected by the creation policy.

The original `created-file-metadata` workload compares native modes, umask return bits, effective UID, parent device/group and identity lifetime across five guest profiles. `virtual-created-metadata` separately compares the entire 144-byte policy record. Native creation timestamps need not all be equal; the fixed values and sparse allocation policy describe the declared virtual filesystem. Permission enforcement, credential switching, ACLs and native APFS metadata behavior remain outside this contract.

```json
{"darwin_files":{"files":[],"umask":18}}
```

[XNU creation](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c), [XNU umask](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c). [XNU rename / renameat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c).

## Same-parent regular-file rename

`rename(128)`, `renameat(465)` and `renameatx_np(488)` rename or replace a regular file within one explicitly mutable immediate parent. Even a same-name no-op requires that namespace grant; admitted no-ops preserve all observations. The low 32 flag bits may be zero or `RENAME_NOFOLLOW_ANY=0x10`. Unknown bits or EXCL+SWAP return EINVAL before reading paths; other known flags stop as unsupported. The existing component walker preserves source-before-target errors, directory-FD rules and original trailing-slash/dot checks. Directory sources stop explicitly before applying regular-file target rules. For a regular source, a successfully resolved final dot/dotdot returns EINVAL before mount or grant checks, including nested and other parents. Missing or non-directory ancestors keep their earlier errors. An ordinary directory target within the admitted parent returns EISDIR.

All existing source descriptions, including independent opens and dup, share the current `F_GETPATH` name. A removed or replaced target keeps its own last linked path, bytes, offsets, flags and mapping lifetime, even when the source moves again. Replacement never transfers the target's write grant or metadata to the source. Each object's known mutation policy updates only its ctime, plus nlink=0 for the replaced target; identity, ownership, birthtime, bytes and allocation remain attached to that object. Without a policy or after whole-EFAULT, complete metadata stays unknown. An actual move invalidates the parent's complete stat/enumeration observations; a no-op does not.

Rename consumes neither a creation inode nor a new entry, and needs no free FD. The destination canonical path/NUL replaces the source's dynamic path charge. Initial input/reference costs stay reserved. Replaced bytes and dynamic path costs can fund admission only if no old description or mapping retains that target, and are reclaimed once. Partial unmap retains the full object's byte charge until its final range is gone. Paths of 1024 bytes or more and insufficient aggregate 16 MiB capacity stop before name, metadata or descriptor changes.

Cross-parent moves and contradictory known device observations remain unsupported: equal stat device numbers do not establish shared mount identity, so the model does not invent EXDEV. Directory moves, swap/exclusive/seclude operations and permission enforcement remain future work. The original `renamed-file` program compares native and guest identity, path, replacement and mapping behavior; policy timestamps and budget rules are explicit virtual behavior.


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
not an access-control model; directory creation and bounded removal use the explicit grants described below.

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
dynamic cookie generations. Legacy `getdirentries` (196), enumeration after namespace mutation,
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

Enumeration validation (2026-10-05, Release): 498 Darwin registrations, 246 passed, 252 unavailable-backend skips, zero failures; all 63/63 required ARM64 HVF cases executed. All 11 original native macOS workloads, 40 public C/CLI/report checks without skips, five Python guest combinations with eight file workloads, and 66 runner tests passed. Counts overlap. Evidence: `build-hvf-arm64/darwin-dirents-merged-evidence/`. Intel HVF Actions remain suspended; other native transports and physical iOS are unvalidated.

## Private file mappings

`mmap` accepts `MAP_PRIVATE` regular catalogue-file mappings (`flags=0x2`
or `0x40002` with `MAP_UNIX03`). Offsets must be OS-page aligned. The complete
mapped pages contain the current file bytes, including bytes beyond a short
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

`DarwinFiles` alone owns shared file contents and resolves descriptor kind and bytes.
`DarwinMemory` owns placement, page allocation, maximum protection, mapping leases
and rollback; mapping bytes are
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
padding and reserved fields are zero. Inputs supply the initial metadata; the
optional mutation policy governs changes. Reads do not change timestamps, and
mode bits do not change catalogue access.
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
on each platform supported by the host ISA: 102 cases on ARM64, or 68 on x64.
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

1. Extend the bounded writable-file model with permission enforcement, cross-parent rename and remaining directory operations,
   shared mappings and EOF fault delivery. Keep native acceptance for cursor,
   mapping lifetime and error-order interactions as the supported set grows.
2. Extend fixed time inputs with advancing clocks and additional system observations,
   and add required Mach/thread services,
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

## Explicit time observations

`ProcessOptions::DarwinTime` / `darwin_time` supplies fixed observations for raw `gettimeofday` (116), including its third `mach_absolute_time` output, on every Darwin profile. Each of `time_of_day`, `timezone` and `mach_absolute_time` is optional: absence means unknown, while explicit zero is a value. An empty object does not supply default clocks. The model never reads host clocks, infers timezone, advances time or converts absolute ticks.

Every member of a supplied record is required. `seconds` is unsigned 32 bits, `microseconds` is in [0, 999999], and `minutes_west` / `dst_time` are signed 32 bits. Absolute ticks are unsigned 64 bits. JSON uses the shared lossless integer rules: quote decimal values outside the safe JSON integer range. Unknown fields, invalid ranges and use on non-Darwin profiles fail before image loading.

The LP64 `timeval` is 16 bytes: zero-extended seconds at offset 0, 32-bit microseconds at 8, and four zero padding bytes at 12. The timezone is two signed 32-bit fields; ticks occupy eight bytes. Requested calendar and absolute values form one initial sample, so both requested observations must exist before any output copy or pointer check. Copies then occur in order: timeval, timezone, absolute ticks. Missing timezone is detected at its phase, preserving an earlier timeval write. A later EFAULT also preserves earlier writes, and aliases follow the same order. An individually partially writable output stops unsupported before that copy, retaining prior copies. All-null arguments succeed without configuration; selective queries require only requested values.

The original `time` workload checks native behavior; guest `time-values` emits the exact configured 32 bytes through C/CLI/Python on all five guest combinations. A separate SDK oracle compares every byte against three outputs captured in one raw native call. These fixed observations do not implement advancing clocks, clock conversion, commpage counters, timers or Mach clock objects/IPC. Dyld, threads, Objective-C/Swift and Foundation/UIKit remain separate environment work. Intel HVF Actions remain suspended; this adds no native Intel or physical iOS acceptance.

```json
{"darwin_time":{"time_of_day":{"seconds":4045620583,"microseconds":654321},"timezone":{"minutes_west":-480,"dst_time":-1},"mach_absolute_time":"18364758544493064720"}}
```

[XNU gettimeofday](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_time.c), [time ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/time.h).

Time validation (2026-10-06, Release): 538 Darwin registrations, 274 passed, 264 unavailable-backend skips, zero failures; all 66/66 required ARM64 HVF cases executed. All 12 original native macOS workloads and the independent single-sample SDK byte comparison passed. Public C/CLI/report: 43/43, no skips. Python passed all five guest combinations, including exact time bytes and the eight existing file modes. All 66 runner tests, localization, capability and format checks passed. Counts overlap. Evidence: `build-hvf-arm64/darwin-time-verified-evidence/`, `darwin-time-native-first/` and `darwin-time-public.xml`.

## Mach time and return conventions

`darwin_time.timebase` supplies `numerator` and `denominator`, both nonzero unsigned 32-bit integers. The exact ratio is retained, without reduction or conversion. Mach `mach_timebase_info_trap` uses index 89: ARM64 X16=-89 or x64 RAX=0x01000059. It writes eight little-endian bytes (numerator, denominator) and returns zero. As in XNU, a wholly invalid output address still returns zero; an individually partially writable output stops before copying because its prefix effects remain unmodeled. Transport failures propagate. Missing timebase configuration stops before pointer checks, including a null pointer.

ARM64 special traps X16=-3 and X16=-4 return the complete unsigned 64-bit `mach_absolute_time` and `mach_continuous_time` observations. Each requires only its own value; explicit zero is valid. These slots are unsupported on x64, where the corresponding native Mach table entries raise EXC_SYSCALL. This does not implement advancing clocks, commpage counters, timers or Mach clock objects/IPC.

Service lookup uses only the low 32 bits of the number register; reports retain all original 64 bits. ARM64 negative numbers select Mach; x64 uses Mach class 0x01000000 and BSD class 0x02000000. Namespaces remain distinct, including BSD read/write numbers 3/4. Unknown numbers and foreign classes stop explicitly. The resolved binding owns the return convention: Mach preserves incoming flags and X1/RDX, while BSD retains its documented carry/secondary rules. X64 still updates RCX/R11 for SYSCALL return. Returning Mach records contain `result` and omit `error`, even when incoming carry is set.

The original `mach-time` workload compares flag/secondary preservation, high number bits, invalid pointers and transitions back to BSD against the ARM64 host kernel. `mach-timebase-values` emits exact configured bytes on all five guest combinations; `mach-clock-values` does so on ARM64. An independent SDK oracle checks layout and a captured native ratio. Intel HVF Actions remain suspended; x64 software tests and syntax checks do not establish native Intel or physical iOS acceptance.

```json
{"darwin_time":{"timebase":{"numerator":125,"denominator":3},"mach_absolute_time":"18364758544493064720","mach_continuous_time":"18446744073709551615"}}
```

[XNU clock traps](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/clock.c), [ARM64 entry](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/bsd_arm64.c), [ARM64 special traps](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/arm64/sleh.c), [x64 entry](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/x86_64/idt64.s).

Mach validation (2026-10-06, Release): 569 unique Darwin registrations, 293 passed, 276 unavailable-backend skips, zero failures; all 69/69 required ARM64 HVF cases executed. All 13 native workloads and both time SDK oracles passed in the final run. Public C/CLI/report: 100/100, no skips; Python covered all five guest combinations. Public comparisons run separately by platform and workload with an explicit 10-second guest budget; product defaults and deadline regressions are unchanged. Counts overlap.

Initial cold native launches exceeded the existing five-second reference limit: one independently measured 6.056 seconds, then 0.010 seconds on reuse. The unchanged native binary subsequently passed all 13 cases under the original limit; failed summaries remain retained. Separate serial verification passed after earlier wall-clock timeouts under host load. Evidence: `build-hvf-arm64/darwin-mach-time-final-evidence/`, `darwin-mach-time-native-recheck/existing-binary-recheck.json`, `darwin-mach-time-public-accepted.xml`. Evidence describes the pre-commit working tree. At that revision, ARM64 MRS/MSR NZCV were outside the checked CPU contract, so the fixture used integer instructions to observe flags. The increment below addresses that CPU gap.

## ARM64 condition flag register

The shared checked ARM64 contract admits exact `MRS Xt, NZCV` and `MSR NZCV, Xt` encodings at EL0 and EL1. Reads return only bits 31–28; writes take only those input bits and ignore all others. Reading into `XZR` discards the result; writing from `XZR` clears the four flags without reading SP. The original instructions run through each transport. The host register setter's validation and the bounded FPCR/FPSR policy are unchanged; neighboring unlisted system registers remain unsupported.

`NeverDAArch64NZCVTests` compares all flag combinations against original host instructions and checks complete scalar/vector state, memory, register boundaries, observer stop/failure, saved-context retry and shared instruction budgets. The ARM64 `mach-time` fixture now uses real MSR/MRS around SVC, covering preservation through Mach and transitions to BSD. Native HVF requirements include all six methods at both privileges plus the host oracle. ARM64 KVM/WHP and physical iOS remain unvalidated. Writable files, system information, advancing clocks, Mach IPC/threads, dyld/runtime/frameworks and device acceptance remain separate environment work.

[Arm NZCV (DDI0601, 2025-06)](https://developer.arm.com/documentation/ddi0601/2025-06/AArch64-Registers/NZCV--Condition-Flags).

## Mutable-file validation (2026-10-06)

The Release Darwin gate passed 322 of 610 registrations, with 288 unavailable-backend skips and no failures; all 72 required ARM64 HVF identities executed. The final focused run passed 102 of 114, with 12 unavailable skips, including the subsequently added EFAULT-metadata assertions. All 15 independent native workloads and 111 public C/CLI/report tests passed. These counts overlap. Evidence is retained in `build-hvf-arm64/writable-darwin-evidence/`, `writable-native-final/`, `writable-focused-final.xml` and `writable-public.xml`. The first native writable run exposed FWASWRITTEN and is retained separately; it was corrected before the passing runs. No deadlines changed. Full GitHub CI and physical iOS remain separate; Intel HVF Actions remain suspended.

Python's initial full integration run timed out in directory enumeration on three ARM64 profiles. An unchanged-argument observation passed all ten new writable scenarios, but one iOS directory call consumed 5.005 seconds wall time for 1.263 seconds CPU time and timed out. Isolated directory rechecks with the same five-second bound then passed all three ARM64 profiles in 2.43–3.17 seconds, each retiring 10,941 instructions and emitting `65`. Host load was 54–70 on 16 logical CPUs. These observations support scheduling pressure; they do not establish stable latency or erase the failed runs. Evidence: `writable-python-observation.json` and `writable-python-directory-recheck.json`.

The final unmodified Python integration method passed all five profile/ISA combinations in 41.118 seconds under the original five-second per-process limit. The earlier failed and diagnostic runs remain separate evidence.


### Mutable metadata verification, 2026-10-06

Release focused coverage: 148 registrations, 124 passed, 24 unavailable-backend skips,
zero failures. Full Darwin run: 645 registrations, 343 passed, 300 unavailable skips,
and two existing ARM64 HVF directory-enumeration timeouts. The unchanged 20-case
method recheck passed 8 with 12 unavailable skips; affected cases took 3.818/3.949s
under the original 5s limit. All 75 required ARM64 HVF identities have passing
observations across these runs; the first gate remains recorded as failed.
Public C/CLI/report 117/117 passed, including 73 Darwin comparisons; the unmodified
Python integration method passed all five profiles in 27.359s. Native original
workloads 15/15 and runner tests 66/66 passed. New allocation results are explicit
virtual-policy tests, not APFS observations. Evidence:
`build-hvf-arm64/mutation-metadata-validation-summary.json` and
`mutation-metadata-darwin-evidence/`, `mutation-metadata-directory-recheck/`,
`mutation-metadata-focused.xml`, `mutation-metadata-public.xml`.
No deadlines changed. Full GitHub CI, native Intel and physical iOS remain separate
acceptance requirements; the full macOS/iOS goal is still incomplete.

### Sparse-seek verification, 2026-10-06

The Release Darwin gate reconciled 671 registrations: 359 passed, 312 unavailable-backend skips, zero failures. All 78 required ARM64 HVF identities executed; Unicorn covered all five guest profiles. Focused file/memory/sparse-seek coverage passed 123 of 147 with 24 unavailable skips. All 16 original native programs and 122 public C/CLI/report checks passed, including 78 Darwin comparisons. The unmodified Python integration method passed all five profiles in 12.344 seconds. Evidence-runner tests passed 66/66. Counts overlap; no deadlines changed. Evidence: `build-hvf-arm64/sparse-seek-validation-summary.json`, `sparse-seek-darwin-evidence/`, `sparse-seek-native/`, `sparse-seek-focused.xml` and `sparse-seek-public.xml`. Earlier failed runs remain preserved. Allocation geometry is an explicit virtual policy, not APFS equivalence. Full GitHub CI and physical iOS remain separate; Intel HVF Actions stay suspended.

### Unlink verification, 2026-10-06

Release Darwin reconciled 708 registrations: 384 passed, 324 unavailable-backend skips, zero failures; all 81 required ARM64 HVF identities executed. Focused coverage passed 137 of 156 with 19 unavailable skips. All 17 original native programs and 128 public C/CLI/report checks passed, including 83 Darwin comparisons. The unmodified Python method passed five profiles in 16.268 seconds; runner tests passed 66/66. Independent design and implementation review found no remaining blocker. Counts overlap, deadlines are unchanged and no recheck was needed. Evidence: `build-hvf-arm64/unlink-validation-summary.json`, `unlink-darwin-evidence/`, `unlink-native/`, `unlink-focused.xml` and `unlink-public.xml`. Directory invalidation and fixed policy times are explicit model behavior; these results do not establish full filesystem/runtime or physical iOS compatibility. Intel HVF Actions remains suspended; complete GitHub CI is separate.

### Creation verification, 2026-10-06

Release Darwin: 748 registrations, 412 passed, 336 unavailable-backend skips, zero failures; all 84 required ARM64 HVF identities executed. Focused: 162 registrations, 150 passed, 12 unavailable skips. Public C/CLI/report: 133/133, including 88 Darwin comparisons. Python passed all five profiles in 9.982 seconds; original native programs 18/18 and evidence runners 66/66 passed. Initial ARM64 creation runs correctly rejected pointer-table rebases introduced by the fixture; replacing the table with inline bytes removed them without changing loader admission. The original failures and binaries remain preserved. The runner inventory expectation was updated from 27 to 28 workloads. Independent review found no remaining blocker, including added parent-observation rollback coverage. Counts overlap and deadlines are unchanged. Full GitHub CI and physical iOS remain separate; Intel HVF Actions stay suspended.

`build-hvf-arm64/create-validation-summary.json`, `create-darwin-evidence/`, `create-focused.xml`, `create-public.xml`, `create-native-final/`, `create-initial-evidence/`.

### Creation metadata verification, 2026-10-06

Release Darwin reconciled 787 registrations: 439 passed, 348 unavailable-backend skips, zero failures; all 87 required ARM64 HVF identities executed. Focused coverage passed 139 of 151 with 12 unavailable skips. Public C/CLI/report passed 145/145, including 98 Darwin input comparisons; the unmodified Python method passed five profiles in 12.211 seconds. Original native programs passed 19/19 and evidence runners 66/66. Independent review found no remaining blocker; added cases cover cross-parent device/group and global inode allocation, unlink before the first write, and umask with no free FD or usable guest input. Counts overlap; deadlines are unchanged and no failure recheck was needed. Fixed creation/mutation times and allocation remain explicit virtual policy, not native APFS observations. Full GitHub CI and physical iOS remain separate; Intel HVF Actions stay suspended.

`build-hvf-arm64/creation-metadata-validation-summary.json`, `creation-metadata-darwin-evidence/`, `creation-metadata-focused.xml`, `creation-metadata-public.xml`, `creation-metadata-native/`.

### Rename verification, 2026-10-06

Release Darwin reconciled 835 registrations: 474 passed, 360 unavailable-backend skips and one existing macOS ARM64 HVF virtual-metadata timeout (5.087s). The unchanged 20-case method recheck passed 8 with 12 unavailable skips; the affected identity took 0.113s under the original 5s limit. All 90 required ARM64 HVF identities have passing observations across these runs; the first final gate remains failed. Rename-focused coverage passed 42 of 54, with 12 skips. Public C/CLI/report passed 150/150, including 103 Darwin input comparisons; the unchanged Python method passed five profiles in 18.478s. Original native programs passed 20/20 and evidence runners 66/66. Independent review caught a nested-dot target classification error; its 4K/16K regression failed before the fix and passed afterward. An earlier test-only readonly-ftruncate expectation was corrected to EINVAL. All failures and probe revisions remain preserved, counts overlap and deadlines are unchanged. Full GitHub CI and physical iOS remain separate; Intel HVF Actions stay suspended.

`build-hvf-arm64/rename-validation-summary.json`, `rename-focused-final.xml`, `rename-darwin-final-evidence/`, `rename-metadata-recheck.xml`, `rename-public.xml`, `rename-native-final/`, `rename-review-initial-evidence/`.

## Explicit system observations

`ProcessOptions::DarwinSystem` / `darwin_system` supplies fixed observations for `sysctl(202)` and raw `sysctlbyname(274)` on every Darwin profile. Each field is optional; an omitted value or unlisted key stops as unsupported. There are no host queries or inferred version/model defaults. Strict JSON and typed validation reject malformed values and non-Darwin profiles before image loading.

`os_revision` is signed 32-bit; `cpu_count` is 1 through INT32_MAX; `memory_size` retains all unsigned 64 bits. Other fields are strings of at most 1023 bytes without embedded NUL; explicit empty strings are valid. Results include the terminating NUL. CPU and memory reports do not change scheduling or the allocation budget.

| JSON field | sysctl name | MIB |
| --- | --- | --- |
| `os_type` | `kern.ostype` | `1,1` |
| `os_release` | `kern.osrelease` | `1,2` |
| `os_revision` | `kern.osrevision` | `1,3` |
| `kernel_version` | `kern.version` | `1,4` |
| `os_version` | `kern.osversion` | `1,65` |
| `machine` | `hw.machine` | `6,1` |
| `model` | `hw.model` | `6,2` |
| `cpu_count` | `hw.ncpu` | `6,3` |
| `memory_size` | `hw.memsize` | `6,24` |

`hw.pagesize` comes from the existing guest memory policy: normally eight bytes, or four when a nonnull output has capacity exactly four. The legacy MIB `[6,7]` and name `hw.pagesize_compat` always return four bytes. The dynamic numeric OID for `hw.pagesize` remains unsupported. `hw.memsize` also narrows at exact capacity four only when its 64-bit pattern is a sign extension of a signed 32-bit value; otherwise ERANGE34 preserves output and length.

MIB counts use the low 32 bits and must be 2–12; named lengths use all 64 bits and must be below 1024. Every supplied name byte is checked before interpreting the first NUL and removing one final dot. An empty name returns ENOENT. Partial input remains unsupported. A nonnull `oldlenp` must be fully readable and writable for eight bytes before effects; faulting native length probes did not return within their deadlines, so those pointers remain an explicit unsupported boundary. Null `oldlenp` means capacity zero; null `oldp` requests only the size. A short buffer returns ENOMEM12, leaves data untouched and writes length zero. Data EFAULT preserves the old length. Input and capacity are captured before data, with the final length copy last, including aliases and retained earlier copies after transport failure.

Only nonzero `newp` together with nonzero `newlen` is a write request. Selected nodes return EPERM1 for the model’s fixed non-root identity before checking the observation or output. This includes `kern.osversion`, which is privileged-writable natively. A pointer with zero new length is ignored. Unknown keys, other trees and dynamic OIDs do not acquire guessed ENOENT results.

The original `system-info` program checks native macOS and guest ABI behavior; `virtual-system` compares exact configured bytes through C++, C/CLI and Python. A separate SDK oracle captures all nine host observations as explicit test inputs and compares both named and numeric output. This does not provide iOS device or Intel HVF acceptance.

```json
{"darwin_system":{"os_type":"Darwin","os_release":"24.test","os_revision":0,"kernel_version":"Virtual kernel","os_version":"V42","machine":"virtual64","model":"VirtualModel","cpu_count":4,"memory_size":"17179869184"}}
```

[XNU sysctl](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_newsysctl.c), [XNU hardware MIB](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mib.c), [Apple sysctl(3)](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man3/sysctl.3.html).

### System-query verification, 2026-10-06

Release Darwin reconciled 881 registrations: 509 passed, 372 unavailable-backend skips and zero failures; all 93 required ARM64 HVF identities executed. Focused checks passed 37 of 49 with 12 unavailable skips. Public C/CLI/report passed 163/163, including 113 Darwin input comparisons. The unchanged Python method passed all five profiles in 15.302s; original native programs passed 21/21 and evidence runners 66/66. Independent review found no blocker; its extra error-priority combinations and the separate SDK capture oracle passed. One new oracle compilation failed for a missing StringExtras include and passed after adding it; that log and source are preserved. Faulting native length-pointer probes remain preserved and outside the admitted contract. Only two file-header comments were normalized after the passing runs, followed by a successful rebuild. Counts overlap, deadlines are unchanged, and no runtime recheck was needed. Full GitHub CI and physical iOS remain separate; Intel HVF Actions stay suspended.

`build-hvf-arm64/sysctl-validation-summary.json`, `sysctl-darwin-final-evidence/`, `sysctl-focused-final.xml`, `sysctl-public.xml`, `sysctl-native-final/`, `sysctl-sdk-build-failure/`, `sysctl-initial-probe-evidence/`.

## Vectored file and capture I/O

`readv`/`writev` and `preadv`/`pwritev`, including nocancel entries, share the scalar file and capture owners. No new input options or host access are introduced. LP64 iovec records contain an eight-byte address and eight-byte length. The signed low 32 bits of iovcnt must be 1–1024. The complete array is copied before descriptor lookup, so later output aliases cannot rewrite the request. An individually partial array remains unsupported.

Descriptor access and positioned-stream checks precede length validation. Each length and their sum must fit INT64_MAX; regular files/directories additionally require total <= INT_MAX. Streams have no vnode limit: finite stdin clips to available bytes, and capture retains its output budget. Every negative pwritev offset is rejected before the array; preadv checks offsets after descriptor/length admission. Zero spans ignore their addresses, while descriptor, directory and offset rules still apply. EOF skips all unused tails. Positioned calls preserve the open cursor and pwritev ignores append. Ordinary append clips the entire request against the original cursor once before selecting EOF.

Copies follow vector order. A later wholly invalid span returns EFAULT while retaining earlier completed bytes, ordinary cursor progress and FWASWRITTEN after nonzero written bytes. An admitted nonzero file write that returns data-buffer EFAULT invalidates complete metadata even with a virtual success policy; argument errors, model admission refusals and backend failures preserve it. A partially writable read span stops with UnsupportedService before copying that span; earlier copies remain. An individually partial file-write source remains unsupported before any file effects. File authorization, mapping leases and aggregate storage admission precede writes; backend preflight/read failures publish no file or capture bytes.

Capture uses the combined stdout/stderr allowance before vector data access. A span crossing the user address limit contributes no bytes; earlier vectors remain. Other partial readable capture spans retain their checked prefix with EFAULT. Scalar range-error priority over the output budget is unchanged. Duplicated and redirected descriptors retain their original sink. The original `vectored-io` workload exercises all eight entries on native macOS and all five guest combinations, including public C/CLI/Python. This does not add cancellation, pipes, threads or physical iOS acceptance.

`readv`: 120/411; `writev`: 121/412; `preadv`: 540/542; `pwritev`: 541/543.

[XNU vector calls](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/sys_generic.c), [XNU iovec lengths](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_subr.c), [XNU vnode I/O](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_vnops.c).

### Vector I/O verification, 2026-10-06

Release Darwin reconciled 937 registrations: 553 passed, 384 unavailable-backend skips, zero failures; all 96 required ARM64 HVF identities executed. Focused checks passed 45/57 with 12 unavailable skips. Public C/CLI/report passed 168/168, including 118 Darwin input comparisons; Python covered all five combinations in 20.397s. Original native workloads passed 22/22, and runner tests 66/66. Independent review added a sparse positioned-write fault test that verifies cursor, actual EOF, metadata refusal and exact remaining storage capacity. Initial compilation still referenced a removed internal query in an old test; that test now checks actual captured output. An optional<bool> mistake in the new service-event assertion initially failed eight otherwise successful guest runs; it was corrected and all affected checks passed. Both failures retain source and logs. Counts overlap; deadlines are unchanged. Full GitHub CI and physical iOS remain separate, and Intel HVF Actions stay suspended.

`build-hvf-arm64/vector-validation-summary.json`, `vector-darwin-final-evidence/`, `vector-focused-final.xml`, `vector-public.xml`, `vector-native-initial/`, `vector-initial-build-failure/`, `vector-initial-assertion-evidence/`.

## File existence queries

`access(33)` and `faccessat(466)` query the current virtual catalogue without allocating a descriptor or changing file bytes, cursors, flags or metadata. F_OK proves that a name exists under the existing catalogue traversal contract. Metadata does not grant or revoke catalogue access; these calls do not establish native ancestor-search, ACL or MAC authorization. Missing or invalidated stat observations do not prevent an existence query. Deleted names return ENOENT even while old descriptors or mappings retain the object; creation, name reuse and rename use the current namespace.

Mode uses the low 32 bits. Native authorization actions come from R/W/X (bits 0–2) or extended rights (bits 9–21). When `(mode & 0x003ffe07) == 0`, the request is an existence query; other bits, including the sign bit, are ignored rather than rejected as EINVAL. Requested permissions remain UnsupportedService after successful lookup, even if metadata or mutation grants appear permissive. Known pathname/descriptor errors occur first. No permission result is guessed.

Faccessat accepts low-bit flags AT_EACCESS (0x10), AT_SYMLINK_NOFOLLOW (0x20) and AT_SYMLINK_NOFOLLOW_ANY (0x800), in any combination. Other flags return EINVAL before pathname or FD access, including when the catalogue is absent. The admitted catalogue has no symlinks and real/effective identities are fixed. Absolute paths ignore dirfd; relative lookup retains configured CWD/directory-FD requirements. Paths copy through their first NUL before relative FD checks; a missing string byte is EFAULT. Empty relative paths still check the FD: EBADF for unknown, ENOTDIR for regular files, otherwise ENOENT. Missing catalogue and unknown stream-directory identity remain explicitly unsupported.

The independent `file-access` workload compares both raw calls, ignored mode bits, flag combinations and lookup order on native macOS and five guest combinations through C++, C/CLI/Python. Native NOFOLLOW_ANY checks use a relative directory FD so host `/tmp` or `/var` symlinks do not alter the reference. Direct tests cover live namespace changes, mixed permission bits, descriptor exhaustion, metadata independence and failed guest-memory access.

[XNU access and faccessat](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU access mode bits](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/unistd.h).

### File-existence verification, 2026-10-06

Release Darwin reconciled 971 registrations: 575 passed, 396 unavailable-backend skips, zero failures; all 99 required ARM64 HVF identities executed. Focused checks passed 23/35 with 12 unavailable skips, including all 14 direct cases. Public C/CLI/report passed 173/173, including 123 Darwin input comparisons. Python passed all five combinations in 16.235s; original native workloads passed 23/23 and evidence runners 66/66. Independent design and implementation review found no blocker. Native probes preserve the initial NOFOLLOW_ANY result caused by a host /tmp symlink and the subsequent canonical-path comparison; the shared workload uses a relative directory FD. Counts overlap, deadlines are unchanged, and no runtime failure recheck was needed. Full GitHub CI and physical iOS remain separate; Intel HVF Actions stay suspended.

`build-hvf-arm64/access-validation-summary.json`, `access-darwin-final-evidence/`, `access-focused-final.xml`, `access-public.xml`, `access-native-initial/`, `access-evidence/`.

## Creating and removing directories

`mkdir(136)` and `mkdirat(475)` create directories in an explicitly mutable immediate parent. New directories inherit namespace mutation authority; initial directories retain their own grants. They inherit only the parent's known device/group identity, never complete stat records, directory size, allocation, timestamps or enumeration cookies. A new directory shadows all old observations at a reused file name. `creation_policy` still applies only to regular files: nested regular files use the inherited parent identity and the existing global inode sequence; mkdir consumes no regular-file inode. Permission enforcement and native directory metadata remain outside this contract.

All calls share the component walker. Mkdir admits a missing final name followed only by slashes; missing ancestors followed by dot/dotdot still return ENOENT, and a regular-file ancestor gives ENOTDIR. Known existing names give EEXIST. Relative paths retain FD/CWD requirements, absolute paths ignore dirfd, and string faults precede relative descriptor errors. Creation needs no available descriptor. Rejected lookup, grant, byte/entry budget or memory transport publishes no name or parent invalidation.

`rmdir(137)` and `unlinkat(472)` with AT_REMOVEDIR(0x80), optionally AT_SYMLINK_NOFOLLOW_ANY(0x800), remove empty directories created by this process. Unknown low32 flags return EINVAL before input; DATALESS and SYSTEM_DISCARDED remain unsupported. Known path/type/root errors remain; initial-directory removal is UnsupportedService. Within created directories, final dot gives EINVAL and dotdot/nonempty gives ENOTEMPTY. Any retained directory FD, including duplicates, or CWD makes removal explicitly unsupported until persistent directory-object lifetime is modeled. Open unlinked regular files and their mappings do not count as names: native comparisons confirm their bytes, inode and last linked F_GETPATH survive parent removal and name reuse.

Every created directory charges its canonical path plus NUL and one entry to the shared 16 MiB/256-entry catalogue limits. Successful deletion refunds only that directory's path and entry, preserving orphan-file bytes and leases. Successful namespace changes invalidate the direct parent's complete stat/enumeration observations; rejected changes preserve them. New directory metadata and snapshots remain unknown even with a regular-file creation policy. The original `directory-mutations` workload exercises nested creation, rename, unlink, deletion and orphan reuse across native macOS and all five guest combinations through C++/C/CLI/Python.

[XNU mkdir/rmdir](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU directory creation lookup](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_lookup.c).

### Directory mutation verification, 2026-10-06

Release Darwin: 1,017 registered, 609 passed, 408 unavailable-backend skips, zero failures; all 102 required ARM64 HVF cases executed. Focused checks: 58 passed, 12 unavailable skips, including 26 new direct 4K/16K cases. Public C/CLI/report: 178/178, including 128 Darwin input comparisons. Python passed five combinations in 17.255s; original native workloads 24/24 and runner tests 66/66 passed. Independent review checked admission, name reuse, parent identity, retained file leases and rollback. An extra native probe exposed a gap after the initial passing gate: slash-only root removal returns EISDIR, while root paths ending in dot/dotdot return EBUSY. The shared removal decision and common native/guest fixture now cover both; the earlier gate and source/binary snapshot remain preserved. Counts overlap; deadlines are unchanged. Intel HVF Actions remain suspended, with full GitHub CI and physical iOS outside this local acceptance.

`build-hvf-arm64/directory-mutation-validation-summary.json`, `directory-mutation-darwin-final-evidence/`, `directory-mutation-focused-final.xml`, `directory-mutation-public-final.xml`, `directory-mutation-native-final/`, `directory-mutation-before-root-fix/`.
