# Guard-proof performance results, 2026-10-10

The combined candidate reduces the measured median source latency for
`0x19b24` and `0x34901` by 23.6% and 38.5%. It also recovers previously
withdrawn jump-table coverage in `0x38ef0`, while reducing that function's
latency. There is a regression: `0x309d0` takes 10.8% longer in the source
benchmark and 5.1% more process CPU in a separate SDK preparation experiment.
The internal cause of that regression remains unresolved. These results do
not establish that NeverD has surpassed IDA.

## Build and measurement identity

The input is the Rust EH x64 Linux unwind O2 corpus binary, SHA-256
`ff7b02957d3c10e9e6d409c62312d0ddcc261a9f69e3807be089ec319a53e538`.
The baseline is commit `882dc00bb83f282015831529844354107e12dc31`.
The candidates use that base with the recorded source patches:

| Frozen SDK | SHA-256 |
|---|---|
| Baseline | `6572c9f092306be7372c3c6fd912d6af6f2fbb13b05b58066c0ce1fb61048d6b` |
| x87 single-flight only | `cecc5902f14cc249512801c3f1d1e8768cdf95bc1aa3eefc6785242413698a7d` |
| Single-flight plus expanded guard proofs | `27b666af54dab1026f108adc710181c4ef4af8431dc1c1018b5c567494c3d596` |

The core uses Clang 23 Release; the standalone worker uses GCC 15. All runs
use the same worker binary, four analysis threads and CPU affinity `0,2,4,6`.
The host exposes 20 logical CPUs. Source-patch, worker, CLI and evidence-file
hashes are recorded in the accompanying
[numerical data](linux-x86_64-20261010-guard-proof-performance.json).

The [benchmark](../pseudocode_latency_bench.py) starts a fresh worker and input
copy for each entry and engine, with warm OS caches. It measures from the
first source request through all 256-line pages, including IPC, text encoding
and hashing. Startup, input opening, function listing and assembly listing
precede this interval. Qt painting and scrolling are not measured.

There are three alternating pairs per entry. The timed SDKs contain no
temporary diagnostic patches and run without GDB; both sides enable the same
ordinary native phase logging. Other host activity varied: the recorded
one-minute load averages before the combined pairs ranged from 3.87 to 9.46.
These samples do not establish statistical significance. Percent changes below
compare the two medians, rather than averaging per-pair speedups.

## Combined candidate: complete source latency

All values are milliseconds; brackets show the observed minimum and maximum.
A negative change means less latency.

| Entry | Baseline median [range] | Candidate median [range] | Median change | Source bytes identical? |
|---|---:|---:|---:|---|
| `0x19b24` | 27,251.6 [25,876.7–28,093.6] | 20,821.5 [17,974.7–23,319.9] | −23.6% | Yes |
| `0x34901` | 18,485.6 [17,429.2–18,542.4] | 11,362.0 [10,387.1–11,932.3] | −38.5% | Yes |
| `0x309d0` | 6,611.0 [6,536.9–7,254.3] | 7,327.9 [7,023.9–7,369.0] | **+10.8%** | Yes |
| `0x38ef0` | 4,753.3 [4,683.1–4,810.5] | 1,611.1 [1,579.2–2,253.4] | −66.1% | **No: coverage changes** |
| `0x54174` | 5.870 [5.288–7.190] | 5.273 [5.229–6.955] | −10.2% | Yes |

Every request succeeded. The source hash is stable across all three samples
of each engine and entry. Matching text is a reproducibility check, not a
proof of semantic equivalence to the machine code. The tiny `0x54174` result
changes by less than a millisecond and has overlapping ranges; its percentage
should not be generalized.

## Why `0x38ef0` changes output

The two guard-occurrence value queries now request the existing expanded
resolver depth of 128. The independent guard syntax limit remains 64, and
work budgets and rollback rules remain in force. This addresses a measured
failure of the old 64-level value reconstruction: guard syntax and value
history are different limits.

Separate instrumented probes found 59 incomplete query batches, each with a
real depth-65 rejection against the 64 limit. Evidence, match and symbolic
budgets were not exhausted. Exact recorded-state comparisons showed repeated
expansion and withdrawal through the 16-stage limit. The failures occurred in
reaching-value and memory reconstruction; these captures do not quantify how
much depth was CFG traversal versus expression nesting.

| Diagnostic observation | Depth 64 | Two query sites at depth 128 |
|---|---:|---:|
| Resolver stages | 16 | 4 |
| Candidate attempts | 136 | 28 |
| Stage rollbacks | 7 | 0 |
| Incomplete value-query batches | 59 | 0 |
| Final published instructions / blocks | 87 / 15 | 975 / 214 |
| Final jump tables / target slots | 0 / 0 | 9 / 40 |

The final recorded instruction graph, ordered targets and published addresses
at depth 128 match the control's transient expanded graph. The depth-64 stage and
depth-rejection captures were separate probes; the depth-128 probe collected both.
Their durations are not used in the performance table.

In the production source benchmark, output grows from 73 lines / 3,035 bytes
to 1,600 lines / 83,141 bytes. Thus the 66.1% reduction accompanies restored
analysis coverage; it is not an equal-output throughput comparison. A finite
limit of 128 is not claimed sufficient for arbitrary binaries, and the corpus
text alone is not a semantic execution oracle.

## Single-flight contribution

A separate earlier experiment measured only the x87 single-flight change.
It coalesces compatible concurrent requests for immutable graph facts; it does
not make different proof contexts interchangeable.

