# IDA and NeverD: debugger investigation, 2026-10-10

The main measured difference is the amount of LowIR work before the selected
function is ready. It is not evidence that more threads alone will close the
gap. This investigation uses the local IDA/Hex-Rays 9.4 installation and the
Rust EH x64 O2 corpus input, SHA-256
`ff7b02957d3c10e9e6d409c62312d0ddcc261a9f69e3807be089ec319a53e538`.
NeverD's control starts at `882dc00bb83f282015831529844354107e12dc31`.

## Normal execution and debugger effects

Six IDA experiments completed 37 decompile calls and seven independent
microcode requests. Fresh decompilation plus pseudocode generation after
automatic analysis took 3.049–3.263 seconds for entry `0x19b24`. Opening the
database and automatic analysis were separate costs: approximately 1.3–1.6 and
2.3–2.9 seconds in the normal controls. This is not an end-to-end GUI timing.

Normal lightweight hooks recorded cumulative boundaries of 41 ms for initial
microcode, 147 ms for local optimization, 653 ms for call analysis, 1,248 ms for
global optimization, 1,288 ms for structural analysis and 3,113 ms for the
returned C tree. Pseudocode generation finished around 3,140 ms; copying that
text into Python took another 3.8 ms. Ten C-tree maturity cycles occurred.
These are event boundaries, not exclusive pass CPU totals.

GDB sampling changed the workload: the same hooked decompile-and-pseudocode
request took 8.780 seconds.
ELF relocations and local disassembly confirmed a verification subtree gated by
the imported `under_debugger` variable; 88 of 290 captured stacks belonged to
that subtree. The installed Python API documentation independently describes
checks enabled by a debugger. No debugger flag, validation switch or IDA code
was patched. These stack percentages must not be presented as normal runtime
CPU shares. `perf` was unavailable under the host's existing permissions.

## Work scope

In one scoped NeverD SDK prepare, native entry breakpoints counted 237 full CFG
builds across 91 addresses: one selected root, 135 x87 builds, 88 register-summary
builds and 13 no-return builds. These count entries into the full builder
pipeline, not 237 results with every semantic proof complete. A later ordinary
instrumented run reproduced those category counts. All 88 register-summary
targets were also built for x87, with different no-return depth and x87-fixup
contexts; address identity alone does not authorize graph reuse. Only three expensive
concurrent followers had exactly identical cacheable construction contexts in
that ordinary run.

IDA emitted one observed root microcode generation. Native decoder breakpoints
recorded 17,341 calls across 2,976 addresses, only 51 outside its root function.
These are different APIs and cannot be divided into a speedup ratio. Together
with their call stacks, they show substantially different preparation scopes.

Coverage also differs. IDA's analyzed root owns 14,470 bytes and 2,925 decoded
instructions. The ELF symbol spans 15,330 bytes; its final 860 bytes contain
exception cleanup and `_Unwind_Resume` and were not owned by an IDA function in
this database. This is not sufficient to rank overall semantic correctness, and
those paths must not be discarded to improve a timing result. Warm cfunc API
lookup also excludes GUI, IPC and text copying and is not comparable to a
NeverD GUI page request.

## A concrete repeated-proof failure

The helper at `0x38ef0` repeatedly expands to nine jump tables, then fails the
last candidate's guard proof and withdraws the stage. Exact typed comparisons
of the recorded graph, proposal, provenance and safety-marker state show the
same two states repeating through the 16-stage limit. The
expanded state contains 975 published instructions and 214 blocks; withdrawal
leaves 87 published instructions and 15 blocks. Decoded bytes remain cached.

Narrow depth instrumentation identified 59 incomplete value-query batches.
Every affected batch had a real depth-65 rejection against limit 64; the
candidate work budget, local match budget and symbolic budget were not
exhausted. The failure sites were reaching-value and memory reconstruction.
This evidence does not distinguish every predecessor step from genuine
expression nesting in the failing walks.

An isolated experiment changed only the two guard-occurrence query limits to
128, already used for expanded CFG value reconstruction elsewhere. It retained
all work budgets and the separate 64-level guard syntax limits. The helper
converged in four stages, with no rollback or incomplete batch, and published
all nine tables. Its recorded instruction graph, ordered 40 target slots and
975 published addresses exactly matched the control's previously transient expanded state.
This is a proof-bound experiment, not a production latency measurement; 128 is
still finite and is not claimed sufficient for arbitrary inputs.

## Direction

Prioritize eliminating redundant proof retries, requesting only necessary
callee facts, and sharing immutable facts under exact construction contexts.
Apply parallel execution to independent work after defining ownership and
cancellation. Reusing a graph before/after x87 fixup requires an explicit shared
representation; the fixup changes operations during block rebuilding, so it is
not a cosmetic final pass that can simply be ignored.

Raw local debugger captures, module hashes, invocation scripts and the Chinese
investigation report are preserved in `build/ida-performance-research/`.
Ordinary context, stage and depth evidence is in `build/perf-round5/`; those
ignored directories include instrumented source snapshots, reversible patches,
frozen SDKs and hashes. Production performance claims must come from a separate
production paired benchmark, with source and semantic checks.

The accompanying [compact evidence data](linux-x86_64-20261010-ida-debugger-research.json)
records the quoted counts, timing boundaries, engine hashes and source-evidence
hashes without redistributing IDA code or disassembly. The local raw directories
are ignored build artifacts and are not included in the repository.
