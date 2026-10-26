# Patch entrypoint measurements

`patch-entrypoints.py` compares the shared SDK's JSON listing, save and reload
with complete `neverd patch-bytes` commands. Use Release binaries from the same
compiler, feature configuration and dependency snapshots. Keep each variant's
CLI and SDK in a separate directory; the driver verifies the CLI's resolved SDK
on Linux before measuring it.

```sh
TMPDIR=/path/on/the/same/filesystem python3 tools/neverd-bench/patch-entrypoints.py \
  --library /snapshot/bin/libneverd.so --cli /snapshot/bin/neverd \
  --rows 64 8192 65536 --samples 15 \
  --output benchmark-results/patch-entrypoints.json
```

Run baseline/candidate/candidate/baseline sequentially, without overlapping
your own builds or tests. Retain source revisions, dependency manifests and
build flags beside the reports. The driver records binary and script hashes,
host load, every sample, medians and p95. Keep machine-local reports in ignored
benchmark directories; commit reusable tools and regression tests.

The owned raw x64 fixtures contain adjacent one-byte rows of alternating kinds.
No guest instructions execute. Every CLI sample starts a new process and
includes loading, command execution and capturing its output. The filesystem
and executable pages are warm; this is not a cold-disk benchmark. CLI apply
really changes a byte every time. Restoration, validation of every saved row,
export and immutable-input checks happen outside the timer. Large terminal
rendering costs are not included.

SDK calls run inside CPython with its host allocator. The CLI uses the allocator
selected by its own build. SDK CPU samples use the Python process clock; CLI
CPU samples use the waited child's user and system CPU usage. Compare each
entrypoint against itself. Omitting `--cli` measures only the SDK. Use
`patch-scaling.py` separately for complete worker commits, including
undo history and journal publication.

Streaming patch output preserves compact protocol JSON and the two-space,
sorted-key, newline-terminated sidecar format. The byte-patch unit tests check
literal output, replay and mixed-row admission. Python integration checks CLI
and SDK text against an independent JSON encoder and injects a failed POSIX
file write to verify that previous saved bytes survive and a later retry works.
Run the worker patch/history suites as well when qualifying this shared path.

This benchmark does not measure assembly, source compilation, decompilation,
analysis, GUI painting or the input loader's performance on large real images.
Patch loading still validates complete files, and GUI history still stores
complete before/after states. Faster output alone is not evidence that those
other boundaries are optimal.
