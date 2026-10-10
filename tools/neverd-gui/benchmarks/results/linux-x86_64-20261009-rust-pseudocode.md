# Rust pseudocode performance investigation — 2026-10-09

The slow Rust example now completes its largest measured function in **37.2 s**,
compared with **112.9 s** for the original engine control, with identical complete
source text. The measured improvement is **3.04×**. Browsing and independent
analysis views use separate workers. **Large-function decompilation remains
slower than IDA.**

[Raw samples, hashes and phase traces](linux-x86_64-20261009-rust-pseudocode.json) record implementation
`1fe9fd2a6757cd320837056f23fc7a5d671e5f2c` and baseline `b3a48893dc1d9363fa609cf0c8810842ab59be2f`.
The input is the repository's `rust_eh_probe-x86_64-unknown-linux-gnu-unwind-o2`,
SHA-256 `ff7b02957d3c10e9e6d409c62312d0ddcc261a9f69e3807be089ec319a53e538`.

## Measurements

Intel Core i9-13900H, 20 logical CPUs; Linux x86-64; Release engine built with
Clang 23.1.2 and LLVM `neverd-llvm-v23.0.0-r4`; GUI/worker built with GCC 15.2
and Qt 6.8.3. Each function uses a fresh process and input copy with warm OS
caches. No builds or tests from this task ran during measurement; unrelated
host activity was not stopped. The raw records include load averages.

The following are medians of three runs. NeverD fetches **all source pages**;
IDA runs fresh auto-analysis before timing decompilation and complete text.
Neither number measures GUI painting, and their output formats are not normalized.

| Entry | NeverD, 1 thread | NeverD, 4 threads | IDA decompile |
|---|---:|---:|---:|
| `0x19b24` | 38.6281 s | 37.1786 s | 2.8755 s |
| `0x34901` | 25.5266 s | 24.3943 s | 1.3723 s |
| `0x309d0` | 11.3873 s | 10.9177 s | 0.6986 s |
| `0x54174` | 0.0092 s | 0.0057 s | 0.0019 s |

The baseline is one additional run with all modified engine sources restored
to the original commit, using the same LLVM r4 toolchain. Its `0x19b24` result
has 4,637 lines and 346,651 bytes; the complete hash matches the new
engine. All four functions also have matching complete hashes across all six
current one-/four-thread runs per entry. This establishes reproducibility of
the tested text, not semantic equivalence to the input binary.

Four threads give approximately 1.04× speedup over the new single-thread engine
on these large functions. The larger gain comes from avoiding repeated proof
work. The small `_init` case is only a few milliseconds and is too small for a
useful parallel-scaling conclusion. Peak RSS for a measured four-thread worker
reaches 186.5 MiB; the original main-function control reached
151.7 MiB. These are single-process measurements, not total GUI memory.

## Changes and remaining bottleneck

- Independent GUI analysis views use up to two lazily started, read-only worker
  replicas. Function pages and graph transactions retain their dispatcher;
  project revision changes retire both. The total response-cache allowance
  stays fixed. The writable project worker remains available for browsing.
- Cyclic stack identities use a shared graph of checked affine equations.
  Conflicting anchors, nonzero cycles, unknown roots and intermediate overflow
  reject the proof. Incremental propagation avoids recursively rebuilding the
  same frame states; transparent value/memory cycle results also reuse a shared
  generation until new concrete evidence invalidates them.
- Independent callee CFGs run in batches of at most eight bodies and four
  decoders. Discovery, budget admission and result publication remain in the
  original breadth-first order. Small batches stay serial.

The largest function still spends a median **32.0 s** in LowIR, roughly
**86%** of source latency. Independent callee work is only part of that phase;
adding more workers alone cannot close the observed gap. Further work should
profile the remaining per-function value/memory proofs and their repeated
queries, retaining exact proof modes, evidence budgets and failure behavior.

## Correctness checks

The affine solver has 11 focused tests, including 512 seeded cyclic graphs
checked by an independent backward path-constraint oracle, overflow, rejected
unanchored cycles, exhausted budgets and linear evidence-growth cases. C API
97/97 and x64/i386 call-contract 42/42 tests passed. The broader jump-table run
passed 385/386; the remaining
`JumpTableProposalLFP.SizedAuthoritativeSelfDispatchStaysOpaqueBranch` failure
also occurs with the original engine. Its whole-fixture LLVM emission encounters
an ambiguous address in a separate `jt_lfp_relative_open_sibling` function.
It is recorded as a pre-existing failure, not counted as a pass.

GUI verification covers independent slow/fast views, isolated cancellation,
function-page affinity, interleaved external graph requests, project replacement
and a queued snapshot needed by an external analysis during view cancellation.
The final Qt offscreen GUI/worker CTest suite passed **16/16** after the
snapshot-cancellation fix. That regression failed before the fix and passed
after it; the production controller suite now passes 73 Qt test cases.

## Reproduce

Use [the fixed-entry benchmark](../pseudocode_latency_bench.py) and the command
in the [harness guide](../README.md#large-function-pseudocode-latency), with
`--samples 3` and separately `--threads 1` / `--threads 4`. Supply an activated
idalib interpreter with `--ida-python` for the IDA comparison. The executable
and engine SHA-256 values in the raw record identify the measured builds.
