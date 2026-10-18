# Offline directory creation and substring search

`linux_files.directory_creation` opts into a bounded, noatime tmpfs directory
provider. It is independent of the Android Application files directory and the
process working directory. None of these inputs is inferred from a host path.

The object has exactly three fields: `umask` (0..511), `limit` (positive reserved
new-directory count), and `parents` (existing directory snapshots). Each parent
has `path` and the same complete twelve-field `metadata` object used by regular
files. Its path must be explicitly declared in `linux_files.directories`, or be
`/`. Metadata must describe a directory with unique nonzero inode, block size
4096, zero allocated blocks, size `20 * (2 + immediate entries)` and link count
`2 + immediate subdirectories`. Initial catalogue and reserved new entries must
fit the common namespace and byte budgets. An omitted provider remains unknown.

For example, an empty declared `/work` can be supplied as:

```json
{"directory_creation":{"umask":18,"limit":8,"parents":[{
  "path":"/work","metadata":{
    "device":7,"inode":91,"mode":16877,"link_count":2,
    "uid":1000,"gid":1000,"size":40,"block_size":4096,"blocks":0,
    "access_time":{"seconds":100,"nanoseconds":0},
    "modification_time":{"seconds":100,"nanoseconds":0},
    "change_time":{"seconds":100,"nanoseconds":0}
  }
}]}}
```

Creation requires an explicit realtime clock. `mkdir` and `mkdirat` share the
Linux kernel owner across x64, AArch64, raw syscalls and Bionic. Absolute paths
ignore dirfd; repeated separators and trailing slashes retain Linux behavior.
Missing parents, file parents and existing names retain their errors. The
provider admits ancestor traversal and excludes ACLs, security hooks, mount
policy and additional credential groups. Mode checks use the existing workload
UID/GID (1000), never the parent owner as the caller. Known ancestors require
search permission; the parent requires write and search. Requested set-ID bits
are removed, the umask applies, and parent setgid group/bit inheritance is kept.

New children inherit the provider. The child gets a workload-local inode, two
links, size 40, zero blocks and the creation time in all three timestamps. The
parent gains one link and twenty logical bytes; mtime/ctime advance while atime
stays unchanged. Path lookup, `stat/fstat`, open descriptions, FD aliases and
`getdents64` see the same objects. New entries append after existing directory
cookies; an open cursor at EOF can see a subsequent insertion without repeating
an earlier entry. Closing a descriptor does not delete the directory. Failed
admission changes no namespace or metadata. Reservation exhaustion stops with
an explicit model limitation, rather than inventing disk exhaustion.

These are selected memory-filesystem semantics, not ext4/f2fs metadata or a
complete Linux permission model. Relative paths, dot walks, symlink traversal,
chmod, removal, rename and general creation of regular files remain separate
unsupported boundaries. Existing catalogue access contracts are unchanged.
The semantic references are Linux 4.14
[`vfs_mkdir`](https://github.com/torvalds/linux/blob/v4.14/fs/namei.c),
[`inode_init_owner`](https://github.com/torvalds/linux/blob/v4.14/fs/inode.c) and
[tmpfs inode/entry accounting](https://github.com/torvalds/linux/blob/v4.14/mm/shmem.c).
The model is independently implemented and does not execute host filesystem calls.

Bionic `strstr` returns the first exact byte match, the original haystack for an
empty needle, and zero when a terminated haystack has no match. It preserves
errno and input bytes, including overlapping inputs and high-bit bytes. It
reads no haystack after a match or NUL. Both scans and prefix-table storage are
bounded by the model memory limit; prefix preparation and guest reads respect
the execution deadline. Invalid pointers or exhausted bounds fail explicitly.
A linear prefix search avoids repeated rescanning. This implements the observable
[API 28 string contract](https://github.com/aosp-mirror/platform_bionic/blob/android-9.0.0_r1/libc/upstream-openbsd/lib/libc/string/strstr.c)
with the shared Bionic dynamic-provider lifetime checks.

Synthetic coverage lives in `LinuxFileNamespace`, `AndroidFiles`, `AndroidSearch`
and `ProcessReport.DirectoryCreationRequiresCompleteBoundedMetadata`. Native
callers run at O0/O2 across relocation formats and available CPU backends.
