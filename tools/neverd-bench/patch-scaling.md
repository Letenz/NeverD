# Interactive patch scaling

`patch-scaling.py` measures established byte overlays through the public Python
SDK and the real worker. Fixtures are disposable raw x64 images; no guest code
is executed. Each case checks the returned data, exported bytes and unchanged
input file. Failed checks invalidate the run. Run without Python `-O`.

```sh
python3 tools/neverd-bench/patch-scaling.py \
  --library /snapshot/bin/libneverd.so \
  --worker /snapshot/bin/neverd-worker \
  --rows 64 1024 2048 --worker-rows 64 1024 2048 \
  --range-bytes 4096 1048576 --samples 21 \
  --output build-bench/patch-scaling.json
```

Use matching Release SDK/worker snapshots and the same driver for each variant.
The Linux worker benchmark pins `LD_LIBRARY_PATH` and verifies the loaded SDK
through `/proc/<pid>/maps`, so a saved executable's build RUNPATH cannot silently
select another library. Omit `--worker` for SDK-only measurements. Set `TMPDIR`
to the same filesystem for both variants; durable commit latency depends on it.
Run variants sequentially in baseline/candidate/candidate/baseline order. Prefer
an idle host; preserve load and raw samples when shared work prevents that.

The three operations have distinct boundaries:

- `sdk_single_byte_edit`: replace one byte within alternating `bytes`/`nop`
  canonical rows. This includes the Python binding, planning, stale-byte check,
  overlay mutation and SDK invalidation. Reset/load and export verification are
  outside the timed region. Both wall and process CPU time are retained.
- `worker_patch_ranges`: request `{ranges_only:true}` for one contiguous patch.
  Timing includes IPC; returned address and size must be exact. It does not time
  Qt painting or analysis.
- `worker_single_byte_commit`: commit one byte through the writer, including
  history and sidecar persistence. Undo restores the fixture outside timing;
  export checks the restored overlay after the sequence.

An established overlay excludes the first immutable input snapshot. These
fixtures do not measure instruction assembly, source compilation, reanalysis,
cold caches, peak memory or other platforms. SDK row publication and durable
history/sidecar snapshots still scale with total overlay state.

## 2026-10-25 review

Mode: **PR Review**, sampled patch edit, persistence and color-refresh paths.

| Symptom | Source | Consequence | Remedy |
|---|---|---|---|
| A local edit repeatedly visits adjacent runs of different kinds | `PatchOverlay::apply` expands its window through unrelated neighbors and repeatedly searches the touched list | A 2,048-row fixture spends over half a second editing one byte | Locate the write with `lower_bound`; visit overlaps and matching-kind neighbors at its original edges once |
| A small range response encodes every original/current byte | The worker obtains full SDK rows before discarding byte payloads | A 1 MiB patch makes a color-range query expensive | Add an owned-string C/Python range API; retain the full-row fallback for older SDKs |
| Persisting a live edit replays the identical overlay and serializes it for comparisons | SDK load and worker acknowledgement both compare full JSON | Extra copying, validation and hex encoding on every durable edit | Parse the sidecar, compare typed rows with the already-admitted live overlay, and acknowledge exact equality; otherwise retain full replacement admission |
| Existing patch colors disappear during an unrelated rename | `Session::refreshBytePatches` clears before its asynchronous read | Transient incorrect colors and redundant repaint signals | Keep the previous same-project ranges until replacement; emit only on change, retaining epoch/revision fences and project-unload clearing |

The load shortcut compares complete parsed rows, not just a saved digest: an
unsaved live edit must still be replaced by the saved overlay. The regression
explicitly saves one value, changes it in memory, then reloads the unchanged
sidecar. Core tests independently check byte values, kind boundaries, overlapping
edits, partial restoration and the 65,536-row limit. Worker tests cover real and
mock SDKs, replicas, stale previews, undo/redo, reopen and exclusive export. GUI
tests keep colors through rename and clear them across project switches; both
X11 and Wayland exercise the full patch-key suite.

### Measurements

Baseline: PR #1763's merged tree, represented by commit
`640418249fe0684ed1a2a9e216603bf6296e18e6`. Candidate: the same source plus these
patch changes, before later `dev` integration. Linux x86-64, i9-13900H, Release,
Clang 23, identical dependency snapshots. Each result below pools 42 samples
from the four sequential runs. Other users' builds were present; no own build
or runtime tests overlapped this paired campaign. See the
[raw evidence](../../docs/evidence/patch-scaling-2026-10-25.json) for artifact and
driver hashes, build options, CPU times, p95, loads and every sample.

| Operation | Before median | After median | Median speedup |
|---|---:|---:|---:|
| SDK one-byte edit, 64 adjacent rows | 0.283 ms | 0.130 ms | 2.2× |
| SDK one-byte edit, 1,024 adjacent rows | 74.587 ms | 0.265 ms | 281× |
| SDK one-byte edit, 2,048 adjacent rows | 581.523 ms | 0.269 ms | 2,166× |
| Worker ranges, 4 KiB changed bytes | 0.412 ms | 0.387 ms | 1.1× |
| Worker ranges, 1 MiB changed bytes | 93.240 ms | 0.109 ms | 853× |
| Worker durable edit, 64 adjacent rows | 68.506 ms | 64.823 ms | 1.1× |
| Worker durable edit, 1,024 adjacent rows | 261.701 ms | 132.440 ms | 2.0× |
| Worker durable edit, 2,048 adjacent rows | 949.861 ms | 202.778 ms | 4.7× |

The large SDK ratio addresses a pathological mixed-kind adjacency chain; it is
not the speedup for every patch. At 2,048 rows, process CPU medians are
523.046 ms → 0.262 ms, supporting an algorithmic improvement despite host load.
The candidate-only row-limit run completed with correct exported bytes at
65,536 rows (1.844 ms wall median, seven diagnostic samples); no old-version
timing at that size is claimed. Small durable edits remain dominated by shared
host and persistence costs, and no significant improvement is claimed for them.

These are NeverD before/after measurements. They do not rank IDA, Binary Ninja,
Ghidra or other products. Native Windows/macOS input and performance remain
unmeasured by this campaign.
