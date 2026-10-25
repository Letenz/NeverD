# Advanced binary DIFF acceptance

This tracks the end-state requirements of
[#1462](https://github.com/NeverSight/NeverD/issues/1462), including the required
advanced areas. A working increment is not completion of the epic. “Complete”
in a comparison or bounded query describes that operation and its declared
profile; it is not a statement that all code, behaviors or security properties
were understood.

## Capability and evidence map

| Requirement | Implemented evidence | Remaining acceptance work |
| --- | --- | --- |
| Reproducible same-ISA native comparison | Shared `lib/diff`, input/analysis identities, strict saved binding, x86/x64 and AArch64 readers, ambiguous candidate inventories, original instruction deltas | Broader linked-application, real-world stripped and duplicate-heavy corpora; measured error rates |
| CLI and GUI workflow | C ABI and Qt-free worker; bounded pages, CFG/source inspection, edits and undo/redo, save/reopen, input/restore/query cancellation, NDDB project inputs | Continue scenario-based usability and large-input latency qualification; support advanced areas below through the same surfaces |
| A: transformations and regional matching | Native instruction-run and finite SSA region hypotheses; split/merge/clone/inline/outline/tail evidence; qualified loop progress profiles through v7 | Broad CFG/dataflow transformations and cross-module movement; independent compiler, LTO, PGO and obfuscation qualification; explicit qualified cross-ISA profiles |
| B: security-fix triage and review | Local change observations, native call-path context, bounded import-effect context, input-bound analyst cases/history and references | Ranked security-fix datasets, interprocedural parameter/return/alias effects, path feasibility, automated patch/backport/revert evidence and reviewer report qualification |
| C: optional semantic validation | Bounded pure scalar-return profile with explicit models, budgets, non-success outcomes and native provenance | Additional memory/call/loop profiles, precondition and coverage qualification; no general equivalence or vulnerability-elimination claim |
| D: version and branch lineage | Qualified offline snapshot graph, direct/indirect/parallel/cyclic associations, grouped conflicts, exact-byte reuse, report-local assessments, CLI/Python/GUI persistence and a reproducible synthetic scale harness | Separate release aliases/chronology, package-wide graphs, automated patch/backport/revert evidence and large real-corpus quality/latency; annotation application remains #99 |
| E: software/game packages | Native loaders, module resources and a separate qualified offline web-package behavior comparison exist | One package manifest workflow routing native/managed/metadata/script/config/resource changes, repacking distinctions, qualified Unity/Unreal/Godot adapters and unknown/encrypted coverage |
| F: competitive qualification | Owned native/compiler regressions and reproducible local receipts | Pinned BinDiff, Diaphora and Ghidra Version Tracking baselines; licensed ground truth, common-input and end-to-end tracks, held-out corpora, per-category quality/resource metrics, frozen numeric gates and statistical evidence |

Each advanced area remains required. An absent native finding is not evidence
that a patch is absent, and an analyst's patch-present assessment is not a
machine proof that a vulnerability has been eliminated. Ambiguous and unsupported
results must remain inspectable; adding profiles must not silently broaden the
meaning of an existing proof.

## Verification record boundaries

The v7 input-expression progress increment was locally qualified at
`7ae9cbf363e11944cb365cdfd49d50312014f067`: 1,427 selected CTests passed,
four optional/environment-dependent tests skipped, 527 compiler comparisons,
89 byte-exact historical report reopens, and Python/Qt integration checks.
Those are distinct types of checks and must not be summed into one benchmark.
The finite-domain oracle's proof calls are not native executions or performance
measurements.

The publication commit `ac381aaef710b436ecc10d27d9386b3c3fba8edb` also contains
concurrent upstream changes. Its exact-source local integration selected 2,521
CTests: initially 2,497 passed, one failed and 23 skipped. The failure was a
Windows HFA execution harness passing vector carriers to projected scalar
parameters. Fixture repair `35826752e1fec446c30e1d83b56301b58c8a24ed` uses the
actual projected argument types while retaining the independent byte oracle.
The repaired test passed, producing a composite 2,498 passed / 23 skipped
qualification; this was not a clean rerun of all 2,521 tests. Original failures
and repair receipts are retained separately.

Saved-decision consistency was subsequently qualified at
`dc6e83e8185931aa0d35935985480cdf7048aa7c`: all 300 selected CTests passed,
including the 527-comparison compiler fixture; 89 historical reports reopened
byte-exactly, and Python/Qt integration passed. These are local Release results
using the selected LLVM 23r6 prebuilt and Capstone
`88f3e3966565fb9383cd4fbb20c051c9aa22f309`, with Z3, emulation and embedded
Python plugins disabled. Native-backed Python bindings were tested separately.
Neither source-file equality nor cancelled CI runs qualify an entire merge.
Future receipts must name tested and delivered commits, dependency inputs,
build profile, selected tests and failures/skips separately.

See [the comparison profiles](binary-diff.md) for exact evidence meanings and
[the test guide](testing.md#structural-binary-comparison) for owning checks.
No comparative performance or “best in class” claim is established by the
current regressions.

The preceding fixed integration `183c8acd77b6cbece352b8c7b2dbb70fc97064b7`
selected 5,439 CTests: 5,376 passed and 63 skipped, with no failures. Its compiler,
historical-report, Python and Qt checks passed. The normal push delivered
`32a3acca81cd4850b0fbb50f5ae5f2369bbbfb5c` after additional concurrent upstream
merges; those later deltas need their own integration coverage.

The first complete lineage integration at
`f9b2ab69915218c25624262056141fcf43ac3665` selected 5,744 CTests: 5,638 passed,
four failed and 102 skipped. The failures exposed a source global-type lookup
that overlooked shared original-image storage, an appearance-test oracle that
misidentified antialiased text, and two growing x86 compilation/execution
matrices exceeding their 120-second per-test deadlines. The global-type repair
is covered by ELF/COFF/Mach-O unit cases and worker tests that compile both C
routes at O0/O2, change the pointer cell between calls, and preserve native IR.
The appearance oracle retains the actual layout tolerances. System-instruction
and descriptor matrices retain all original cases in separate tests.

The source-frozen rerun at `a90cd50056338b67938332b453c5b0b73a68ea1b`
selected 5,843 CTests: 5,734 passed, two failed and 107 skipped. All four earlier
failures were repaired; the remaining failures were two aggregate AMX matrices
exceeding the unchanged 120-second deadline. This run also passed the
527-comparison compiler fixture, 89 byte-exact historical report reopens,
native-backed Python checks, desktop comparison/appearance/lineage checks at
150% scaling, and lineage checks in private GNOME Wayland sessions at both
100% and 150%. Matrix splitting increases the CTest count without adding
semantic coverage; optional NeverC cases account for the additional skips.
Original failure and diagnostic receipts remain separate from subsequent
qualification. The AMX matrices were subsequently split by encoding, preserving
their original cases and the 120-second per-case deadline.

The preliminary delivery merge `4155ff7a180ff069918f48b431b0093047829f40`
also exposed a stale test caller of the shared stack-projection interface during
the full build. The fixture now supplies its matching LowIR source; the original
stack-operation assertions remain intact. Inspection of the newly integrated
byte-patch workflow then reproduced a data-only overlay reusing the unpatched
DIFF identity and collapsing two states into one lineage snapshot. SDK and
Python regressions first failed against that source. Repair
`5a9b57dffa5f2ce9455e4ead210671b4adc85e85` binds effective overlay bytes in the
shared native snapshot adapter, including bytes outside decoded functions.
Focused tests passed for data-only identity, refusal of an older saved binding,
sidecar replay, revert, and identical bytes with different edit kinds/row splits.
The source-frozen qualification of this repaired source selected 5,777 CTests:
5,707 passed, 70 skipped and none failed. A separate, disjoint selection of all
126 split AMX cases passed without skips. The compiler fixture's 527 comparisons
are included in the selected CTests, not additional CTest cases. All 89 historical
report reopens remained byte-exact, and the five Python groups passed 111 tests.
Comparison, appearance and lineage checks passed at 150% offscreen scaling;
native compositor gestures are explicitly skipped on that platform. Private
GNOME Wayland sessions at both 100% and 150% each passed all ten lineage cases,
including real resizing, restored geometry, replacement navigation and bounded
long-label rendering. Source hashes remained unchanged throughout qualification.

The same source passed three fresh-process scale samples at each of 1,000 and
10,000 functions per snapshot, with 16 snapshots and 19 source comparisons.
The larger workload retains 160,000 function nodes and 190,000 association edges.
The reusable harness verifies authored identities, complete graph counts,
first/middle/last-function paths and original edges, current cache budgets,
cancelled replacement and exact report export. Hashed inputs, native library,
bindings and harness, original failures, per-sample timings and process RSS are
retained in local qualification artifacts. These are synthetic scale checks;
they do not qualify real-corpus matching quality, desktop latency or competitive
superiority.

This qualification uses Release, LLVM 23r6, Capstone
`88f3e3966565fb9383cd4fbb20c051c9aa22f309` and signature export
`fe9a373c020fd48f68802353497bf8e315f7e178`. Z3, emulation and embedded Python
are disabled; native Python bindings are qualified separately. Skips include
optional NeverC, Windows/PDB and exception corpora, platform-specific native
execution and newer LLVM intrinsics. Subsequent upstream integration and the
delivered commit require separately identified coverage.
