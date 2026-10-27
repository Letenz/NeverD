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

## Run IDA, BinExport and BinDiff

[`scripts/benchmark_binary_diff_bindiff.py`](../scripts/benchmark_binary_diff_bindiff.py)
automates the Linux end-to-end track using existing, licensed IDA and explicitly
selected BinExport/BinDiff installations. It does not install products or change
their license configuration. Supply the complete BinDiff configuration file:

```bash
python3 scripts/benchmark_binary_diff_bindiff.py \
  --corpus benchmark-results/diff-corpus/manifest.json \
  --ida /path/to/idat --binexport-plugin /path/to/binexport_ida.so \
  --bindiff /path/to/bindiff --config /path/to/bindiff-config.json \
  --output-dir benchmark-results/diff-bindiff
```

Each case starts two fresh IDA databases, waits for automatic analysis, exports
their graphs and invokes BinDiff. The exporter binds its graph bytes to IDA's
native input SHA-256 and version. Admission also checks BinDiff's own ordered
input identities. A zero process exit alone cannot certify a completed export.
The runner retains the databases, graph exports, logs, commands, configuration,
copies of the selected binaries and scripts, hashes, stage outcomes and evaluation.
Later source edits do not invalidate those retained copies. Timeout or a failed
stage remains an incomplete run; the timeout terminates only that stage's owned
process group. Ctrl-C also stops that group and records cancellation before
exiting with code 130. The default per-stage deadline is 600 seconds and `--timeout`
overrides it. Existing output directories are refused.

Exit 0 means every case had an exact mapping, 1 means all pipelines completed
but at least one mapping differed, and 2 means invalid input or an incomplete
pipeline. A completed competitor run with wrong matches is still useful
evidence and must not be retried until its score improves. Stage times are
diagnostics, not a matching-only or peak-memory benchmark. Pin the surrounding
product installations and dependencies separately for release qualification;
the runner's hashes do not inventory every shared library or installed plugin.

## Run Ghidra Auto Version Tracking

[`scripts/benchmark_binary_diff_ghidra.py`](../scripts/benchmark_binary_diff_ghidra.py)
creates a new local project for every case and imports both inputs under distinct
names, including when their original basenames are identical:

```bash
python3 scripts/benchmark_binary_diff_ghidra.py \
  --corpus benchmark-results/diff-corpus/manifest.json \
  --ghidra /path/to/ghidra --java-home /path/to/jdk-21 \
  --output-dir benchmark-results/diff-ghidra
```

The runner checks an input-bound completion receipt after each import. A native
analysis timeout is rejected even when headless processing exits successfully.
It then invokes Ghidra's Auto Version Tracking task, observes its completion
status, saves the session and exports its associations. A partial session or a
successful export alone cannot complete the pipeline. The task's status retains
any markup-application warnings; scoring covers accepted function associations.
The qualified profile matches the bundled Ghidra 12.1.3 headless script defaults.
Its three overrides of task defaults are explicit in
[`RunNeverDVersionTracking.java`](../tools/neverd-bench/RunNeverDVersionTracking.java).
Other Ghidra releases need their own runtime/profile qualification.

`--cpus` defaults to 2, `--analysis-timeout` to 300 seconds per input, and
`--timeout` to 600 seconds per process. The retained project, copied input bytes,
analysis/task receipts, scripts, selected runtime files and native SHA-256
identities support inspection and independent re-evaluation. Installed Ghidra
extensions, preferences and the surrounding JDK remain part of the separately
pinned environment.

## Run IDA and Diaphora

[`scripts/benchmark_binary_diff_diaphora.py`](../scripts/benchmark_binary_diff_diaphora.py)
starts fresh IDA databases and invokes the selected Diaphora exporter and matcher
APIs with explicit completion checks:

```bash
python3 scripts/benchmark_binary_diff_diaphora.py \
  --corpus benchmark-results/diff-corpus/manifest.json \
  --ida /path/to/idat --diaphora /path/to/diaphora-source \
  --python /path/to/venv/bin/python \
  --python-path /path/to/venv/lib/python3.12/site-packages \
  --output-dir benchmark-results/diff-diaphora
```

