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

The service inventory is `exit`, `write`, `getpid`, `getppid`, `getuid`,
`geteuid`, `getgid`, `getegid`, `mmap`, `mprotect` and `munmap`. PID, UID and
GID are deterministically 1000, and parent PID is 1. Output descriptors 1 and
2 are captured byte sinks, including NUL and non-UTF8 bytes; other descriptors
return EBADF. A partial readable prefix is captured, but a subsequent copy
fault retains EFAULT, consistent with XNU's
[write error propagation](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/sys_generic.c).

Memory services support private anonymous data mappings with descriptor -1
and offset zero (`flags=0x1002`). Lengths and nonfixed hints round up to the OS
page size; an occupied hint searches upward before falling back. The raw
legacy zero-length mmap succeeds with address zero without allocating;
`MAP_UNIX03` is outside this profile. Unmap/protect addresses must be aligned.
NONE, READ and WRITE protections are supported, and WRITE
implies READ. Physical owners are per OS page, so partial unmap releases its
budget and subsequent mappings are zeroed. Maximum protections are distinct
from current rights. A failed protect across a hole or maximum-rights boundary
leaves the complete range unchanged. The rules are grounded in XNU's
[BSD VM services](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_mman.c).

File/shared/fixed/JIT mappings, executable anonymous mappings, Mach traps,
indirect system calls, threads, signals, filesystem/network services, dyld
linking, Objective-C/Swift runtime and Foundation/UIKit are outside this
profile. They stop explicitly. This is neither a full Apple OS compatibility
layer nor the iOS Simulator application.

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
fixtures; missing `ld64.lld` cannot turn the required suite into a skip. Intel
HVF, Linux KVM and Windows WHP runtime evidence still require their own hosts.

### Local verification, 2026-10-03

Release evidence on Apple M4 Max / macOS 15.6.1:

| Check | Executed successfully | Failed | Skipped |
| --- | ---: | ---: | ---: |
| Darwin process, image and VM tests | 114 | 0 | 133 |
| Execution session and shared image mapping | 26 | 0 | 35 |
| Linux/Windows processes and public C API/CLI | 168 | 0 | 235 |
| Full native HVF gate, Unicorn disabled, 20 owners | 811 | 0 | 5,842 |

Skipped cases belong to unavailable backends, nonmatching ISAs or the disabled
software backend. Matching ARM64 HVF cases are required to run. The native gate
has no missing registrations or unexecuted required cases. Its 811 successes
include 57 Darwin checks; the rows above overlap and are not additive.
The host-specific gate requires each native Darwin platform's startup/service
fixture explicitly: three on ARM64, two on Intel. It fails on a missing
registration as well as a skipped case. Required totals are 12 and 11 including
the common HVF cases. The script also records exact required names and host ISA.

All five Darwin platform/ISA combinations also passed through the Python SDK
and built shared engine, alongside the existing Linux/Android integration
tests (three methods, no skips). The capability manifest, SDK declaration audit,
localized documentation checks and new-source formatting passed. Script audits
ran 234 tests; one capability inventory expectation was updated for the new
entry and its focused rerun passed.

Evidence stays in the disposable build trees: `build-hvf/verification/` contains
the focused and process regression JUnit files; `build-hvf-native/darwin-evidence/`
contains the native inventory, results, log and summary. This does not establish
Intel HVF, Linux KVM or Windows WHP hardware execution for this change.

The rebuilt desktop bundle passed dependency and signature checks for 186
Mach-O images and its Cocoa startup smoke. A signed probe loaded the packaged
engine and matched 18 reports against the separate `BUILD_TESTING=OFF`,
Unicorn-OFF CLI: normal exit/output, partial-copy EFAULT, anonymous memory,
CPU fault, unsupported service and instruction limit across all three ARM64
platforms. Every report selected HVF. Both x64 platforms' explicit HVF requests
were rejected on this ARM64 host. Evidence is `darwin-delivery.json` in the
verification directory; it records the bundled engine's source hash and the
separate CLI/bundle build configurations.

The stronger host gate passed all 811 cases again, including its 12 required
ARM64 cases; evidence is `build-hvf-native/darwin-host-gate-evidence/`.
Its 96 script/inventory/result/CI regression tests passed, including simulated
ARM64/Intel selection, native skips, deleted guest registrations and unknown
host rejection.

With HVF and Unicorn both disabled, the Darwin, HVF and configuration targets
also built successfully: 38 checks passed, zero failed, and 231 backend cases
skipped. `otool -L` confirmed that the Darwin test executable has no
Hypervisor.framework dependency. This is build/diagnostic isolation evidence,
not guest execution evidence; results are under
`build-hvf-disabled/verification/`.
