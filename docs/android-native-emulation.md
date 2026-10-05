# Android native emulation

`android-aarch64-api28-v1` runs one native function in an Android 9 / API 28
AArch64 shared library using NeverD's CPU, address space, AAPCS64 call frames,
execution sessions, and Linux syscall models. This is a bounded native analysis
environment, with explicit inputs and observable failures. It does not boot an
Android system or supply ART, JNI, Binder, signals, a general filesystem, or a
network. It never calls host functions to satisfy a guest import.

Build with `NEVERD_ENABLE_CPU_EMULATION=ON`. The host transport is selected
independently of the guest profile; `unicorn` works without an Android device.

```sh
neverd emulate example.so --profile=android-aarch64-api28-v1 --options='{
  "backend": "unicorn",
  "instruction_limit": 100000,
  "event_limit": 10000,
  "android": {
    "entry_symbol": "inspect_buffer",
    "arguments": ["0x20000000", 4],
    "properties": {"test.device": "sample"},
    "memory": [{"address": "0x20000000", "size": 4096,
                "bytes_hex": "01020304"}],
    "read_memory": [{"address": "0x20000000", "size": 16}],
    "trace_limit": 4096
  }
}'
```

The same request works with `neverd_emulate_process_json` and the Python
`emulate_process` wrapper. C++ callers use `ProcessOptions::Android` and
`emulateProcess(..., ProcessProfile::AndroidNativeAArch64, ...)`.

## Native input contract

Choose exactly one `entry_symbol` (a defined dynamic function symbol) or
`entry_address` (a **link-time virtual address**, before `load_bias`). The
load bias defaults to `0x40000000`; it must satisfy the ELF load alignment.
Addresses and scalar arguments accept unsigned JSON integers or decimal/`0x`
strings. Results use hexadecimal strings without a prefix, like Linux reports.

Arguments are non-variadic 64-bit scalar AAPCS64 values. Values beyond x0–x7
are placed on the guest stack by `IntegerABI`. Floating-point arguments,
aggregates, and variadic classification are not inferred. Callers explicitly
supply extension of narrow values. Top-level Linux `arguments` and
`environment` are invalid for this profile.

Memory regions are explicit page-aligned address/size pairs. Initial
`bytes_hex` are followed by zeros. They are readable/writable; `executable`
defaults to false. Overlaps and reserved runtime addresses are rejected.
`read_memory` snapshots must fit the output budget and initially readable
memory. A snapshot that becomes unreadable fails explicitly. Terminal CPU or
model faults do not produce snapshots. No host addresses are exposed as guest
memory.

For input larger than the 64 KiB options JSON limit, supply `path` instead of
`bytes_hex` in a `memory` region. C++ callers set `NativeMemoryRegion::File`.
The path names an explicit regular file on the caller's host; relative paths
use the caller's working directory and JSON paths are UTF-8. The complete file
must fit the region. Preparation copies its bytes into already budgeted guest
memory and leaves the remaining bytes zero. Guest writes never modify the
file, and another workload reads its current contents again.

```json
{"address":"0x20000000","size":131072,"path":"input.bin"}
```

`path` and `bytes_hex` are mutually exclusive, including empty `bytes_hex`.
Missing files, non-regular files, oversized files and detected size changes
during copying fail before CPU execution. Keep the input stable while it is
read; copying is not an atomic filesystem snapshot. The region shares the
image/stack memory limit, and bounded reads check the preparation deadline.
Blocking host filesystem I/O itself has no hard deadline guarantee. This
explicit preparation input does not expose host files through guest APIs.

By default, DT_INIT and DT_INIT_ARRAY constructors execute in order before the
selected function, with an explicit empty argc/argv/envp. All calls share the
same instruction, event and deadline budget. `initialize: false` explicitly
requests analysis of an **uninitialized** library; the report records this
choice. A normal function return has `stop_reason: "returned"`, a
`return_value`, and CLI status 0 regardless of its integer return value.
In the default single-thread mode, raw Linux `exit`/`exit_group` retain
process-exit semantics. With guest threads enabled, `exit` ends the current
thread and `exit_group` ends the workload. Destructors are not run when the
observation ends at the selected function's return. The libc
`exit` import and automatic FINI-array execution remain unsupported.

## Linking and Android contracts

