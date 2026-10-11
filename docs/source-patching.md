# Source and native object patches

NeverD can compile a replacement with a selected compiler, bind its exact entry
symbol to an original function address, and install it in a new binary. The
compiler owns the replacement's language semantics. NeverD owns address binding,
placement, relocation, unwind registration, entry redirection and publication.
This route does not decompile and rebuild the rest of the program.

## Compiler selection

```sh
# Clang C, through LLVM IR
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=clang --compiler-path=/path/to/clang \
  --patch-report -o app.patched

# NeverC ordinary C compilation, preserving the produced native machine code
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=neverc --compiler-path=/path/to/neverc \
  --artifact=object --patch-report -o app.neverc

# GCC uses native objects by default in the CLI
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=gcc --compiler-path=/path/to/x86_64-linux-gnu-gcc \
  --patch-report -o app.gcc

# An independently compiled native object uses the same installer
clang --target=x86_64-unknown-linux-gnu -fPIC -fno-lto -c replacement.c -o replacement.o
neverd patch app --from-object=replacement.o --entry=replacement \
  --func=0x401140 --patch-report -o app.object
```

Addresses above are examples; use authenticated function entries from the input.
The replacement declaration must match the original machine ABI, including
argument/return conventions and structure layouts. A stripped binary generally
cannot prove a complete C++ type declaration. An address is target selection,
not proof of that declaration.

Use `--language=c++ --compiler=clang --compiler-path=/path/to/clang++` for C++.
Use `--compiler=gcc --compiler-path=/path/to/g++ --language=c++` for G++ native
objects. An `extern "C"` entry makes naming explicit; otherwise pass the exact
mangled symbol. Overloads and multiple externally visible definitions are not
guessed. A replacement can have additional helper functions without replacing
original functions with similar names.

`--from-c` retains the C-only compatibility entry. Its nonzero `--func` address
now actually selects the original function. Requests that formerly reported
success after installing zero entry jumps now fail. New requests should specify
both `--entry` and `--func`. New APIs represent address presence separately from
address zero. Legacy name matching remains centralized for requests without
explicit mappings; native objects require explicit mappings.

## Current profiles

| Producer | Artifact | Target and language verified |
|---|---|---|
| Clang 22.1.6 | LLVM IR | ELF x64 C/C++; Mach-O ARM64 C/C++; PE x64 C/C++ FH3 |
| Clang 22.1.6 | Native object | ELF x64 C/C++ |
| GCC/G++ 13.3.0 | Native object | ELF x64 C/C++ with a matching Itanium ABI runtime |
| NeverC 3389.1.5 ordinary mode | LLVM IR | ELF x64 C; Mach-O ARM64 C |
| NeverC 3389.1.5 ordinary mode | Native object | ELF x64 C |

These are fixed profiles, not compatibility promises for every compiler version,
flag or binary. The checked target, actual compiler path/version, arguments,
artifact, requested bindings and installed addresses appear in the JSON report.
Native objects additionally have a SHA-256 digest. An external object has an
unknown producer; its file name does not establish compiler identity.

The IR route uses NeverD's LLVM code generator. Native objects retain producer
machine instructions and section contents, changing only admitted relocation
fields and the original entries. NeverC's `-fpatchable-function-entry=16` has been
checked through this route: every non-relocation text byte, including the
compiler's added entry padding, survived installation. Other MIR/obfuscation
transformations need their own relocation and runtime verification.

NeverC is optional. Ordinary NeverC compilation does not enable dyncode. Explicit
`--compile-mode=dyncode` currently fails because its code-block/runtime contract
has not been adapted. NeverC C++ is not an admitted source profile. GCC cannot
produce the LLVM IR artifact. COFF/Mach-O native objects are not admitted.
Additional architectures are outside the runtime-verified source profiles;
matching a target triple alone does not establish runtime support. Additional
Windows EH encodings need their own contracts and runtime profiles. The existing
lifted rewrite path retains its existing rules.

## References and storage

```sh
neverd patch app --from-source=replacement.c --entry=replacement --func=0x401140 \
  --bind=function:original_helper=0x401190 \
  --bind=data:shared_counter=0x404020:4 \
  --bind=const:lookup_table=0x402040:64 \
  --patch-report -o app.bound
```

Bindings name external declarations. `function` requires an authenticated
original entry or import stub. `data` requires writable original storage;
`const` requires a const declaration on the IR route. Data extents cannot cross
a section/segment boundary or promise writable access to RELRO. Object data
bindings require an explicit nonzero size; IR bindings can infer a declaration's
allocation size. New writable globals and zero-initialized state occupy a
separate RW allocation, and code remains RX. ELF absolute data/function pointers
are registered with the dynamic loader, retaining the original dynamic symbol
indices and their version/dependency relationships.

