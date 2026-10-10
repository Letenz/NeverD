# Rust pseudocode performance, second pass — 2026-10-09

The largest measured Rust function now takes a median **25.84 s**, compared
with **38.75 s** in the matched control. The three large functions show
**33%, 39% and 47% less waiting**. The complete source bytes match in every
old/new pair and in the additional single-thread check. These measurements
isolate this optimization pass; subsequent `dev` integration is recorded
separately below. **Large-function decompilation remains slower than IDA.**

[Raw samples, hashes and phase traces](linux-x86_64-20261009-rust-pseudocode-round2.json)
identify implementation `c0bc08761d75c2ed748424e9adb09e97519d8d55` and control
`0853d2eec7e3b83b3cf214eb1527ab6168723095`. The control already includes the
previous affine-proof and bounded-parallelism improvements. Both engines use
the same worker and Release toolchain.

## Measurements

The input is `rust_eh_probe-x86_64-unknown-linux-gnu-unwind-o2`, SHA-256
`ff7b02957d3c10e9e6d409c62312d0ddcc261a9f69e3807be089ec319a53e538`.
The machine is an Intel Core i9-13900H with 20 logical CPUs, Linux x86-64,
Clang 23.1.2 and LLVM `neverd-llvm-v23.0.0-r4`; the worker uses GCC 15.2 and
Qt 6.8.3. Driver and workers inherit CPU affinity **0, 2, 4, 6**, one hardware
thread on each of four physical performance cores.

Each function starts a fresh worker and input copy. After the same open,
function-list and disassembly-listing requests, the timed operation fetches
the first 256 source lines and every remaining page. OS caches are warm. The
engines alternate order between repetitions. No builds or tests from this
task run during timing; other host activity is not stopped. Affinity does not
reserve cores, so these remain exploratory measurements, not an SLA or a
GUI frame-time measurement.

| Entry | Control, 4 threads | New, 4 threads | Less waiting |
|---|---:|---:|---:|
| `0x19b24` | 38.7518 s | 25.8378 s | 33.3% |
| `0x34901` | 25.4826 s | 15.4695 s | 39.3% |
| `0x309d0` | 11.2261 s | 5.9044 s | 47.4% |
| `0x54174` (`_init`) | 6.16 ms | 5.65 ms | Too small to infer a gain |

Values are medians of three runs. The largest function ranges from
37.86–43.09 s in the control and 25.77–28.77 s after the change. The other
new large-function ranges are 14.36–18.50 s and 5.86–6.28 s; every raw pair
is retained. An earlier unpinned run was interrupted during unrelated host
compilation and is excluded from these comparisons.

The additional one-thread checks take 27.90, 17.30, 7.71 s and 6.1 ms in
entry order. They are one sample per function for reproducibility and
scheduling diagnostics, not a three-sample parallel-scaling comparison.
The largest result still has 4,637 lines and 346,651 bytes, complete SHA-256
`4b8d043c3600224151b52b8065748fa7c0c8da06387b5c57d4a6a3c294a051eb`.
Source equality is a reproducibility check, not proof of binary semantics.

The largest function's median worker peak RSS increases from **166.8 MiB**
to **192.9 MiB**; the observed ranges are 166.7–186.0 and 185.3–196.4 MiB.
More concurrent callee bodies retain state at once. Concurrency remains
bounded to four builders and batches of eight bodies per worker. These are
single-worker measurements, not total memory for the GUI and its replicas.

## Changes and remaining bottleneck

- Compute the existing ordered-lookup evidence charge with `bit_width`,
  retaining exactly the same integer charge for empty, small and large sets.
- Use ordered insertion hints with an ordinary lookup fallback for duplicate
  and out-of-order proof points. Query-owned arenas allocate proof-index nodes
  together and release them after all referencing containers are destroyed.
- Cache immutable register lane metadata in 64 fixed entries per query. The
  complete register offset and width are checked; temporary values bypass the
  cache. Collisions evict entries. CFG/value proofs and mutable evidence are
  never stored in this cache.
- Let independent x64 callee batches with at least 4 KiB of metadata-estimated
  code use the existing worker pool, down from 16 KiB. Tiny batches stay
  serial. Discovery, budget admission and publication retain their original
  breadth-first order; each builder has its own decoder and CFG builder.

