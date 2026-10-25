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
| D: version and branch lineage | Offline snapshot graph, direct/indirect/parallel/cyclic associations, grouped conflicts, exact-byte reuse, report-local assessments, CLI/Python/GUI persistence | Integrated qualification of the new profile; separate release aliases/chronology, package-wide graphs, automated patch/backport/revert evidence and large-corpus quality/latency; annotation application remains #99 |
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

The lineage implementation is undergoing integrated qualification. Isolated core,
C ABI, Python and real worker/CLI results are development evidence, not a claim
that the complete desktop/product build or a final delivery merge is qualified.
The preceding fixed integration `183c8acd77b6cbece352b8c7b2dbb70fc97064b7`
selected 5,439 CTests: 5,376 passed and 63 skipped, with no failures. Its compiler,
historical-report, Python and Qt checks passed. The normal push delivered
`32a3acca81cd4850b0fbb50f5ae5f2369bbbfb5c` after additional concurrent upstream
merges; those later deltas need their own integration coverage.
