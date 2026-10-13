**Languages**: [English](source-patching.md) | [简体中文](zh-CN/source-patching.md) | [繁體中文](zh-TW/source-patching.md) | [日本語](ja/source-patching.md) | [한국어](ko/source-patching.md) | [Français](fr/source-patching.md) | [Deutsch](de/source-patching.md) | [Español](es/source-patching.md) | [Italiano](it/source-patching.md) | [Русский](ru/source-patching.md) | [العربية](ar/source-patching.md)

[← Documentation Index](README.md)

# Source and native object patches

NeverD compiles a replacement with the selected compiler and installs it at an explicit original function entry. The compiler owns C/C++ semantics; NeverD owns address binding, relocation, storage, unwind records and publication. Binary repair and lifted rewrites retain their own pipeline. The rest of the program is not rebuilt.

<!-- i18n-section: compiler -->
## Compiler selection

Choose `--compiler=clang`, `--compiler=gcc` or `--compiler=neverc`. Clang and ordinary NeverC can produce LLVM IR; the `object` artifact preserves native producer instructions. The CLI selects objects for GCC. The Python SDK needs an explicit `artifact="object"` for GCC. A selected compiler never silently falls back.

Use `--compiler-arg` or SDK `compiler_args` for includes, macro definitions and supported source options. Target/output overrides and response-file tokens (`@file`) are refused in all positions, including separate option values. Ordinary `-I` paths and `-D` definitions remain supported.

Use authenticated entries for the example addresses. The replacement declaration must match the original machine ABI, including calling convention and structure layouts; an address cannot prove that declaration. C++ needs `--language=c++` and the correct Clang++/G++ executable. Prefer an `extern "C"` entry or pass the exact mangled symbol. Extra helpers are allowed. Both `--entry` and `--func` select the replacement explicitly. The legacy `--from-c` address is also honored.

```sh
# Clang C -> LLVM IR
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=clang --compiler-path=/path/to/clang \
  --patch-report -o app.clang

# NeverC ordinary C -> native object
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=neverc --compiler-path=/path/to/neverc \
  --artifact=object --patch-report -o app.neverc

# GCC -> native object (CLI default for GCC)
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=gcc --compiler-path=/path/to/gcc \
  --patch-report -o app.gcc

# Independently compiled ELF x64 object
clang --target=x86_64-unknown-linux-gnu -fPIC -fno-lto -c replacement.c -o replacement.o
neverd patch app --from-object=replacement.o --entry=replacement \
  --func=0x401140 --patch-report -o app.object
```

<!-- i18n-section: profiles -->
## Verified profiles and limits

| Producer | Artifact | Verified OS runtime profile |
|---|---|---|
| Clang 22.1.6 | LLVM IR | ELF x64 C/C++; Mach-O x64/ARM64 C/C++; PE x64 C/C++ FH3 |
| Clang 22.1.6 | Native object | ELF x64 C/C++ |
| GCC/G++ 13.3.0 | Native object | ELF x64 C/C++ with a compatible Itanium runtime |
| NeverC 3389.1.5 ordinary mode | LLVM IR | ELF x64 C; Mach-O x64/ARM64 C |
| NeverC 3389.1.5 ordinary mode | Native object | ELF x64 C |

These are checked OS runtime profiles, not promises for every compiler version or option. The native installers accept all four architectures in ELF, PE and Mach-O. `dyncode` and NeverC C++ are not admitted; GCC requires a matching ELF driver and cannot produce LLVM IR. Unknown relocations, TLS, constructors, LTO, common/IFUNC symbols and unresolved imports fail clearly. The JSON receipt records compiler identity, arguments, bindings and installed addresses; native objects also record SHA-256.

The CPU matrix requires successful source, supplied-IR and native-object installation on all twelve combinations. It also covers ARMv7k watchOS and ARMv7 iOS separately, mutable/readonly state, original floating-point and stack-argument bindings, and entry execution after image rebasing. CPU execution does not certify an OS loader, C++ unwinder or arbitrary ABI; use the OS profiles above for those claims.

| Format | `x86` | `x64` | `ARM32` | `ARM64` |
|---|---|---|---|---|
| `ELF` | `CPU` | `CPU` | `CPU` | `CPU` |
| `PE` | `CPU` | `CPU` | `CPU` | `CPU` |
| `Mach-O` | `CPU` | `CPU` | `CPU` | `CPU` |

`CPU` means source/IR/object installation, reload and exact function execution. ARM ELF selects EABI5, ARM/Thumb state and the floating-point ABI from the image and registers a complete sorted `EHABI` index. PE preserves x64/ARM runtime rows and `HIGHLOW`/`DIR64`/`MOV32T` base relocations; x86 uses compiler registration code. Mach-O selects the image's CPU subtype and preserves the distinct watchOS DWARF and iOS `SjLj` contracts. Source ARM/Thumb entry branches are relative and survive rebasing.

Native objects use their own bounded function symbols, auxiliary/section definitions and unwind records; they never receive invented MC owner IDs or an IR recompilation. COFF producers need exact auxiliary sizes or singleton function sections (`-ffunction-sections`). The watchOS compiler adapter retains leaf frames to obtain real unwind rows. Native COFF/Mach-O language handlers remain outside the verified object profile; use the verified IR route for C++ exceptions. A statically linked ARM unwinder with a baked exidx registry is refused, as are unregistered absolute pointers in Mach-O and unsupported RELRO storage.

GCC C objects were also installed and executed on ELF x86, ARM32 and ARM64 through Linux QEMU user mode. The adapter selects ARM/Thumb from the image, and GNU property records use the ELF pointer-width alignment.

