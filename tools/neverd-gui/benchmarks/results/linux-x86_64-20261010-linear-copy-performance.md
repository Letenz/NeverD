# Linear COPY proof performance results, 2026-10-10

Removing two artificial COPY-chain depth limits reduces median complete-source
latency by 23.8% for `0x309d0` and 68.8% for `0x38820` in the Rust EH corpus.
The latter also recovers 14 jump tables that the baseline withdraws. The largest
measured function, `0x19b24`, has no consistent improvement: its candidate
median is 3.8% slower. These results do not establish that NeverD surpasses IDA.

## Measurement identity and scope

The input is `rust_eh_probe-x86_64-unknown-linux-gnu-unwind-o2`, SHA-256
`ff7b02957d3c10e9e6d409c62312d0ddcc261a9f69e3807be089ec319a53e538`.
The baseline is `211476760dfae6a2e39d26ddde7860fb4899474e`. The candidate adds
the production COPY-loop change committed in
`c8aa0a57c8324853d9e5615f57606ecfebc78ba2`.

| Frozen Release artifact | SHA-256 |
|---|---|
| Baseline SDK | `40e22f989bad2e4d696a953e4ec9efb1152fa48bbf8ceb7ee0c633e3ba6a5d99` |
| Candidate SDK | `7a8e39a2e5f6be05f251624bc6e2c7ea98566540219aed07850015bc465fec64` |
| Common worker | `d42b2207685ea7e81774c157052df52dfdea37a5ea46b4295455727b10fa15bc` |

The [source benchmark](../pseudocode_latency_bench.py) makes three alternating
pairs per entry, each with a fresh worker and disposable input copy. OS caches
are warm. Both sides use four analysis threads and CPU affinity `0,2,4,6` on a
20-logical-CPU host. The interval covers the first source request and all
256-line pages, including IPC and text hashing, after opening and initial
function/assembly listing. Qt painting and scrolling are outside this interval.

Neither timed SDK contains temporary diagnostic patches or runs under GDB.
No builds, tests or other engine runs owned by this investigation overlap the
timing window. Other host activity remains present; recorded one-minute load
averages span 4.76–7.65. Three pairs do not establish statistical significance.
The paired SDKs predate the unrelated upstream merge described under validation.

## Complete-source latency

Values are milliseconds; brackets give the observed minimum and maximum.
Changes compare medians, with negative values meaning less latency.

| Entry | Baseline median [range] | Candidate median [range] | Change | Identical source? |
|---|---:|---:|---:|---|
| `0x19b24` | 22,889.1 [18,652.2–24,815.6] | 23,767.9 [17,617.7–25,063.1] | **+3.8%** | Yes, 4,637 lines |
| `0x309d0` | 7,412.5 [7,335.2–7,535.4] | 5,651.7 [5,419.8–5,998.4] | −23.8% | Yes, 3,072 lines |
| `0x38820` | 1,220.3 [1,207.9–1,280.4] | 381.1 [379.4–577.2] | −68.8% | **No: 63 → 990 lines** |
| `0x54174` | 6.877 [5.066–6.975] | 6.859 [5.132–7.053] | −0.3% | Yes, 20 lines |

All 24 worker requests succeed. Each engine's source hash is stable across
the three samples. Every candidate pair is faster for `0x309d0` and `0x38820`;
`0x19b24` has wide overlapping ranges, and the tiny `0x54174` difference is
noise. Source equality establishes reproducibility, not machine-code semantic
equivalence. The accompanying [data](linux-x86_64-20261010-linear-copy-performance.json)
retains all paired times, source hashes, RSS observations and artifact hashes.

## Confirmed cause and fix

The two-table recognizer scans candidate load addresses through their reaching
definitions. Its address and selected-base COPY walks formerly stopped after
16 steps and marked analysis incomplete. In `0x38820`, ordinary address chains
hit that limit with ample evidence budget remaining. The resulting incomplete
candidate invalidated the entire resolver stage, withdrawing tables that had
already been proved. Subsequent stages repeated the same failure.

The two affected walks now continue through the finite instruction prefix.
Every reaching-definition lookup starts strictly before the previous definition,
and both iteration and scan work debit the existing evidence budget. Recursive
expression, mask, selector and spill limits are unchanged. Address, target,
domain, storage-ownership and final replay proofs still gate publication.

