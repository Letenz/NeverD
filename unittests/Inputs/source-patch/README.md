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
