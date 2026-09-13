# Sibling Audit And Validation

## 5. Audit Sibling Surfaces

Expand from the root abstraction, not from superficial text similarity.

### Control-flow surfaces

- call block;
- dominating predecessor;
- loop header and backedge;
- direct and indirect calls; and
- optimized block layouts.

### Register and architecture surfaces

- full register and subregister aliases;
- AArch64 and ARM32;
- x86-64 and x86 when the target-register API exposes the same concept; and
- argument, return, frame, and temporary values where the shared helper applies.

### Binary-format surfaces

- Mach-O;
- ELF; and
- PE/COFF.

Do not assume a bug is format-specific because the reporter supplied one format.
If the conflicting choice occurs in shared MedIR or ABI recovery, build an
equivalent fixture for the other supported formats. Conversely, do not claim an
exact cross-format reproducer unless the same intermediate shape was observed.

## 6. Validate in Layers

Run validation in increasing cost order:

1. policy-level RED tests;
2. the exact reporter fixture;
3. reversed-order and unsafe-wide counterexamples;
4. relevant architecture variants;
5. Mach-O, ELF, and PE/COFF fixtures when the boundary is shared;
6. the owning semantic and pointer-provenance suites;
7. patch or recompilation execution when affected; and
8. the full relevant test binary or CI profile.

For a patchable native fixture, test the produced binary repeatedly under its
normal address-randomization behavior when practical. A successful lift alone
does not prove that a recovered pointer remains valid after recompilation.

Report exact pass, failure, and skip counts. A skip is not coverage. If platform
CI is still running, say so explicitly rather than reporting it as passed.
When a nearby suite already contains fail-closed skips, compare the patched
tree with a clean build of the current base commit before calling them a
regression. Expand by shared semantic owner and invariant, not merely by a
similar diagnostic string; two fallback paths can emit the same wording after
different specialized owners decline them.

### Choose the build profile by the evidence needed

Release is the default for broad NeverD semantic validation, not a universal
replacement for Debug. NeverD's Debug configuration is intentionally
unoptimized, and its decode/lift path is substantially slower; the repository
CI runs the normal cross-platform suite in Release.

A focused Debug green is diagnostic evidence, not closure. After it passes,
repeat the exact linked fixture and the full owning regression binaries from a
freshly rebuilt Release target. Use both a shared synthetic MedIR matrix and
real linked fixtures when loader metadata matters: the former proves one
format-neutral policy, while Mach-O, ELF, and PE/COFF fixtures prove that each
loader supplies the required permissions and relocation provenance. A writable
section containing pointer relocation slots can still belong to the generic
pointer mirror; do not classify ownership from the writable flag alone.

| Purpose | Preferred profile |
|---|---|
| Focused RED test, assertion failure, source stepping | Debug |
| Broad `NeverDSemanticTests`, lift/recompile, or end-to-end regression | Release |
| Optimized-path diagnosis that still needs symbols | RelWithDebInfo |
| Sanitizer run | Dedicated Debug/sanitizer build |

For a broad semantic run, use a dedicated, known-good Release tree and rebuild
the owning target from the current sources before executing it:

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build-release --target NeverDSemanticTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDSemanticTests$' --output-on-failure --parallel 4
```

Do not infer the profile from a directory name. Check `CMAKE_BUILD_TYPE` in its
`CMakeCache.txt`, and ensure the requested target actually rebuilt. Reconfigure
after adding a test source so an old executable cannot silently omit it. If a
reused build tree has inconsistent Ninja/CMake state or unexpectedly starts an
unrelated full rebuild, stop using it as validation evidence and configure a
separate clean tree instead of trying to salvage the authoritative run in
place. Inspect the CMake target and CTest inventory before selecting a test:
specialized probe suites may be standalone executables rather than part of
`NeverDSemanticTests`, and a `ctest -R` run that reports zero tests is no
evidence. When NeverD and its LLVM fork evolve together, prefer the integrated
fork build for authoritative validation; a cached prebuilt package is usable
only after its API revision is proven compatible with the checked-out NeverD
source.

A long run counts only when the process exits normally and prints its final
summary. Do not attach a debugger to the only authoritative suite process to
investigate a long tail: attaching can suspend it, and terminating the debugger
can terminate or invalidate the test run. Reproduce the suspected case in a
separate filtered process. Report an interrupted, timed-out, killed, or
debugger-disturbed run as incomplete even when no failure appeared before the
interruption.

### Keep the validation toolchain matched to NeverD LLVM

Do not use an arbitrary system Clang as the sole oracle for LLVM IR emitted by
NeverD. The repository LLVM fork can carry a different canonical intrinsic
signature from the host toolchain; a host parser error around an overloaded
intrinsic can therefore be version skew rather than invalid NeverD output.
Re-run object emission with the repository's LLVM/Codegen path and use that
result for the relink/runtime check.

Do not rename a canonical LLVM operation to a private `__neverd_*` pseudo
intrinsic merely to make the host compiler accept it. If the required intrinsic
is genuinely absent from the project toolchain, add the real intrinsic to the
LLVM fork and keep neverc's corresponding surface synchronized. Record host
toolchain incompatibility separately from semantic test failures.

## 7. Classify the Change Honestly

Use evidence from history and the failing path:

- **New regression:** the faulty behavior was introduced by a recent change.
- **Latent defect exposed by hardening:** the inconsistent rule predates the
  diagnostic, but a newer verifier stopped unsafe output.
- **Incomplete earlier fix:** history corrected one implementation of the rule
  but left an equivalent caller unchanged.

Do not blame the hardening check merely because it is where the pipeline now
stops. A conservative failure can be the correct exposure of an older bug.