A callable import stub, a GOT/IAT pointer slot and the pointed-to address have
different meanings. The resolver preserves that distinction. Only existing
imports are reused; it does not add arbitrary libraries or import-table entries.
Missing runtime symbols fail, including stack-protection and exception symbols.
The installer does not disable those requirements to make a patch fit.

An explicit binding to the original body of a replaced function is rejected.
There is no saved-prologue callback trampoline in this profile. Calls through the
original entry reach the replacement after installation.

Mach-O writable storage currently needs enough unused load-command header space
for the new segments. Test inputs reserve it with `-Wl,-headerpad,0x400` or more.
Insufficient space fails before publication. Existing code is not moved to
manufacture that space.

PE x64 source patches put new code in RX storage, mutable state in RW storage,
and merged runtime/base-relocation tables in read-only storage. Original runtime
rows and base relocations remain intact. The image must have unused section-header
space. Active CFG, EH continuation protection, XFG and other guard modes require
additional compiler/installer contracts and are rejected without changing flags.
Informational CRT instrumentation flags alone do not enable these modes.

The native ELF object reader retains an entire single object, including validated
COMDAT groups and link-order members. It supports bounded x64 PC32/PLT32, GOTPCREL
(including GOTPCRELX forms), and writable 64-bit pointer relocations. It validates
CFI ranges against exact object function symbols. Unknown relocations, malformed
symbols, overlapping writes, TLS, IFUNC, common symbols, LTO, archives, unresolved
weak references and dynamic initializer/finalizer sections fail clearly.
Post-relocation read-only allocations such as `.data.rel.ro` need additional
RELRO registration and are currently rejected. CET/IBT original images require
an ENDBR-preserving entry scheme and are currently rejected without changing
protection flags.

## Exception behavior

ELF uses the compiler's FDE/LSDA records and a rebuilt registered search table.
Mach-O uses compiler-generated compact-unwind/DWARF data. The checked runtime
cases cover normal returns, internal catch, a new throw caught by an original
caller, an original callee throwing through the replacement, exactly-once RAII
cleanup, rethrow, noexcept termination and untouched original functions.

PE uses the compiler's new x64 unwind programs and C++ FH3 state/handler tables.
Compiler function/funclet ranges authenticate these new records; original lifted
EH identities are neither copied nor fabricated. The checked producer is Clang
with the MSVC ABI and an existing compatible runtime. Type-info vtables and shared
data may need explicit bindings, for example
`--bind=const:??_7type_info@@6B@=0x140003008:8` (an example address, not a default).
The private type descriptors emitted by the compiler receive writable storage
and load-time base relocations. FH4, custom personalities and SEH source profiles
are not admitted by this path.

The original call site must already permit exception propagation. A compiler may
remove catches or cleanups around an original function it proves cannot throw;
installing a new throwing function does not recreate those removed caller paths.
The fixed C++ fixtures therefore use an originally throwing target. The original
runtime must provide matching personality functions, type identities and any
stack-protection dependencies. Symbol-name equality alone is not a general ABI
compatibility proof.

Linux x64 runtime evidence on the development Mac uses Docker and the Linux
dynamic loader (`/lib64/ld-linux-x86-64.so.2`). This exercises the Linux exception
runtime but is not evidence of direct launch on x64 hardware. Some emulators
provide an incorrect `AT_PHDR` for a moved program-header table; explicit loader
launch was used for those environments. The native Linux test suite also runs
ordinary launches when executing on Linux x64.

PE runtime evidence currently uses Wine 11's Windows exception runtime in an
x64 Docker environment. Its seven C++ scenarios are also available as an optional
test on a Windows x64 host/runner. This is not a claim of Windows hardware
coverage or forced-ASLR execution. On the development emulator, Wine required
its preloader to be disabled and the Linux ELF loader to be invoked explicitly.

### LLVM dependency correction

The address-model backend in the r3 and pinned r4 LLVM releases interprets the
x64 PLT specifier as an AArch64 page modifier. Late compiler calls such as
`_Unwind_Resume` can therefore receive a wrong displacement. NeverD checks these
encoded source-patch references and refuses an invalid image.

The scoped backend correction is included in
[`cmake/patches/llvm-aarch64-specifier-scope.patch`](../cmake/patches/llvm-aarch64-specifier-scope.patch).
The integrated build applies this correction to a build-directory copy of the
single LLVM source file. It preserves the submodule checkout and revision, and
accepts an already corrected source. An unrecognized source fails configuration
instead of silently skipping the correction. Use an integrated build:

