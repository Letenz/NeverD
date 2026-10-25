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
| D: version and branch lineage | Pairwise reports and analyst history are reusable inputs | Version graph, conflicting direct/indirect paths, uncertainty, content-addressed reuse, timeline/assessment projections and CLI/GUI persistence; annotation application remains #99 |
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
concurrent upstream changes. Its subsequent local integration verification is
tracked separately; neither source-file equality nor cancelled CI runs qualify
that entire merge. Future receipts must name the tested commit, dependency pins,
build profile, selected tests, failures/skips and delivered commit separately.

See [the comparison profiles](binary-diff.md) for exact evidence meanings and
[the test guide](testing.md#structural-binary-comparison) for owning checks.
No comparative performance or “best in class” claim is established by the
current regressions.