Separate complete instrumented captures establish the changed work:

| Observation for `0x38820` | Baseline | Candidate |
|---|---:|---:|
| Resolver stages | 16 | 3 |
| Stage rollbacks | 16 | 0 |
| Reaching-value query batches | 3,652 | 532 |
| Final published instructions / blocks | 50 / 13 | 448 / 106 |
| Final jump tables | 0 | 14 |

The address-loop-only experiment exposed the second, selected-base COPY limit;
both walks must change to remove this failure. The captures report no dropped
events or sink failures. Their elapsed times are excluded from the latency
table. Restoring coverage explains the larger source output, so the `0x38820`
result is not an equal-output throughput comparison. This change reduces
repeated work without adding parallelism.

## Verification

The new structural regression fails on the exact baseline: no table is
recovered where one is expected. With the fix, all eight focused two-table
tests pass. The new cases cover 20 address COPY instructions and 20 selected-base
COPY instructions, eight exact targets and case labels, exhausted shared work
budgets, and a partial address clobber that must remain unresolved.

The execution regression decompiles both positive functions through HighC,
optimized LLVM-C and unoptimized LLVM-C. Each generated C program is compiled
at O0 and O2 with undefined-behavior traps and checked against an independent
selection formula for all 256 byte inputs and four selector inputs. Existing
selector-depth and physical-identity failure controls also pass.

After merging upstream through `4e66b2310578581fa842a6e3e6894a0a84493932`,
the final code at `ba17c726d472095c5129f7ffef580df966ab234b` was rebuilt in
Release and passed **1,049 tests, with zero failures, errors, disabled tests
or skips**: 417 jump-table, 24 no-return, 27 pipeline-outcome, 382 HighIR
control-flow and 199 x87/calling-convention tests. The SDK hash remained
unchanged throughout validation, and both x87-cache factory ABI overloads
remain exported. The full repository test aggregate was not run.

A fresh-worker smoke check of the merged SDK reproduces the candidate's full
source hashes for all four measured entries. It uses the common frozen
benchmark worker, so it does not validate unrelated upstream worker edits.
That smoke check overlaps regression validation and is used only for output
comparison; its durations are excluded from the performance results.

## Remaining work and limits

The large `0x19b24` case remains unresolved. Complete diagnostic captures of
helpers `0x2ebd0` and `0x21a50` show repeated mask-domain incompleteness and
stage rollback, with no reaching-value depth exhaustion. Relevant dispatches
combine `TZCNT` with a byte-width upper-bound guard. The shared guard
symbolizer does not model `POPCOUNT`/`LZCOUNT`; x86 lowering represents `TZCNT`
using `POPCOUNT`. A six-function assembly probe on the frozen baseline
diagnostic SDK isolates a guard-width sensitivity: `TZCNT`, `POPCNT` and
`LZCNT` each recover one four-slot table with a full 32-bit guard and no table
with a low-byte guard. All three counts are at most 32, so the compared values
agree for this input width. The byte-guard cases terminate after one stage,
without rollback; they do **not** reproduce the large-function 16-stage loop.
Adding an `AND 15` to each input in a second six-function probe leaves these
outcomes unchanged, so a preceding constant mask alone does not explain the loop.
Thus the local guard-width gap is confirmed, but its causal role in the Rust
rollback cycle and a shared range-fact fix still need validation. No bit-count
optimization is included in this change.

A separate early experiment that prescanned x87 graphs for unsupported
instructions changed the two large-function medians by only about 0.4%, with
identical source. It was reverted and is absent from this candidate.

An existing LLVM-C declaration failure also surfaced when unrelated positive
and negative fixtures shared one data-section ownership extent. The frozen
baseline reproduces it even after removing the long COPY chains. The new
execution fixtures use distinct owned storage sections; no LLVM backend change
is included here, and retained shared-storage declaration coverage remains a
separate issue.

This round does not remeasure IDA. The earlier
[IDA debugger investigation](linux-x86_64-20261010-ida-debugger-research.md)
distinguishes normal runtime from debugger-triggered verification and records
different exception-tail ownership. Its timings cannot be combined with these
pairs into a new cross-tool speedup claim.