```sh
cmake -S . -B build-source -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DNEVERD_LLVM_PREBUILT=OFF \
  -DNEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE=/path/to/clang
cmake --build build-source --target neverd NeverDSourcePatchTests \
  NeverDNativeExceptionRewriteContractTests --parallel 4
```

Point the test compiler setting at a compatible Clang (22.1.6 was verified),
which may differ from the system compiler used to build NeverD itself.
The shared prebuilt cache is not rewritten. Its r3/r4 backend remains subject to
the displacement check and may refuse an IR patch. The native object route does
not recompile through this LLVM MC path. The standalone patch can also be applied
when building the LLVM fork outside NeverD.

## SDK and transaction behavior

The append-only C API is `neverd_patch_source` with
`neverd_source_patch_options_v1`; `neverd_patch_source_report` returns the JSON
receipt. The caller supplies byte length explicitly for object input. The legacy
`neverd_patch_from_c` symbol remains available.

```python
report = session.patch_source(
    replacement_source, "app.patched",
    mappings={"replacement": original_address},
    compiler="gcc", compiler_path="/path/to/gcc", artifact="object",
    bindings={"shared_counter": ("data", original_counter_address, 4)},
)
```

Inputs are snapshotted; the candidate is staged beside the destination, checked
against every requested installation, reloaded, and published exclusively only
after its exact entry bytes and executable destinations agree with the receipt.
Any missing mapping fails the whole request. Existing output files and symlink
aliases are not overwritten. Mach-O ad-hoc signatures use the existing strict
resigning/verification path; signatures requiring a separate signing workflow
are refused. Signed PE input is not silently stripped.

`--mode=inplace` is explicitly rejected for these inputs. Session LLVM IR
transforms apply to IR artifacts; they cannot be requested for native objects.
Compiler options are argument arrays, with target/output modes owned by each
adapter. Selected compilers never silently fall back to another executable.

## Validation record (2026-10-12)

Before integration with the refreshed `dev`, the focused
source/native-exception/COFF suites ran 313 checks: 311 passed and
two Linux-only checks were skipped on the development Mac. The integrated build
with the corrected LLVM dependency ran 29,577 registered checks: 29,480 passed,
31 were skipped, 64 failed and two timed out. All 66 failures/timeouts were also
reproduced against the original baseline; no new failure was found in this run.
The 3,216 composed semantic cases across four architectures and three formats
passed. These semantic checks do not extend the runtime-verified profile table.

The Python SDK checks against the built shared library ran 154 tests: 109 passed
and 45 lacked the required platform or fixture. The ABI audit and mypy passed.
Runtime evidence and its platform limits are described above; skipped coverage
and reproduced baseline failures are not counted as passes.

The integration checkpoint at `ce298f758` uses the pinned r4 LLVM with the
build-directory correction. Its source, native-exception, COFF and
plugin suites ran 475 registered test executions: 473 passed and two Linux-only
checks were skipped on the Mac. There are 330 distinct test names; shared
checks run in both the lift and focused exception binaries. The required exact
PLT producer check passed with `NEVERD_TEST_REQUIRE_FIXED_LLVM=1`. Another 364
format and shared IR checks passed, including the newly integrated return-proof
and cleanup regressions.

All 3,216 composed semantic cases across four architectures and three formats
passed at this checkpoint, with no skips. These checks cover the shared rewrite
pipeline and do not extend the runtime-verified source profiles.

After incorporating the later C-output changes through `a5f9115d3`, the final
build repeated the 475 focused executions with the same 473 passes and two
Linux-only skips. All five incoming image-table and signed-comparison tests
also passed. The composed matrix above was run at the earlier checkpoint.
The subsequent integration through `1a131e7dd` repeated the same focused result
and both seven-scenario Linux exception runs. Its computed-goto regression
passed; the ARM32 literal-pool runtime test failed because the macOS linker
cannot link `armv4t`, another host-profile limitation.

The final CLI's Clang IR and GCC object replacements each passed seven Linux
C++ exception scenarios in Docker. The GCC input retains stack protection.
The PE FH3 runtime test passed its seven scenarios under Wine. These remain
emulated/runtime evidence with the platform limits above. The final shared
library's Python SDK run passed 110 of 165 tests, with 55 explicit skips. ABI
audit, mypy, formatting and documentation checks passed.

Two additional upstream source tests could not pass in this local profile:
`AArch64_HighCExecution.FMaxAndFMinPropagateANaN` needs a Clang supporting
`__builtin_elementwise_minimum/maximum`; the configured system Clang lacks
them. `HighCPointerAddresses.AnX87StoreWritesItsTenBytes` tries to compile x87
assembly with the ARM64 host compiler. These environment failures are recorded
separately and are not counted as passes.