The default profile includes library/thunk functions, decompiler output and
microcode. It requires an available licensed decompiler. Use `--no-decompiler
--no-microcode` for an explicitly different assembly-only profile, or
`--no-include-library-thunks` to exclude those functions. `--python-path` is
repeatable; select dependency directories compatible with IDA's embedded Python.
Inherited `DIAPHORA_*`, `PYTHONHOME` and `PYTHONPATH` settings do not silently
change the selected run. Other Diaphora options come from its archived source
configuration. `--cpus` defaults to 2 and `--timeout` to 600 seconds per process.

The runner executes a retained copy of the Diaphora source tree, including its
configuration, data and license notices. Both export receipts bind native IDA
SHA-256 identities to database bytes; admission also verifies the databases'
native MD5 and version fields. Matching must return success and preserve the
export bytes. Its receipt binds the result to those ordered exports, and the
result database's own configuration must agree. The `diaphora-native` evaluator
rechecks this entire chain without requiring the original absolute database
locations to remain live. The format version is recorded separately from the
source hash; Diaphora tag 3.5.0 uses internal format version 3.4.

Both runners use the same timeout/cancellation handling and exit-code meanings
as the BinDiff runner. Existing results are preserved, incomplete runs are not
scored, and completed mismatches are retained. These are Linux end-to-end
diagnostics; they do not qualify shared-input fairness or competitive timing.

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
| `diaphora-native` | Completed match receipt referencing SQLite `best`/`partial` rows and both exports | Native IDA SHA-256, native export MD5, ordered result configuration and retained export/result hashes |
| `diaphora` | SQLite `results` rows in `best` or `partial`; hex addresses | Receipt; retained export databases belong in its artifacts |
| `ghidra-vt` | Accepted function associations exported by `ExportNeverDVTMatches.java` | Native ordered SHA-256 plus receipt |
| `ghidra` | Accepted function associations in the `neverd.baseline.ghidra-vt-smoke.v1` JSON export | Receipt; retained import/VT invocation evidence belongs in its artifacts |

Diaphora's `unreliable` and `multimatch` rows and Ghidra's available candidates,
data matches and rejected associations are not promoted to accepted function
pairs. Repeated Ghidra correlator evidence is deduplicated by address pair.
Conflicting pairs remain visible and are scored; they never overwrite each other.
Ghidra addresses must use the `ram` space. Other spaces need a qualified adapter.
These readers match the retained BinDiff 8, Diaphora tag 3.5.0 (internal format
version 3.4), and Ghidra 12.1.3 diagnostic outputs. They do not launch or install
those products. The Ghidra input is the recorded export format, not an arbitrary
VT database. Use the provided
[`ExportNeverDVTMatches.java`](../tools/neverd-bench/ExportNeverDVTMatches.java)
inside an existing completed Ghidra VT project:

```bash
/path/to/ghidra/support/analyzeHeadless /path/to/projects project-name \
  -process after.elf -noanalysis \
  -scriptPath /path/to/NeverD/tools/neverd-bench \
  -postScript ExportNeverDVTMatches.java /session-name /path/to/new-matches.json
```

The script reads the existing association decisions and native program SHA-256
identities, checks cancellation, and requires a new output file. Select
`ghidra-vt` in its run receipt. It does not create a VT session or accept
candidates. The legacy `ghidra` reader remains available for earlier receipts
without native hashes. The automated runners above add fresh import, correlation
and completion checks; exporting an existing session does not establish those
earlier stages retroactively.

Use `end-to-end` when each product performs its own import and analysis. A
`common-input` receipt describes a separately prepared, qualified shared-input
experiment. Merely selecting that string does not make different disassemblers'
inventories equivalent. Never combine those tracks into one leaderboard.

## Truth and metrics

Each case supplies complete one-to-one function truth, with unique IDs and
addresses. A corpus must contain at least one case; case IDs must be unique
single directory names containing only ASCII letters, digits, dots, underscores
and hyphens, starting with a letter or digit. Invalid corpus declarations are
rejected before starting the runtime or creating output directories. A removed
function has a null secondary address; an added function
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
python3 -m unittest scripts.tests.test_benchmark_binary_diff \
  scripts.tests.test_benchmark_binary_diff_bindiff \
  scripts.tests.test_benchmark_binary_diff_external -v
```

Keep generated corpora and results in ignored benchmark directories. Expand
coverage with licensed, independently labeled and held-out applications before
setting competitive gates; see [the acceptance ledger](binary-diff-acceptance.md).
