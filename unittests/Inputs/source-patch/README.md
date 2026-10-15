# Native source patch producer fixtures

These first-party sources and compiler outputs exercise NeverC's pure C and
`hosted-v1` DynCode profiles at producer revision
[`182fd2c5dc`](https://github.com/NeverSight/NeverC/commit/182fd2c5dc).
The manifests authenticate each source, LLVM IR/native object, flat payload
and audit. NeverC does not translate or compile C++ in this integration.
Regenerating an artifact requires updating its authenticated hashes.

`neverc-hosted` supplies x64/ARM64 PE C IR, objects and hosted payloads. The
callback has two noninline C frames, so an exception thrown and caught by the
surrounding C++ image must traverse actual NeverC CFI/SEH. Ordinary C fixtures
check IR, object and hosted object installation. The Windows checker compiles
independent C++ originals against the native MSVC CRT and Windows SDK.

`neverc-linux` supplies x64/ARM64 ELF C++ originals and NeverC C IR/objects.
Originals were compiled with Ubuntu 24.04 GCC/G++ 13.3.0; exact flags are in the
manifest. Use `x86_64-linux-gnu-g++` for x64 and `g++` on Linux ARM64:

```sh
g++ -O0 -fPIC -pie original-eh.cpp -o original-eh.elf
g++ -O0 -fno-pie -no-pie original-callback.cpp -o original-callback.elf
# For x64, also pass -fcf-protection=none for this exception fixture.
```

`cpp-linux-objects` separately retains genuine Clang 22.1.6 and GCC 13.3.0
C++ exception objects for both architectures, with source, producer and object
hashes. The Clang IR, Clang object and GCC object exception routes use the same
independent originals. They do not use the NeverC compiler.

The `producer-commands.json` files record direct C compilation commands,
compiler versions and hashes. Run them from the repository root with the
pinned compiler, using `-fno-lto` for a native object. The C callback PE objects
use:

```sh
neverc --no-default-config --target=aarch64-pc-windows-msvc -O2 \
  -funwind-tables -fno-lto -fno-autolink -fdyncode -fdyncode-hosted \
  -fdyncode-entry=replacement -fdyncode-keep-obj=arm64.obj \
  -fdyncode-report=arm64.audit.json -x c replacement.c -o arm64.bin
```

`check_source_patch_hosted_pe_runtime.py` and
`check_source_patch_elf_runtime.py` install these pinned artifacts with the
**current** NeverD build. They compare installed hosted code with the final
payload and execute hashed images on matching native OS hosts. This qualifies
compiler-produced bytes and OS unwinding; it does not claim that NeverC was
built or executed on the CI host. The optional NeverC source matrix covers
66 ordinary C checks and 30 structure, helper, hosted and C++ refusal checks.

`native-processes` retains 30 authenticated current-engine images as sparse
deltas. Its engine source digest, original/image hashes and publication
receipts bind native replay to the producing engine and the C-only contract.
Its C images are installed from the pinned producer artifacts above; actual
compiler selection and hosted DynCode source compilation are checked by the
separate NeverC source matrix. The snapshot manifest records the input-manifest
hashes as well as the engine identity.

`additional-processes` adds native macOS x64/ARM64 C ABI, Clang C++ exceptions
and existing NeverC pure C IR/object/hosted outputs; native Windows x64/ARM64
FH3 replays; Windows/Linux x86 compatibility processes; and explicit Linux
ARM32 QEMU processes in both ARM and Thumb mode. Execution identity is part of
the report: Rosetta, compatibility and QEMU results cannot satisfy a matching
native-host requirement. Every observation is repeated three times, with an
independent case inventory and expected result. Mach-O signatures and installed
hosted payload bytes are checked separately.

The additional producer records the actual existing NeverC executable hash
and C-only commands; it does not claim the earlier pinned compiler produced
these new Mach-O inputs, and does not build or modify NeverC. Source files,
producer inputs, originals, deltas and successful publication receipts are
authenticated before replay. Regenerate into new candidate/work directories
with `scripts/prepare_source_patch_additional_fixtures.py --help`, then verify
the candidate before replacing the committed fixture directory. The tool needs
the current built CLI/shared library, Clang/LLD, an existing NeverC, and matching
ELF x86/ARM32 GCC drivers.

`scripts/check_source_patch_additional_runtime.py` requires an explicit
`--execution` value and records actual host architecture independently of the
target. PE ARM32 and legacy Mach-O i386/iOS/watchOS CPU profiles remain in the
154-case matrix; their CPU results do not establish a native OS or unwinder.
