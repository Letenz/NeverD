# Native source patch producer fixtures

These first-party test sources and their compiler outputs exercise NeverC's
`cpp-core-v2` C++17 translation and `hosted-v1` DynCode at producer revision
[`182fd2c5dc`](https://github.com/NeverSight/NeverC/commit/182fd2c5dc).
The manifests authenticate the original source, target, translated carrier,
LLVM IR/native object, flat payload and audit bytes. The generated files must
stay unchanged; regenerating a fixture requires updating its hashes.

`neverc-hosted` supplies x64/ARM64 PE objects and C++ IR. The callback source has
two noninline frames so a native exception must traverse actual NeverC CFI/SEH.
The C++ source separately checks template arithmetic and constructor/destructor
effects through IR, object and hosted object installation. The Windows checker
compiles independent originals against the native MSVC CRT and Windows SDK.

`neverc-linux` supplies x64/ARM64 ELF originals and NeverC IR/objects. Originals
were compiled with Ubuntu 24.04 GCC/G++ 13.3.0; the exact flags are in its
manifest. Use `x86_64-linux-gnu-g++` for x64 and `g++` on Linux ARM64:

```sh
g++ -O0 -fPIC -pie original-eh.cpp -o original-eh.elf
g++ -O0 -fno-pie -no-pie original-callback.cpp -o original-callback.elf
# For x64, also pass -fcf-protection=none for this exception fixture.
```

The `producer-commands.json` files record the NeverC translation and codegen
commands. Run them from the repository root with the pinned compiler. For a
fresh translation, create a new output directory; the translator refuses to
overwrite its carrier/manifest/map. The C callback PE objects use:

```sh
neverc --no-default-config --target=aarch64-pc-windows-msvc -O2 \
  -funwind-tables -fno-lto -fno-autolink -fdyncode -fdyncode-hosted \
  -fdyncode-entry=replacement -fdyncode-keep-obj=arm64.obj \
  -fdyncode-report=arm64.audit.json -x c replacement.c -o arm64.bin
```

`check_source_patch_hosted_pe_runtime.py` and
`check_source_patch_elf_runtime.py` install these pinned producer artifacts with
the **current** NeverD build. They independently compare installed hosted code
with the final payload, then execute the hashed images on matching native OS
hosts. This qualifies compiler-produced bytes and OS unwind behavior; it does
not claim that a NeverC compiler executable was built or run on that CI host.
The source adapter and translator contracts have their own six-profile matrix.