The loader reads original PT_LOAD/PT_DYNAMIC file bytes, independently of
section tables and analysis patches. It decodes ordinary RELA, Android APS2
RELA and RELR records, including dynamic symbols bounded by SysV or GNU hash
tables. The Android model handles AArch64 RELATIVE, ABS64, GLOB_DAT and JUMP_SLOT,
then applies segment permissions and GNU RELRO. Unsupported relocations,
IFUNC/TLS symbols, ELF TLS templates, text relocations, interpreters, malformed
tables and unmodeled data imports are errors. Sectionless shared libraries are
supported.

Dependencies are **not loaded transitively**. Undefined function imports are
bound to identified trap slots. Calling an unmodeled import produces
`unsupported_service` with its name and arguments; it never returns invented
success. This is selective native analysis, not a complete dynamic linker:
all such function imports have non-null trap addresses, including weak imports.
Code that tests availability of an optional provider requires a fuller linker
model. Symbol versions are not used to select alternate implementations.

The supported Bionic subset is:

- `memcpy`, `memmove`, `memset`, `memcmp`, `strlen`, `strnlen`, `strcmp`, `strncmp`.
- `strtok_r`, scanning guest byte strings and updating the caller's eight-byte,
  aligned save pointer. Delimiters may change between calls; separate contexts
  remain independent. API 28 clears the saved pointer at the final token or an
  empty remainder. A null saved cursor returns NULL without reading delimiters
  or writing memory. Only a terminating delimiter is overwritten; a final
  token with no separator can reside in read-only memory. Both write spans are
  checked before either effect, and errno is preserved. Invalid pointers and
  exhausted scan/deadline bounds stop explicitly. No host tokenizer or hidden
  process cursor supplies state; `strtok` remains unsupported.
- `snprintf`, `vsnprintf`, `sprintf`, `vsprintf`, for the bounded integer and
  byte-string formatting subset described below.
- `malloc`, `calloc`, `realloc`, `free`, with live allocation tracking and
  bounded anonymous guest memory. Zero-size allocations may return a unique
  pointer; allocation failure returns NULL and sets ENOMEM.
- `__errno`, `getpid`, `gettid`, `getuid`, `geteuid`, `getgid`, `getegid`,
  `android_get_device_api_level` (28). Identity queries share the Linux
  process model: PID/TID and real/effective UID/GID are 1000, including raw
  `svc #0` and calls resolved through the explicit dynamic catalogue. They
  preserve errno and never query host credentials. Credential changes such
  as `setuid` remain unsupported.