<!-- i18n-section: references -->
## Original references and storage

Bindings refer to external declarations: `function` requires an authenticated original function/import stub; `data` requires writable storage; `const` requires a const IR declaration. Object data bindings need a nonzero extent. Extents cannot cross storage boundaries or grant writes to RELRO. Calls to the original body of the function being replaced are refused: no saved-prologue callback trampoline exists in this profile.

New code stays RX and new mutable state stays RW. Only existing imports are reused. GOT/IAT slots and callable stubs retain distinct meanings. Mach-O needs unused load-command space, for example `-Wl,-headerpad,0x400`; PE needs unused section-header space. Insufficient space fails. ELF `.data.rel.ro` and original CET/IBT protections require additional contracts and are currently refused.

```sh
neverd patch app --from-source=replacement.c --entry=replacement --func=0x401140 \
  --bind=function:original_helper=0x401190 \
  --bind=data:shared_counter=0x404020:4 \
  --bind=const:lookup_table=0x402040:64 \
  --patch-report -o app.bound
```

Mach-O unwind-table growth also requires unused adjacent file and virtual space; header padding alone does not reserve it. The verified fixtures keep these requirements distinct.

<!-- i18n-section: exceptions -->
## C++ exceptions

ELF uses compiler FDE/LSDA data; Mach-O uses compact unwind/DWARF. PE x64 admits compiler FH3 tables with a compatible existing MSVC runtime and type identities. Type-info data may need explicit bindings. FH4, custom personalities, SEH source and active CFG/EHCont/XFG modes are refused; original protection flags are preserved.

Runtime cases cover normal return, internal catch, a new throw caught by an original caller, an original callee throwing through the replacement, exactly-once RAII cleanup, rethrow, noexcept termination and untouched original functions. The original caller must already allow propagation: a patch cannot restore catches that the original compiler eliminated around a proven no-throw function.

<!-- i18n-section: publication -->
## SDK and publication

The append-only C API is `neverd_patch_source` with `neverd_source_patch_options_v1`; `neverd_patch_source_report` returns the receipt. Object input supplies its byte length. The legacy `neverd_patch_from_c` remains available.

Inputs are snapshotted. The candidate is staged beside the destination, reloaded and checked against every requested entry before exclusive publication. Existing files and symlink aliases are not overwritten. One missing mapping fails the entire request. `--mode=inplace` is refused. Session IR transforms cannot be applied to native objects. Mach-O uses strict signature verification/resigning; signed PE is not silently stripped.

```python
report = session.patch_source(
    replacement_source, "app.patched",
    mappings={"replacement": original_address},
    compiler="gcc", compiler_path="/path/to/gcc", artifact="object",
    bindings={"shared_counter": ("data", original_counter_address, 4)},
)
```

<!-- i18n-section: verification -->
## Building and verification

Use a compatible source producer through `NEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE` (Clang 22.1.6 was checked). Integrated LLVM receives a scoped correction in a build-directory copy; the submodule and shared prebuilt cache remain unchanged. The r3/r4 prebuilt x64 PLT bug can cause an IR patch to be refused; native objects avoid that MC path. `NEVERD_TEST_REQUIRE_FIXED_LLVM=1` requires the exact corrected PLT producer.

`NeverDSourcePatchTests` contains native C/API/object/EH cases; `NeverDSourcePatchMatrixTests` requires 143 checks across the thirteen target profiles through Unicorn. `SourcePatchPE` runs seven C++ scenarios when `NEVERD_TEST_WINDOWS_CRT_LIB`, `NEVERD_TEST_WINDOWS_UCRT_LIB` and `NEVERD_TEST_WINDOWS_SDK_LIB` are supplied. Non-Windows hosts also need `NEVERD_TEST_WINDOWS_RUNNER`. `NEVERD_TEST_REQUIRE_WINDOWS_RUNTIME=1` makes missing runtime inputs fail. The dedicated Windows workflow requires native execution and rejects skipped cases.

Linux evidence from the development Mac uses Docker and an explicit Linux loader; PE evidence uses Wine 11. Mach-O ARM64 runs on the Mac itself. Native Windows CI results must be checked separately; a configured job is not a passing run. Historical counts, baseline failures and emulator limits are in [the detailed contracts and validation record](source-patching-details.md).

```sh
cmake -S . -B build-source -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DNEVERD_LLVM_PREBUILT=OFF \
  -DNEVERD_ENABLE_SEMANTIC_TESTS=ON \
  -DNEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE=/path/to/clang
cmake --build build-source --target NeverDSourcePatchTests \
  NeverDSourcePatchMatrixTests --parallel 4
ctest --test-dir build-source -L \
  '^(NeverDSourcePatchTests|NeverDSourcePatchMatrixTests)$' \
  --output-on-failure --parallel 4
```

The scoped LLVM copy also corrects Mach-O x64 GOT data references: Clang’s typed-catch table is relative to the end of its four-byte relocation word. The r3/r4 prebuilt misanchors that reference by four bytes. Publication validates the encoded field and refuses the uncorrected producer. On the development Mac, x64 Mach-O runtime cases use Rosetta 2; ARM64 cases execute directly.

Native Windows x64 qualification at [2ed7e78fc](https://github.com/NeverSight/NeverD/actions/runs/38307461805) passed all 16 focused PE/object/trampoline checks and all 143 CPU target-matrix checks without skips. The FH3 case executed seven C++ runtime scenarios; the target matrix uses Unicorn. This run used Clang 22.1.6 with native MSVC and Windows SDK inputs and integrated LLVM.