| Entry | Baseline median, ms | Single-flight median, ms | Median change |
|---|---:|---:|---:|
| `0x19b24` | 27,748.4 | 26,547.6 | −4.3% |
| `0x34901` | 17,683.6 | 17,783.2 | +0.6% |
| `0x309d0` | 7,113.5 | 6,712.6 | −5.6% |
| `0x54174` | 7.173 | 5.982 | −16.6% |

Each entry has three pairs with identical source hashes. The modest or mixed
results do not support attributing the combined gains to concurrency alone.
These are separate runs with different host load, so subtracting the two
experiments would not isolate the guard change's independent cost.

## Investigating the `0x309d0` regression

A separate ordinary SDK experiment used three alternating pairs with the same
frozen baseline and combined candidate. It timed `neverd_prepare_function`
after loading, excluding the worker prelude, IPC, source rendering and GUI.
Process CPU includes all threads in that process.

| Metric | Baseline samples, seconds | Candidate samples, seconds | Median change |
|---|---|---|---:|
| Process CPU | 7.4497, 7.8974, 7.1790 | 7.8978, 7.8294, 7.4907 | 7.4497 → 7.8294, +5.1% |
| Wall time | 6.2516, 6.5866, 5.9958 | 6.6082, 6.6129, 6.3328 | 6.2516 → 6.6082, +5.7% |

Further content and native-entry checks found:

- Public LowIR, MedIR and HighIR text exports are byte-identical: respectively
  395,291, 344,270 and 77,401 bytes. Their hashes are in the numerical data.
- Both GDB-counted executions enter CFG construction 31 times across 17 targets:
  one selected root, 14 register-summary builds and 16 x87 builds, with no
  no-return builds. x87 graph requests are 43 across 30 addresses on both sides.
- All recorded category/entry/count multisets agree. No target was added or
  removed, and no same-category target was built more than once in either run.

These checks exclude changed final public IR text and an expanded callee
construction target set in the observed executions. They do not establish
identical internal summaries, query counts, proof iterations, allocation costs
or pass work. Extra callee register summaries are not exported by those IR
APIs. GDB also changes scheduling and cache behavior. These diagnostic runs
were permitted to overlap test compilation; neither their elapsed times nor
phase-log durations contribute to the timing results above. The combined
candidate contains both changes, so this evidence does not isolate which
change causes the extra cost.

## IDA reference scope

The paired runner also measures local IDA/Hex-Rays 9.4 using
`str(ida_hexrays.decompile(entry))` after separately timed database opening and
automatic analysis. For `0x19b24`, that post-analysis interval has a median of
2,982.0 ms in the combined experiment. Across all five entries (15 IDA runs), database opening and automatic
analysis together range from 2,923.9 to 4,337.5 ms. These intervals differ
from the NeverD worker request and are not an end-to-end GUI comparison.

Coverage differs too: the earlier database inspection found that IDA's
`0x19b24` function owns 14,470 bytes, whereas the ELF symbol spans 15,330 bytes.
The final 860 bytes contain exception cleanup and `_Unwind_Resume` and were not
owned by an IDA function in that database. This is not sufficient to rank
overall semantic correctness. The separate
[debugger investigation](linux-x86_64-20261010-ida-debugger-research.md) also
identified debugger-gated verification work, so its GDB durations and stack
percentages are not normal-runtime performance measurements. No cross-tool
speedup ratio or claim of surpassing IDA follows from these results.

## Verification status and evidence

The focused guard regression was verified red/green against the two query-site
changes. With the original depth arguments,
`JTE_X86_64.GuardEvidenceSeparatesValueHistoryFromSyntaxDepth` failed because
zero tables were recovered where one was expected. With the change, both it
and `ResolverValueQueryCache.ExpandedValueDepthKeepsTheDefaultGuardCap` passed.
The latter also passed on the control. It checks that the 80-step predecessor
fixture succeeds only with the expanded limit, the 160-step fixture still
rejects the query, and a cached expanded result cannot satisfy a later
default-depth request. These are specific finite-depth cases, not a rule
about all functions of those sizes.

After merging upstream `e548f07d8`, code commit `96e8a4beac0eb838a2975409cb1ade4f879acbb2` was rebuilt
in Release and passed **1,047 tests, zero failures and zero skips**: jump tables
415, no-return 24, pipeline outcomes 27, high control flow 382, and the three
isolated x87/calling-convention translation units 199. The SDK hash stayed
unchanged throughout execution. Both the original no-argument C++ graph-cache
factory and the new overload are exported.

The resulting SDK SHA-256 is `40e22f989bad2e4d696a953e4ec9efb1152fa48bbf8ceb7ee0c633e3ba6a5d99`.
One fresh worker request for each of the five corpus entries also succeeded;
every complete source hash matches the frozen combined candidate. This is
integration and output verification, not another paired performance estimate.
The timing tables above remain measurements of the explicitly frozen
882dc00-based runtimes, not of this merged commit. These affected suites do not
certify the whole repository, every optional profile or Qt painting.

The [JSON companion](linux-x86_64-20261010-guard-proof-performance.json) contains
all timing samples, source hashes, compact diagnostic counts and evidence-file
hashes. Raw captures and frozen runtimes remain local ignored artifacts under
`build/perf-round5/`; they are not distributed in the repository. This report
and its JSON contain no host-specific absolute paths, IDA binary data or IDA
disassembly.
