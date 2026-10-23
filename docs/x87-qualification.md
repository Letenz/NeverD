# Scoped x87 execution qualification

This record addresses issue [#1677](https://github.com/NeverSight/NeverD/issues/1677)
for the explicitly admitted profiles in [x87 semantics](x87-semantics.md).
Instruction recognition, numerical behavior, state observations, native execution,
translation and formal equivalence remain separate claims. The shared
[capability inventory](x87-capabilities.md) describes routes and prerequisites;
it does not execute a host probe or turn regression sources into proof.

**Candidate status:** native ARM64 generated execution has passed, but native
translation, combined cross-host arithmetic and ARM64 measurements remain pending.
Issue #1677 remains open. The current candidate must pass those gates before this
record can claim closure.

## Profiles and limits

| Interface | Admitted scope | Explicit limits |
| --- | --- | --- |
| Native typed projection | i386 and x64 System V; selected x64 Microsoft typed cases; raw binary80 values, validated stack/call contracts and defined status | Complete architectural state remains unknown; typed BCD/pair guards trap outside their domain; processor-dependent results belong to the measured CPU |
| Native owned context | x64 authenticated mutable state, native instruction/value/state replay, selected Linux fault boundaries | No arbitrary CPU/OS qualification; relocated/undefined fields and signal resumption are excluded |
| Portable concrete | `intel64-architectural-masked-v1`, x64 owned state, explicitly listed operations and control/input domains | Unsupported nonfinite arithmetic, stack faults, unmasked data operations, MMX and general environment effects refuse; no native fallback |
| Portable LLVM/HighC/LLVM-C | Explicit C++ preparation and emission into Step V1; x64 owned integer source ABI; Linux little-endian x64/AArch64 | Source calls/floating ABIs, ordinary guest memory and x87 integer-flag forms are refused; ordinary decompilation does not select this interface |
| Portable translation | Explicit C++ profile and authenticated logical extension; x64 to native Linux AArch64 ELF, checked memory/objects/credentials | Legacy C/Python/CLI translation object APIs stay scalar-only; no i386 translation, non-Linux runtime, floating ABI, MMX/general environment, arbitrary applications or termination proof |
| Symbolic/refinement | Conservative unknown for the unqualified x87 state/formal model | A native, differential or digest result cannot become a checked equivalence result |

## Recorded execution

The [machine-readable ledger](x87-qualification.json) binds each recorded result
to its source commit,
CPU, compiler/runtime configuration, record hash and retained artifacts. Passing
records check inputs again after execution. Any changed source, generated binary,
backend, runtime or assumption requires new applicable evidence. Historical
records are labelled by their actual tested commit; they are not automatically
proof for a later checkout.

The completed generated-only run is
[40016086999](https://github.com/NeverSight/NeverD/actions/runs/40016086999), at
`d3d429208c8c5f3ff6e616c15a3dfade4868b468`: both native hosts passed and their
6,096 observations agree. Local native replay at
`62158ce9e40f30b9ba21f6aabfa122aeeb3fb14a` passed 112 tests with 1,568 retained
executables. These historical records identify their actual source versions.
They do not establish the pending native translation result for this candidate.

Native translation qualification initially exposed an ELF metadata liveness bug:
LLVM removed four audited symbol-free metadata blocks before the sealed audit.
The correction retains those blocks with private zero-width live symbols and
keeps the original content, cardinality and relocation audits. A real LLVM
pruning regression reproduces the old failure at O0/O2 on any host. Native
execution after that correction is still required.

The native differential layer retains original/generated C, LLVM/object files,
executables, commands, output, raw binary80 values and defined state. It covers
separate typed/owned and i386/x64 contracts, long-double calls and returns,
branches/backedges, BCD and stack management, selected transcendental/remainder
behavior, pending exceptions, environment transitions, MMX and Linux fault
boundaries. Undefined packed-BCD digits and out-of-profile typed pair behavior
remain refusals instead of convenient native answers. Processor-dependent
results are scoped to the recorded physical CPU.

The portable generated layer has 36 artifacts: three routes, three authored
body shapes, default/NoOpt source pipelines and host O0/O2. Each host records
6,096 observations, including all eight TOP values, all three valid PC settings,
all four RC settings, refusal/guard paths, raw memory and provenance. Native x64
adds 5,760 original-instruction comparisons. Cross-host comparison uses complete
file manifests and matches portable observations separately from native-only
reference fields.

The translation gate requires all thirteen tests on native Linux AArch64 without
skips. Its exhaustive fixture is designed to retain 192 TOP/PC/RC/O0/O2
observations and the
1,152 actual objects given to JITLink, full logical state after each block and
agreement with production sessions. The candidate result is pending. Separate
tests cover ten-byte memory faults,
self-modification, forged/stale invocation bindings, prefix retention, mixed
unsupported programs, branches/backedges and checkpoint/resume. Passing these
checks qualifies
this bounded profile; object compilation on x64 alone does not supply this result.

Three fixed arithmetic seeds (`0x871677`, `0x871678`, `0x871679`) produce 57,888
observations per host. The local x64 recording passed; the combined native
ARM64 comparison is pending. Raw result bits and defined IE/DE/ZE/OE/UE/PE/C1 bits
are compared with original x64 instructions. Additional local replay covers native
i386/x64 GCC/Clang at O0/O2. Fixed boundary fixtures, LLVM APFloat and the pinned
Unicorn/Haswell reference remain distinct references with the limitations listed
in the semantic contract; missing Unicorn status fields are not counted as
exception evidence.

## Measurements and authorized replay

The completed local protocols retain thirty paired native workload configurations
and one hundred portable execution/transport trials at `62158ce9e`. ARM64 session
measurements remain pending. Record hashes and configurations are in the ledger;
machine-local timings and memory measurements remain in ignored artifacts, as
required by the repository contribution policy.

The portable protocol reports exact execution, every-32nd-call qNaN refusal,
raw 80-bit transport and explicit logical snapshots separately. AArch64 adds
whole-session create/compile/link/run/snapshot/unload cost. Default sampling is
five trials pinned to one allowed CPU, with CW=0x037f and TOP=5. Counts and final
raw state must match the frozen records, including failures and refusals. There
is no fallback dispatch: availability is false and observed fallback calls are
zero. VmHWM is whole-process peak resident memory, including libraries/fixtures.

Native workload replay runs an authored 2,048-sample FSIN/status stream and a
raw-transport baseline across five mode/ABI/state combinations and three output
routes. Original/generated trials alternate order. This is a deterministic,
authorized batch workload; no imported third-party application is executed.
Neither layer claims a speed improvement, all-control-word performance, isolated
allocator cost or arbitrary application compatibility. No reduced-precision
execution mode is offered or used to satisfy an exact-mode criterion.

## Acceptance mapping

This table maps implementation and required evidence; the pending gates above
prevent treating it as a completed acceptance checklist.

| Phase | Evidence and boundary |
| --- | --- |
| X0 | One instruction/form inventory, all mode/ABI/state/consumer/backend/destination dimensions, shared runtime instruction gate, malformed/negative matrix tests, and regenerated documentation |
| X1 | Exact finite binary80 integer arithmetic, all valid PC/RC, raw transport/provenance and transactional state tests, independent references, adversarial/random observations and actual native ARM execution |
| X2 | Low/Med/LLVM/HighC/LLVM-C routes; separate native i386/x64 typed/owned ABI tests; environment/MMX synchronization; explicit ARM profile; C/CLI/Python/worker/GUI report and refusal parity |
| X3 | Typed stack/BCD/pair differences reconciled within guarded contracts; selected-CPU transcendental/remainder and fault/environment replay; unsupported portable effects and formal unknown retained |
| X4 | Frozen artifacts/configurations/seeds, raw/defined state and memory comparisons, all admitted destination hosts, separate exact/native-reference measurements, refusal/fallback and memory counts, deterministic authored replay |

This mapping uses the issue's named, narrow profiles and non-goals. It does not
assert support for every x87 instruction, CPU generation, ABI, OS exception model
or arbitrary program, or complete architectural/formal equivalence.

## Reproduction and retention

Use the commands in [testing](testing.md), the strict recorders under `scripts/`,
and the manually dispatched `x87-portable-generated.yml` workflow. Builds use
`scripts/build.py` with actual verified NeverSight fork snapshots. Linux AArch64
builds the pinned repository LLVM revision from source because the pinned
prebuilt package set has no ARM archive. Each record retains the actual compiler
version and hashes, rather than assuming that a major-version label identifies
one compiler binary.

CI artifacts retain success and failure evidence for thirty days. Download and
preserve them before expiry when long-term binary replay is required. A small
committed ledger preserves hashes, scope and run links; it is not a substitute
for the retained artifacts or a cryptographic correctness proof. Local records
remain in the original verification workspace. Unavailable hosts/tools and
skipped tests are explicitly distinguished from passes in the ledger.