A separate two-repetition isolation check of the 16 KiB versus 4 KiB cutoff
reduces the largest function's median from 27.74 to 25.67 s (about 7.5%).
Those unpinned diagnostic samples are retained separately in the JSON and
are not pooled with the fixed-core table above. An experiment borrowing
operation pointers instead of keeping owned copies did not show a stable
benefit and was reverted.

Temporary profiling observed 13,047 proof-graph builds and 11,970 table-value
queries for the largest function. The cumulative graph/query times include
nested and concurrent work and must not be added as wall time. The final
uninstrumented run reduces median LowIR time from **33.036 to 20.536 s**,
but that is still about **79%** of the complete-source wait. Reducing repeated
proof/graph work further requires preserving snapshot identity, evidence
budgets and failure behavior.

IDA was not remeasured in this pass. The
[previous investigation](linux-x86_64-20261009-rust-pseudocode.md) records
2.8755 s for the same largest entry after IDA auto-analysis. That is a
historical comparison under different scheduling conditions; the remaining
gap is clear, but this pass does not establish a new controlled IDA ratio.

## Verification and integration

Three new register-cache tests cover high-byte and width identity, deliberate
collisions, temporary/non-register inputs, and shuffled cold/warm answers
across x64, x86, ARM and AArch64. The broader jump-table suite exercises
duplicate proof points, malformed targets, cycles and exhausted budgets.
The performance edits retain the original semantic decisions and rejection
paths.

After integration with the current `dev`, the jump-table suite passes
**393/393**, C API **99/99**, ABI **42/42**, source anchors **4/4**, and the
Qt offscreen GUI/worker CTest suite **17/17**. Source-dialect tests pass
**15**, with **2 optional cases skipped** because
`NEVERD_SOURCE_DIALECT_CORPUS` and `NEVERD_SOURCE_DIALECT_FILE` are unset.
The earlier 388/389 jump-table result included a pre-existing failure caused
by another function in an adversarial fixture; upstream now restricts that
test to its intended function through the production pipeline. It passes in
the final suite. No unsupported branch behavior was relaxed by this pass.

The integration checks and final build hashes are recorded in the companion
JSON. A final four-thread run after the shared-pipeline changes takes
**26.15, 15.09, 7.65 s and 7.9 ms** in entry order. All four complete hashes
still match. This is one integration check per entry, separate from the
matched three-pair medians above. The `0x309d0` check is slower than the earlier
optimized samples; upstream changes and host activity both differ, so this
single run cannot attribute that difference or establish a new median for the
merged engine.

A leftover merge separator in the upstream workbench test was removed
during integration. A pixel regression also exposed taller fallback-font
ascent when Chinese comments were added: keeping the row baseline fixed
preserves the code prefix's position. The regression checks both baseline
coordinates and rendered pixels.

A graph-navigation failure during the last integration run exposed a timing
bug: requesting another function changed the graph's function identity while
retaining the old nodes. A rapid return jump could appear satisfied by those
foreign nodes and then be overwritten by the pending layout. Switching
functions now retires those nodes and geometry immediately. A deterministic
test enters that loading interval without waiting, jumps back, and checks the
final function and exact instruction. It fails before the fix and passes
after it; the complete GUI suite also passes after the fix. These are automated
offscreen checks, not a new manual scrolling or displayed-frame benchmark.

## Reproduce

Build each revision with the same Release profile and retain its
`libneverd.so` separately. Use a worker with a compatible C ABI, and select
matching physical performance cores for the machine being tested:

```sh
taskset -c 0,2,4,6 python3 tools/neverd-gui/benchmarks/pseudocode_latency_bench.py \
  --worker /absolute/path/to/neverd-worker \
  --engine /absolute/path/to/new/libneverd.so \
  --baseline-engine /absolute/path/to/control/libneverd.so \
  --entry 0x19b24 --entry 0x34901 --entry 0x309d0 --entry 0x54174 \
  --threads 4 --samples 3 --timeout 180 \
  --output build-bench/rust-round2.json /absolute/path/to/rust-eh-fixture
```

Repeat without `--baseline-engine`, using `--threads 1 --samples 1`, for the
single-thread check. The harness records engine, worker and input hashes,
CPU affinity, host load, complete source hashes, timings, RSS and phase traces.