- `getpagesize`, returning the Android profile's 4096-byte guest page size
  without changing errno. It uses the same layout as guest mappings,
  independently of the host page size. Direct imports and explicit dynamic
  catalogue entries share this behavior; provider lifetime rules still apply.
  See the pinned Android 9
  [Bionic implementation](https://android.googlesource.com/platform/bionic/+/android-9.0.0_r61/libc/bionic/getpagesize.cpp).
- `__system_property_get`, backed only by the explicit property dictionary.
  Missing properties return length zero and write NUL; values must fit 91 bytes
  plus NUL. Host properties are never inherited.
- `time`, `gettimeofday`, and `clock_gettime`, using the shared explicit
  [`linux_time` inputs](process-emulation.md#explicit-guest-clocks). Imports,
  named dynamic calls and raw ARM64 services read the same fixed observations.
  Successful calls preserve errno; kernel errors become `-1` and errno in
  Bionic alone. API 28's `time` fallback stores through the caller's `time_t*`
  in user space, so a bad destination is a runtime failure rather than a
  returning kernel `EFAULT`. Signed timestamps retain all 64 bits, including
  negative values; they are not mistaken for raw errno bits. No guest vDSO or
  host clock is invoked. See the pinned Android 9
  [Bionic wrappers](https://github.com/aosp-mirror/platform_bionic/blob/196632fb3c59ebbf1184d791a3e7124dd0c3f22b/libc/bionic/vdso.cpp).
- `pthread_once`, executing the guest initializer on the current guest stack.
  An API 28 four-byte control transitions from 0 to 1 before the callback and
  to 2 only after normal return. A completed control returns zero without
  invoking its initializer. Nested initialization of different controls and
  modeled imports inside callbacks share the caller's execution budget.
  With cooperative threads enabled, a caller waits when another live guest
  thread owns the initializer. All waiters become runnable only after that
  callback returns and its completion word is written. Resumption restores
  the original CPU/TLS/stack and rechecks the control before completing the
  original import. Recursive initialization and running controls without a
  known owner stop as unsupported. Resetting an active control also stops. Callback
  faults and exhausted budgets leave the call result incomplete. Unwinding,
  cancellation and fork recovery are unsupported.
- `__cxa_atexit` stores the guest callback, opaque argument and DSO token;
  registration does not execute the callback or dereference those values.
  `__cxa_finalize(dso)` invokes matching registrations in reverse order;
  a NULL DSO selects all. Each registration is retired before invocation,
  including during recursive finalization. Registrations made by a callback
  are considered before older remaining entries. NULL callbacks are skipped.
  Guest callbacks share the suspended caller's CPU, stack and budgets, including
  nested `pthread_once`. A failed callback leaves the finalize event incomplete.
  The registry has a model limit of 4096 pending slots, including NULL entries;
  exceeding it stops explicitly. Finalizing all entries releases unused slots.
  Callback alignment and execute permission are checked at invocation. A DSO
  token is an opaque identity, independent of the catalogue's `dlopen` handles.
  Catalogue `dlclose` does not unload guest code or automatically finalize it.
  FILE cleanup, atfork handlers, exception unwinding and concurrent destruction
  remain outside this model. No host callback is invoked.
- `dlopen`, `dlsym`, `dlclose`, `dlerror`, using an explicit local catalogue
  described below. Function availability and implementation are separate:
  an available symbol whose call is unmodeled still stops explicitly.
- `write`, `writev`, `mmap`/`mmap64`, `mprotect`, `munmap`, `madvise`, delegated to the shared Linux
  service implementation. Bionic wrappers translate negative kernel error
  values to -1 and thread-local errno; raw `svc #0` preserves negative errno
  bits and does not update TLS errno.
  `madvise` supports the shared [KSM eligibility contract](process-emulation.md#linux-elf64-profile)
  for `MADV_MERGEABLE` and `MADV_UNMERGEABLE`; other advice stops explicitly.
- `open`/`open64`, `openat`/`openat64`, `read`, `close`, `lseek`/`lseek64`
  and `fstat`/`fstat64`
  use the explicit [`linux_files` catalogue](process-emulation.md#explicit-memory-files).
  Raw traps, imported calls and guest threads share descriptors and cursors;
  only Bionic converts negative errors into `-1` and TLS errno. No host files or
  implicit proc data are visible. File writes, directory/relative opens and
  unmodeled flags stop explicitly.
  File status requires complete explicit `metadata`; it writes the 128-byte
  AArch64 layout and preserves the open cursor. Missing observations and mixed
  writable/inaccessible outputs stop without invented values or partial bytes.
- `syscall(number, ...)` uses the same Linux service table and effects. The
  AArch64 wrapper takes the number from x0 and six arguments from x1–x6;
  x7 is unused. It preserves full-width results and applies Bionic's -1/errno
  conversion only to kernel errors. Successful calls preserve errno, and
  exit services terminate the workload without returning to the caller.
  Unknown numbers and unsupported memory or networking services stop explicitly.
  Static and dynamic calls retain their native import event; only an actual
  `svc #0` produces a raw service event.

TLS uses the API 28 Bionic layout: TPIDR_EL0 points to a guest TLS block,
`__errno` addresses slot 2, and the stack guard occupies slot 5. The guard is a
deterministic analysis value, not host randomness. `__stack_chk_guard` aliases
that value; `__stack_chk_fail` and `abort` fail explicitly. The known `__sF`
stdio data symbol has an opaque, inaccessible guest address: field reads fail
instead of receiving fabricated FILE contents. FILE I/O operations remain
unmodeled; string formatting does not expose a FILE object.

These ABI choices are based on the pinned AOSP Android 9 definitions:
[Bionic TLS slots](https://android.googlesource.com/platform/bionic/+/refs/tags/android-9.0.0_r1/libc/private/bionic_tls.h),
[`__errno`](https://android.googlesource.com/platform/bionic/+/refs/tags/android-9.0.0_r1/libc/bionic/__errno.cpp),
[system property declarations](https://android.googlesource.com/platform/bionic/+/refs/tags/android-9.0.0_r1/libc/include/sys/system_properties.h),
[`pthread_once`](https://github.com/aosp-mirror/platform_bionic/blob/android-9.0.0_r1/libc/bionic/pthread_once.cpp),
[`__cxa_atexit` and `__cxa_finalize`](https://github.com/aosp-mirror/platform_bionic/blob/196632fb3c59ebbf1184d791a3e7124dd0c3f22b/libc/stdlib/atexit.c),
and the [AArch64 syscall wrapper](https://github.com/aosp-mirror/platform_bionic/blob/android-9.0.0_r1/libc/arch-arm64/bionic/syscall.S).
The model is independently implemented; these sources specify the ABI.

### String formatting

The four string formatting imports share one Android implementation. It reads
ordinary GP arguments from x0–x7 and then the guest stack. The `v` forms consume
the caller's actual 32-byte AAPCS64 `va_list`, including its GP save area and
overflow stack; they never use the host's variadic ABI. The caller's list is
not modified. Narrow integer arguments undergo the guest's integer promotions;
`long`, `long long`, `intmax_t`, `size_t`, pointers and `ptrdiff_t` have LP64 widths.

Supported conversions are `d i u o x X p s c %`, with `hh h l ll j z t` integer
lengths, signs, alternate form, left/zero padding, and fixed or `*` width and
precision. Android's `q D O U` integer aliases and ignored grouping flag are
accepted. `%s` operates on bytes; precision bounds its memory reads. NULL strings
use `(null)`. Pointer output includes `0x`, including NULL; API 28's zero-value
`%#.0o` emits no digits. Embedded NUL from `%c` counts toward the result.

Successful calls preserve errno, write a terminating NUL when capacity is
nonzero, and return the complete required length excluding that terminator.
`snprintf(NULL, 0, ...)` still validates and counts its input. Truncation needs
only the actual destination prefix to be writable. Padding is counted without
materializing discarded bytes; input scans and retained output share the
process's existing memory and deadline limits.

Floating-point, wide text, positional arguments, `%n`, unknown conversions,
`snprintf` capacities above INT_MAX, and lengths exceeding INT_MAX stop as
`unsupported_service` before publishing output. This boundary does not simulate
Bionic's version-specific overflow or fortify error side effects. Invalid guest
memory or malformed GP `va_list` state fails explicitly. Guest strings are never
passed to host printf functions. Resolving these names with `dlsym` uses the
same implementation and provider lifetime checks as direct imports.

The independently authored implementation and compiled O0/O2 callers follow
[AAPCS64's variadic layout](https://github.com/ARM-software/abi-aa/blob/2025Q1/aapcs64/aapcs64.rst#the-va_list-type),
Android 9's [formatting entrypoints](https://github.com/aosp-mirror/platform_bionic/blob/196632fb3c59ebbf1184d791a3e7124dd0c3f22b/libc/stdio/stdio.cpp)
and [conversion behavior](https://github.com/aosp-mirror/platform_bionic/blob/196632fb3c59ebbf1184d791a3e7124dd0c3f22b/libc/stdio/vfprintf.cpp).

### Explicit dynamic symbol catalogue

Supply exact library names and their available function symbols in the native
options. The same object works through C, CLI and Python:

```json
{"android": {
  "entry_symbol": "inspect_buffer",
  "libraries": {"libc.so": ["strlen", "memcmp"]}
}}
```

`dlopen("libc.so", RTLD_NOW)` now obtains an opaque guest handle. A subsequent
`dlsym(handle, "strlen")` returns a named guest trap that dispatches to the
existing bounded Bionic model. `android.native_calls` records the requested
`library` and `symbol` on the lookup, its returned address, and the same address
and provider on the later named call. Two providers have distinct traps even
when their function names agree. These addresses and handles are local model
identities, not host addresses or recovered original device addresses.

The catalogue defaults to empty. Library names match exactly, with no filesystem
search, dependency loading, aliases, constructors or host `dlopen`/`dlsym`.
There are at most 256 libraries and 4096 total function entries, with nonempty,
NUL-free names no longer than 1024 bytes and no duplicate functions per library.
Only functions are represented; data symbols and symbol versions need a fuller
linker model.

Supported flags are any combination of LP64 `RTLD_LAZY` (1), `RTLD_NOW` (2)
and `RTLD_NOLOAD` (4), including zero and NOLOAD alone, as accepted by API 28.
Unknown bits return a linker error; recognized GLOBAL/NODELETE requests stop
as unsupported. Repeated opens share a live handle and increment its reference
count. The last close invalidates that handle; a later open receives a new one.
Calls through a provider's trap while that provider is closed stop explicitly.
Missing libraries/symbols, closed or invalid handles, and a null symbol name
return a lookup error. `dlerror()` consumes the pending guest error pointer in
API 28 TLS slot 6 once; successful lookups do not clear an earlier error, and
`errno` is independent. Error message wording belongs to the model.

An optional `default_scope` supplies the complete ordered provider list for
`dlsym(RTLD_DEFAULT, name)` (handle 0 on LP64):

```json
{"android": {
  "entry_symbol": "inspect_buffer",
  "libraries": {"libfirst.so": ["strlen"], "libsecond.so": ["strlen", "memcmp"]},
  "default_scope": ["libsecond.so", "libfirst.so"]
}}
```

The first provider containing the requested function wins; reports retain its
library identity. Entries must be distinct exact names in `libraries`. An
omitted scope is unknown and stops explicitly; `[]` asserts an empty scope and
produces ordinary lookup errors. Merely listing a library in the catalogue or
opening it with `RTLD_LOCAL` does not add it to this search.

Scope providers are explicitly resident for the whole workload. Their functions
can be called before `dlopen`; `RTLD_NOLOAD` succeeds, and balanced opens/closes
preserve their handles and callable traps. A close without an outstanding open
returns a modeled error. Other providers retain the ordinary unload behavior
above. Each workload starts with fresh handles and linker errors.

This is a caller-supplied, fixed search snapshot. It must already incorporate
the applicable namespace, global/local group visibility and search order for
the workload; the model does not derive them from the input ELF, target SDK,
dependencies or caller PC. It supplies neither actual provider binaries nor
new function implementations. ELF import binding is unchanged by this scope.

`dlopen(NULL)`, `RTLD_NEXT`, `RTLD_GLOBAL`, `RTLD_NODELETE`,
Android linker namespaces and `android_dlopen_ext` remain unsupported. They
stop with a diagnostic rather than selecting a guessed process-wide scope.
`RTLD_NEXT` additionally requires an authenticated caller position.

ABI references: Android 9 [`dlfcn.h`](https://android.googlesource.com/platform/bionic/+/refs/tags/android-9.0.0_r1/libc/include/dlfcn.h),
[`dlsym`/`dlclose`](https://android.googlesource.com/platform/bionic/+/refs/tags/android-9.0.0_r1/linker/linker.cpp),
and [`dlerror`](https://android.googlesource.com/platform/bionic/+/refs/tags/android-9.0.0_r1/linker/dlfcn.cpp).

## API 28 mutexes

The Bionic model implements `pthread_mutexattr_init`, `destroy`, `gettype`,
`settype`, `getpshared`, `setpshared`, `getprotocol`, and `setprotocol`, plus
`pthread_mutex_init`, `destroy`, `lock`, `trylock`, and `unlock`. LP64 attributes
occupy eight bytes. Mutex objects occupy 40 bytes with four-byte alignment;
their guest bytes are authoritative, including static initializers. Ordinary
operations retain padding and reserved storage. Initialization clears all
40 bytes before interpreting an overlapping attribute, including when an
invalid type returns EINVAL.

Normal, recursive and error-checking mutexes retain their distinct behavior.
Error-checking self-lock returns EDEADLK; busy try-lock returns EBUSY;
recursive depth exhaustion returns EAGAIN; a non-owner unlock of a recursive
or error-checking mutex returns EPERM. These are direct pthread return values
and do not change errno. Ownership uses the same guest TID as `gettid`.
Destroyed-object use stops explicitly, following the API 28 target behavior;
older app-target compatibility is not modeled.

With guest scheduling enabled, contended locks suspend the original import
and mark the guest object contended. Unlock wakes one matching waiter in
round-robin order; a wake permits another acquisition attempt. The waiter
reloads guest state and permissions after restoring its CPU context, and
returns through its original event only after acquiring the lock. A competing
acquirer can make it wait again. Resumed acquisitions retain the contended
bit, so subsequent releases do not strand other sleepers. Recursive unlock
wakes only when its final level is released. Normal locks never use the
owner field. State snapshots are confined to a single modeled operation.

Without guest scheduling, blocking and wake transitions stop explicitly.
The process-shared bit is retained within the workload's single address
space; it does not introduce an external process. Priority inheritance, timed
operations and nonzero futex padding that would require an EAGAIN retry loop
remain unsupported. Invalid state, changed mutex attributes while suspended,
and denied memory fail without completing the pending lock. Complete write
spans are checked before changing state or ownership. No host mutex or
futex supplies guest behavior.

ABI references: Android 9 [`pthread_mutex.cpp`](https://github.com/aosp-mirror/platform_bionic/blob/android-9.0.0_r1/libc/bionic/pthread_mutex.cpp)
and [`pthread_types.h`](https://github.com/aosp-mirror/platform_bionic/blob/android-9.0.0_r1/libc/include/bits/pthread_types.h).
These declarations and state rules inform an independent implementation.

## API 28 thread attributes

`pthread_attr_init`, `destroy`, and the get/set pairs for `detachstate`,
`inheritsched`, `schedpolicy`, `schedparam`, `stack`, `stacksize`, `guardsize`
and `scope` operate on guest LP64 storage. The 56-byte, eight-byte-aligned
object keeps its padding and reserved bytes during initialization: only its
six fields are assigned. Defaults include a 1 MiB minus 16 KiB stack and a
4 KiB guard. Destruction fills all 56 bytes with `0x42`.

Getters write four-byte integers or eight-byte pointers/sizes as declared.
Inheritance flags take precedence over API 28's historical scheduling-policy
fallback. Stack-size-only assignment accepts any size at least 16 KiB;
assignment of a stack address and size additionally requires page alignment.
Stack addresses are opaque guest values. Guard sizes and scheduling policies
retain the supplied values without inventing validation absent from Bionic.
Invalid enums and sizes return EINVAL before accessing the attribute. Scope
operations ignore the attribute: system scope succeeds and process scope
returns ENOTSUP. These direct pthread error returns preserve errno.

The model checks all affected spans before publishing writes, and preserves
ordered reads/writes when `getstack` outputs overlap the attribute. Invalid
memory or alignment stops explicitly. Attribute assignment does not create a
thread or authorize a scheduling policy. The shared symbol catalogue also supplies
named dynamic calls through `dlsym` and enforces provider lifetime.

This independently implemented model follows pinned AOSP
[`pthread_attr.cpp`](https://github.com/aosp-mirror/platform_bionic/blob/196632fb3c59ebbf1184d791a3e7124dd0c3f22b/libc/bionic/pthread_attr.cpp),
[`pthread_types.h`](https://github.com/aosp-mirror/platform_bionic/blob/196632fb3c59ebbf1184d791a3e7124dd0c3f22b/libc/include/bits/pthread_types.h),
[`pthread.h`](https://github.com/aosp-mirror/platform_bionic/blob/196632fb3c59ebbf1184d791a3e7124dd0c3f22b/libc/include/pthread.h)
and [`pthread_internal.h`](https://github.com/aosp-mirror/platform_bionic/blob/196632fb3c59ebbf1184d791a3e7124dd0c3f22b/libc/bionic/pthread_internal.h).
`AndroidThreadAttributes.def` owns layout constants; the existing Android
symbol and diagnostic tables retain their respective vocabularies.

## Cooperative guest threads

The default `android.thread_limit: 1` preserves the single-thread contract.
Values 2 through 256 enable a deterministic guest scheduler and bound the
total identities created, including the entry thread. Retired identities are
not reused. `pthread_create`, `pthread_join`, `pthread_detach`, `pthread_self`,
`pthread_equal`, `pthread_exit`, `pthread_getattr_np`, and `pthread_gettid_np`
share the imported and dynamically resolved call paths.

Threads have separate saved CPU contexts, stacks, TPIDR_EL0, errno, linker
errors and callback continuations. They share guest memory, allocations,
library handles and the process destructor registry. Scheduling rotates at
`instruction_quantum`, a blocking join, or thread completion. Native calls
consume the current quantum instead of restarting it. The shared execution
budget is never replenished. This is cooperative single-core execution, not
SMP or a host pthread implementation. Normal, raw and variadic `gettid` and
mutex ownership use the same current identity; `getpid` remains process-wide.

Creation admits normal scheduling and a model-allocated stack. Inherited and
historical API 28 flags retain their policy interpretation. Explicit stacks,
unknown flags and scheduling policies requiring unmodeled behavior stop.
Identity or memory capacity exhaustion returns EAGAIN without changing errno
or publishing a handle. Pointer faults remain failures. Stack and guard sizes
round to 4 KiB; each identity reserves a model address slot below a 64 MiB
ceiling. A PROT_NONE guard precedes writable stack bytes, and a separate TLS
page follows. `pthread_getattr_np` reports that actual model span, including
the guard, and preserves copied reserved attribute bytes. Handles are opaque
unmapped identities, distinct from TIDs; this does not reconstruct Bionic's
private TCB or its allocator-dependent placement. Changed TLS identity slots
or TPIDR_EL0 stop Bionic calls explicitly.

A join retains its original service continuation until the target finishes,
then publishes the full pointer result and releases its storage. Detached
threads release storage on completion. Invalid non-null or retired handles
fail at the API 28 target boundary; NULL lookup returns ESRCH, self-join
returns EDEADLK, and detached or already claimed targets return EINVAL.
No runnable thread with outstanding join, once or mutex waits is an explicit unsupported stop.
`pthread_exit` returns its pointer to a joiner; raw `SYS_exit` terminates only
the current thread and leaves its pthread result zero. When the last thread
exits without an entry return, its low eight status bits become the workload
exit status. `exit_group` terminates
the workload. Guest cleanup handlers, TLS keys/destructors, exit during a
once/finalize callback, timers, cancellation,
signals and clone remain unsupported. No cleanup or completion is invented.

By default the selected entry's return stops observation, even with live
children. `android.drain_threads: true` continues those children under the
same limits. The entry's `return_value` remains visible if a later child stops
with an error. `android.threads` records identity, placement, waiting,
completion and retirement; completion of its initial row means the selected
invocation ended, not that the process exited. Named calls and raw services
include `thread_id`. `android.trace_threads` partitions the existing PC trace
using zero-based `trace_index` and `thread_id`. Each new run consumes 16 bytes
of the remaining report allowance; insufficient allowance truncates trace
retention, not execution. Thread metadata reserves 128 bytes per allowed
identity in addition to the trace and memory snapshots.

These independently implemented API rules use AOSP Bionic revision
`196632fb3c59ebbf1184d791a3e7124dd0c3f22b`:
[`pthread_create.cpp`](https://github.com/aosp-mirror/platform_bionic/blob/196632fb3c59ebbf1184d791a3e7124dd0c3f22b/libc/bionic/pthread_create.cpp),
[`pthread_join.cpp`](https://github.com/aosp-mirror/platform_bionic/blob/196632fb3c59ebbf1184d791a3e7124dd0c3f22b/libc/bionic/pthread_join.cpp),
[`pthread_detach.cpp`](https://github.com/aosp-mirror/platform_bionic/blob/196632fb3c59ebbf1184d791a3e7124dd0c3f22b/libc/bionic/pthread_detach.cpp),
and [`pthread_exit.cpp`](https://github.com/aosp-mirror/platform_bionic/blob/196632fb3c59ebbf1184d791a3e7124dd0c3f22b/libc/bionic/pthread_exit.cpp).
They establish a bounded model contract, not native Android equivalence.

## Evidence and limits

`android.native_calls` records import arguments and nullable results.
`android.trace` records **admitted instruction attempts**, including service
instructions; it does not claim every attempt retired. `trace_limit` bounds
retention, and `trace_truncated` distinguishes an incomplete trace. Trace
storage plus requested memory snapshots must fit `output_limit`. Output streams
also obey the existing process output limit.

A trace demonstrates behavior for its explicit inputs. It is not proof of
unvisited branches or arbitrary input equivalence. Unknown services and CPU
instructions, invalid pointers, stack-check failures, and exhausted budgets
remain visible stops. No unsupported instruction is replaced by NOP.
