# Reproducible function-matching diagnostics

[`scripts/benchmark_binary_diff.py`](../scripts/benchmark_binary_diff.py) builds
owned x64 ELF inputs and scores retained function associations against authored
identities. The three development fixtures cover stripped function reordering,
the same reordering with a changed bound, and duplicate local symbols in reversed
object order. They are small regression diagnostics. They do not establish
held-out quality, competitive speed or statistical superiority.

## Generate and run

Provide explicit compiler, ELF linker and strip executables. The generator
records their versions and hashes, all commands, original C, object files,
unstripped binaries and linker maps. The compiler targets x86-64 Linux at O2;
the binaries have no C runtime and are analyzed, never executed.

```bash
python3 scripts/benchmark_binary_diff.py generate \
  --clang /path/to/clang --linker /path/to/ld.lld --strip /path/to/llvm-strip \
  --output-dir benchmark-results/diff-corpus
python3 scripts/benchmark_binary_diff.py run \
  --corpus benchmark-results/diff-corpus/manifest.json \
  --neverd /path/to/frozen/bin/neverd \
  --library /path/to/frozen/bin/libneverd.so \
  --output-dir benchmark-results/diff-neverd
```

The runner currently requires Linux. It starts a fresh CLI for each case, selects
the explicit shared-library directory, records CLI/library hashes and version,
and retains stdout, stderr, exit status, report and evaluation. Only input ELF
paths are passed to the matcher. The separate `truth` directories and corpus
manifest are evaluator inputs. Freeze the executable and library throughout the
run. A CLI exit of 0 or 2 can produce a complete comparison; timeout, failure and
incomplete comparison are retained as failures. Output directories and result
files must be new, so a rerun cannot overwrite earlier evidence.

Wall time is a diagnostic observation of that individual CLI process. It includes
loading and analysis and does not provide peak RSS or an isolated performance
comparison. Shared builds or other system load invalidate competitive latency
interpretation. Do not compare it with a competitor's matching-only time.

## Evaluate another retained result

The evaluator accepts a versioned run receipt for one corpus case:

```json
{
  "schema": "neverd.benchmark.diff.run.v1",
  "case": "stripped-reorder",
  "format": "bindiff",
  "track": "end-to-end",
  "completed": true,
  "tool": {"name": "BinDiff", "version": "exact installed version and revision"},
  "input_sha256": ["primary SHA-256", "secondary SHA-256"],
  "result": {"path": "before_vs_after.BinDiff", "sha256": "result SHA-256"},
  "artifacts": [
    {"path": "invocation.json", "sha256": "invocation receipt SHA-256"}
  ]
}
```

Paths in a receipt are relative to that receipt; corpus input paths are relative
to its manifest. Record commands, configuration, exporter/disassembler versions,
dependencies, export databases, analysis logs and completion evidence in hashed
artifacts. `completed` is the producer's recorded outcome, not an inference from
the existence of a result file. Pin those inputs when creating the receipt; do
not reconstruct a stronger provenance claim than the original run retained.

```bash
python3 scripts/benchmark_binary_diff.py evaluate \
  --corpus benchmark-results/diff-corpus/manifest.json \
  --run benchmark-results/bindiff/run.json \
  --output benchmark-results/bindiff/evaluation.json
```

| Format | Scored associations | Input binding |
| --- | --- | --- |
| `neverd` | `matches` in a complete `neverd.bindiff` schema 3 report | Native ordered SHA-256 plus receipt |
| `bindiff` | SQLite `function` rows; orientation comes from `metadata.file1/file2` | Native SHA-1 or SHA-256 plus receipt SHA-256 |
| `diaphora` | SQLite `results` rows in `best` or `partial`; hex addresses | Receipt; retained export databases belong in its artifacts |
| `ghidra` | Accepted function associations in the `neverd.baseline.ghidra-vt-smoke.v1` JSON export | Receipt; retained import/VT invocation evidence belongs in its artifacts |

Diaphora's `unreliable` and `multimatch` rows and Ghidra's available candidates,
data matches and rejected associations are not promoted to accepted function
pairs. Repeated Ghidra correlator evidence is deduplicated by address pair.
Conflicting pairs remain visible and are scored; they never overwrite each other.
Ghidra addresses must use the `ram` space. Other spaces need a qualified adapter.
These readers match the retained BinDiff 8, Diaphora tag 3.5.0 (internal format
version 3.4), and Ghidra 12.1.3 diagnostic outputs. They do not launch or install
those products. The Ghidra input is the recorded export format, not an arbitrary
VT database. Automated competitor invocation/export remains separate work.

Use `end-to-end` when each product performs its own import and analysis. A
`common-input` receipt describes a separately prepared, qualified shared-input
experiment. Merely selecting that string does not make different disassemblers'
inventories equivalent. Never combine those tracks into one leaderboard.

## Truth and metrics

Each case supplies complete one-to-one function truth, with unique IDs and
addresses. A removed function has a null secondary address; an added function
has a null primary address. Matching either to an unrelated function is a false
positive. This evaluator does not cover incomplete truth, aliases at one
address, split/merge regions or semantic equivalence.

`correct` counts accepted pairs in the truth. `wrong` counts other accepted
pairs. `missed` counts truth pairs absent from accepted results. A wrong pair
can also miss a true pair, so wrong and missed are not disjoint function counts.
Precision is correct / accepted pairs; recall is correct / expected pairs.
A zero denominator produces null, not a perfect score. `exact_mapping` requires
both zero wrong and zero missed pairs. The evaluator returns 0 for an exact
mapping, 1 for a scored mismatch and 2 for invalid input or an incomplete run.
Raw addresses, denominators, repeated evidence count and source hashes remain
in the evaluation for inspection.

Run the independent metric and adapter regressions with:

```bash
python3 -m unittest scripts.tests.test_benchmark_binary_diff -v
```

Keep generated corpora and results in ignored benchmark directories. Expand
coverage with licensed, independently labeled and held-out applications before
setting competitive gates; see [the acceptance ledger](binary-diff-acceptance.md).
